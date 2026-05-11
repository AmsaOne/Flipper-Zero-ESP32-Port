#include "spectrogram_app.h"
#include "spectrogram_render.h"
#include "spectrogram_worker.h"
#include "spectrogram_freqs.h"

#include <furi.h>
#include <furi_hal_display.h>
#include <furi_hal_spi_bus.h>
#include <gui/gui.h>
#include <input/input.h>
#include <esp_lcd_panel_ops.h>
#include <esp_heap_caps.h>

#define TAG "SpectrogramApp"

#define SPECTROGRAM_DEFAULT_START_HZ 433000000U
#define SPECTROGRAM_DEFAULT_END_HZ 435000000U

static void spectrogram_input_callback(const void* value, void* ctx) {
    SpectrogramApp* app = ctx;
    const InputEvent* event = value;

    if(event->type != InputTypeShort && event->type != InputTypeLong &&
       event->type != InputTypeRepeat) {
        return;
    }

    furi_mutex_acquire(app->mutex, FuriWaitForever);

    if(event->key == InputKeyBack) {
        app->stop = true;
        furi_mutex_release(app->mutex);
        return;
    }

    if(event->key == InputKeyOk) {
        if(event->type == InputTypeShort) {
            app->selected = (app->selected == SpectrogramFieldStart) ? SpectrogramFieldEnd :
                                                                       SpectrogramFieldStart;
            app->footer_redraw_due = true;
        }
        furi_mutex_release(app->mutex);
        return;
    }

    int delta = 0;
    if(event->key == InputKeyUp || event->key == InputKeyRight) delta = +1;
    else if(event->key == InputKeyDown || event->key == InputKeyLeft) delta = -1;

    if(delta != 0) {
        size_t cur_band = spectrogram_freq_band(app->f_start_hz);
        uint32_t* target = (app->selected == SpectrogramFieldStart) ? &app->f_start_hz :
                                                                       &app->f_end_hz;
        size_t idx = spectrogram_freq_nearest_index(*target);
        ssize_t ni = (ssize_t)idx + delta;

        /* Walk the preset table in the delta direction. Prefer to stay inside
         * the current band, but if we run off the band edge, jump to the
         * adjacent band and snap both Start and End there. */
        bool placed = false;
        while(ni >= 0 && ni < (ssize_t)SPECTROGRAM_FREQ_PRESET_COUNT) {
            uint32_t cand = spectrogram_freq_presets_hz[ni];
            size_t cand_band = spectrogram_freq_band(cand);

            if(cand_band == cur_band) {
                /* In-band: ensure ordering still holds (start < end) */
                if(app->selected == SpectrogramFieldStart && cand >= app->f_end_hz) {
                    ni += delta;
                    continue;
                }
                if(app->selected == SpectrogramFieldEnd && cand <= app->f_start_hz) {
                    ni += delta;
                    continue;
                }
                *target = cand;
                placed = true;
                break;
            }

            /* Crossed into adjacent band: snap both ends into the new band.
             * Pick the lowest preset of the new band for start, the highest
             * (or two steps up) for end. Walking-direction matters: if delta
             * < 0 we landed on the highest preset of a lower band; pick that
             * as End and find the lowest preset of the same band for Start. */
            if(delta > 0) {
                /* Use cand as Start; find Start's band's last preset as End */
                uint32_t new_start = cand;
                uint32_t new_end = cand;
                for(size_t j = ni + 1; j < SPECTROGRAM_FREQ_PRESET_COUNT; j++) {
                    if(spectrogram_freq_band(spectrogram_freq_presets_hz[j]) != cand_band) break;
                    new_end = spectrogram_freq_presets_hz[j];
                }
                if(new_end == new_start) {
                    /* Only one preset in this band — keep walking */
                    ni += delta;
                    continue;
                }
                app->f_start_hz = new_start;
                app->f_end_hz = new_end;
            } else {
                /* delta < 0: cand is the highest preset of the lower band */
                uint32_t new_end = cand;
                uint32_t new_start = cand;
                for(ssize_t j = ni - 1; j >= 0; j--) {
                    if(spectrogram_freq_band(spectrogram_freq_presets_hz[j]) != cand_band) break;
                    new_start = spectrogram_freq_presets_hz[j];
                }
                if(new_start == new_end) {
                    ni += delta;
                    continue;
                }
                app->f_start_hz = new_start;
                app->f_end_hz = new_end;
            }
            placed = true;
            break;
        }
        if(placed) app->params_dirty = true;
        app->header_redraw_due = true;
        app->footer_redraw_due = true;
    }

    furi_mutex_release(app->mutex);
}

static SpectrogramApp* spectrogram_app_alloc(void) {
    SpectrogramApp* app = malloc(sizeof(SpectrogramApp));
    memset(app, 0, sizeof(*app));

    app->gui = furi_record_open(RECORD_GUI);
    app->input = furi_record_open(RECORD_INPUT_EVENTS);
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    app->canvas = gui_direct_draw_acquire(app->gui);

    app->panel = furi_hal_display_get_panel_handle();
    app->panel_w = furi_hal_display_get_h_res();
    app->panel_h = furi_hal_display_get_v_res();

    app->band_top = SPECTROGRAM_HEADER_H;
    app->band_bottom = (uint16_t)(app->panel_h - SPECTROGRAM_FOOTER_H);

    app->f_start_hz = SPECTROGRAM_DEFAULT_START_HZ;
    app->f_end_hz = SPECTROGRAM_DEFAULT_END_HZ;
    app->selected = SpectrogramFieldStart;
    app->max_rssi = -127.0f;
    app->max_freq_hz = 0;
    app->header_redraw_due = true;
    app->footer_redraw_due = true;

    app->input_sub = furi_pubsub_subscribe(app->input, spectrogram_input_callback, app);
    app->worker = spectrogram_worker_alloc(app);
    return app;
}

static void spectrogram_app_free(SpectrogramApp* app) {
    spectrogram_worker_free(app->worker);
    furi_pubsub_unsubscribe(app->input, app->input_sub);
    gui_direct_draw_release(app->gui);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_INPUT_EVENTS);
    furi_record_close(RECORD_GUI);
    free(app);
}

static void spectrogram_clear_screen(SpectrogramApp* app) {
    const uint16_t W = app->panel_w;
    const uint16_t STRIPE_H = 16;
    size_t bytes = (size_t)W * STRIPE_H * sizeof(uint16_t);
    uint16_t* black = heap_caps_malloc(bytes, MALLOC_CAP_DMA);
    if(!black) return;
    memset(black, 0, bytes);
    furi_hal_spi_bus_lock();
    for(uint16_t y = 0; y < app->panel_h; y += STRIPE_H) {
        uint16_t rows = (uint16_t)((y + STRIPE_H > app->panel_h) ? (app->panel_h - y) : STRIPE_H);
        esp_lcd_panel_draw_bitmap(app->panel, 0, y, W, y + rows, black);
    }
    furi_hal_spi_bus_unlock();
    heap_caps_free(black);
}

int32_t spectrogram_app(void* p) {
    UNUSED(p);

    SpectrogramApp* app = spectrogram_app_alloc();

    /* DMA-capable strip buffer reused for header + footer composition. Size it
     * for the larger of the two so we only allocate once. */
    uint16_t strip_h = (SPECTROGRAM_HEADER_H > SPECTROGRAM_FOOTER_H) ? SPECTROGRAM_HEADER_H :
                                                                       SPECTROGRAM_FOOTER_H;
    size_t strip_bytes = (size_t)app->panel_w * strip_h * sizeof(uint16_t);
    uint16_t* strip = heap_caps_malloc(strip_bytes, MALLOC_CAP_DMA);

    spectrogram_clear_screen(app);

    if(strip) {
        spectrogram_draw_header(app, strip);
        spectrogram_draw_footer(app, strip);
    }

    spectrogram_worker_start(app->worker);

    bool stop = false;
    while(!stop) {
        bool draw_header = false, draw_footer = false;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        stop = app->stop;
        if(app->header_redraw_due) {
            app->header_redraw_due = false;
            draw_header = true;
        }
        if(app->footer_redraw_due) {
            app->footer_redraw_due = false;
            draw_footer = true;
        }
        furi_mutex_release(app->mutex);

        if(strip) {
            if(draw_header) spectrogram_draw_header(app, strip);
            if(draw_footer) spectrogram_draw_footer(app, strip);
        }
        furi_delay_ms(50);
    }

    spectrogram_worker_stop(app->worker);
    if(strip) heap_caps_free(strip);
    spectrogram_app_free(app);
    return 0;
}
