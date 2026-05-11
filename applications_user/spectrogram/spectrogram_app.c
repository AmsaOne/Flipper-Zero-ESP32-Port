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

#define SPECTROGRAM_DEFAULT_START_HZ 378000000U  /* 433 full band start */
#define SPECTROGRAM_DEFAULT_END_HZ   481000000U  /* 433 full band end   */

/* Full-band boundaries matching furi_hal_subghz_is_frequency_valid ranges */
static const uint32_t band_lo[] = { 0U,         281000000U, 378000000U, 749000000U };
static const uint32_t band_hi[] = { 0U,         361000000U, 481000000U, 962000000U };
static const char* const band_names[] = { "CUSTOM", "315 MHz", "433 MHz", "868 MHz" };

static void spectrogram_apply_band(SpectrogramApp* app) {
    if(app->band_mode == SpectrogramBandCustom) {
        app->f_start_hz = app->custom_start_hz;
        app->f_end_hz   = app->custom_end_hz;
    } else {
        app->f_start_hz = band_lo[app->band_mode];
        app->f_end_hz   = band_hi[app->band_mode];
    }
    app->params_dirty      = true;
    app->header_redraw_due = true;
    app->footer_redraw_due = true;
}

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
        if(event->type == InputTypeLong) {
            /* Long-press OK: cycle band mode Custom->315->433->868->Custom */
            if(app->band_mode == SpectrogramBandCustom) {
                app->custom_start_hz = app->f_start_hz;
                app->custom_end_hz   = app->f_end_hz;
            }
            app->band_mode = (SpectrogramBandMode)((app->band_mode + 1) % SpectrogramBandCount);
            spectrogram_apply_band(app);
        } else if(event->type == InputTypeShort) {
            if(app->band_mode == SpectrogramBandCustom) {
                /* Custom mode: toggle which end is selected for tuning */
                app->selected = (app->selected == SpectrogramFieldStart) ?
                    SpectrogramFieldEnd : SpectrogramFieldStart;
            } else {
                /* Full-band mode: toggle waterfall / bar chart */
                app->display_mode = (app->display_mode == SpectrogramDisplayWaterfall) ?
                    SpectrogramDisplayBars : SpectrogramDisplayWaterfall;
                app->params_dirty = true;
            }
            app->footer_redraw_due = true;
        }
        furi_mutex_release(app->mutex);
        return;
    }

    /* Rotation only applies in custom mode */
    if(app->band_mode != SpectrogramBandCustom) {
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

        bool placed = false;
        while(ni >= 0 && ni < (ssize_t)SPECTROGRAM_FREQ_PRESET_COUNT) {
            uint32_t cand = spectrogram_freq_presets_hz[ni];
            size_t cand_band = spectrogram_freq_band(cand);

            if(cand_band == cur_band) {
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

            if(delta > 0) {
                uint32_t new_start = cand;
                uint32_t new_end = cand;
                for(size_t j = ni + 1; j < SPECTROGRAM_FREQ_PRESET_COUNT; j++) {
                    if(spectrogram_freq_band(spectrogram_freq_presets_hz[j]) != cand_band) break;
                    new_end = spectrogram_freq_presets_hz[j];
                }
                if(new_end == new_start) { ni += delta; continue; }
                app->f_start_hz = new_start;
                app->f_end_hz = new_end;
            } else {
                uint32_t new_end = cand;
                uint32_t new_start = cand;
                for(ssize_t j = ni - 1; j >= 0; j--) {
                    if(spectrogram_freq_band(spectrogram_freq_presets_hz[j]) != cand_band) break;
                    new_start = spectrogram_freq_presets_hz[j];
                }
                if(new_start == new_end) { ni += delta; continue; }
                app->f_start_hz = new_start;
                app->f_end_hz = new_end;
            }
            placed = true;
            break;
        }
        if(placed) {
            app->custom_start_hz = app->f_start_hz;
            app->custom_end_hz   = app->f_end_hz;
            app->params_dirty = true;
        }
        app->header_redraw_due = true;
        app->footer_redraw_due = true;
    }

    furi_mutex_release(app->mutex);
}

static SpectrogramApp* spectrogram_app_alloc(void) {
    SpectrogramApp* app = malloc(sizeof(SpectrogramApp));
    memset(app, 0, sizeof(*app));

    app->gui   = furi_record_open(RECORD_GUI);
    app->input = furi_record_open(RECORD_INPUT_EVENTS);
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    app->canvas = gui_direct_draw_acquire(app->gui);

    app->panel   = furi_hal_display_get_panel_handle();
    app->panel_w = furi_hal_display_get_h_res();
    app->panel_h = furi_hal_display_get_v_res();

    app->band_top    = SPECTROGRAM_HEADER_H;
    app->band_bottom = (uint16_t)(app->panel_h - SPECTROGRAM_FOOTER_H);

    /* Start in full 433 MHz band waterfall - most useful default view */
    app->band_mode    = SpectrogramBand433;
    app->display_mode = SpectrogramDisplayWaterfall;
    app->f_start_hz   = SPECTROGRAM_DEFAULT_START_HZ;
    app->f_end_hz     = SPECTROGRAM_DEFAULT_END_HZ;
    /* Custom range initialized to a useful 433 MHz preset slice */
    app->custom_start_hz = 433075000U;
    app->custom_end_hz   = 434177000U;

    app->selected  = SpectrogramFieldStart;
    app->max_rssi  = -127.0f;
    app->max_freq_hz = 0;
    app->header_redraw_due = true;
    app->footer_redraw_due = true;

    app->input_sub = furi_pubsub_subscribe(app->input, spectrogram_input_callback, app);
    app->worker    = spectrogram_worker_alloc(app);
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

/* Expose band_names for the render layer */
const char* spectrogram_band_name(SpectrogramBandMode m) {
    if((size_t)m < SpectrogramBandCount) return band_names[m];
    return "?";
}

int32_t spectrogram_app(void* p) {
    UNUSED(p);

    SpectrogramApp* app = spectrogram_app_alloc();

    uint16_t strip_h = (SPECTROGRAM_HEADER_H > SPECTROGRAM_FOOTER_H) ?
        SPECTROGRAM_HEADER_H : SPECTROGRAM_FOOTER_H;
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
        if(app->header_redraw_due) { app->header_redraw_due = false; draw_header = true; }
        if(app->footer_redraw_due) { app->footer_redraw_due = false; draw_footer = true; }
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
