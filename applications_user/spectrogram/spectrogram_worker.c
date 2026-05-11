#include "spectrogram_worker.h"
#include "spectrogram_render.h"
#include "spectrogram_freqs.h"

#include <furi.h>
#include <furi_hal_spi_bus.h>
#include <esp_heap_caps.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/preset.h>

#define TAG "Spectrogram"

#define SPECTROGRAM_DWELL_MS      1U
#define SPECTROGRAM_BAR_DECAY     0.85f   /* multiplied per sweep when signal drops */
#define SPECTROGRAM_ANTENNA_MS    10U     /* RF path settling after a band change */

#define SPECTROGRAM_MAX_REDRAW_INTERVAL_MS  1000U
#define SPECTROGRAM_LIVE_REDRAW_INTERVAL_MS  200U

/* OOK 650 kHz preset, MCSM0=0x08 (FS_AUTOCAL disabled for fast frequency
 * stepping without per-pixel VCO calibration overhead). */
static const uint8_t spectrogram_preset_regs[] = {
    0x02, 0x0D,
    0x03, 0x07,
    0x08, 0x32,
    0x0B, 0x06,
    0x14, 0x00,
    0x13, 0x00,
    0x12, 0x30,
    0x11, 0x32,
    0x10, 0x17,
    0x18, 0x08,
    0x19, 0x18,
    0x1D, 0x91,
    0x1C, 0x00,
    0x1B, 0x07,
    0x20, 0xFB,
    0x22, 0x11,
    0x21, 0xB6,
    0x00, 0x00,
    0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

struct SpectrogramWorker {
    FuriThread* thread;
    SpectrogramApp* app;
    volatile bool running;
};

static void publish_status(SpectrogramApp* app, SpectrogramStatus s) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->status = s;
    app->footer_redraw_due = true;
    furi_mutex_release(app->mutex);
}

static void spectrogram_clear_band(SpectrogramApp* app, uint16_t* line) {
    const uint16_t W = app->panel_w;
    memset(line, 0, (size_t)W * sizeof(uint16_t));
    furi_hal_spi_bus_lock();
    for(uint16_t cy = app->band_top; cy < app->band_bottom; cy++) {
        esp_lcd_panel_draw_bitmap(app->panel, 0, cy, W, cy + 1, line);
    }
    furi_hal_spi_bus_unlock();
}

/* Draw a full bar-chart frame using the persistent bars[] array.
 * bars[col] is 0.0–1.0 representing normalized RSSI level.
 * Bars grow from the bottom; color follows the RSSI ramp so bar tips are
 * red (strong) and the baseline is blue. */
static void spectrogram_draw_bars(SpectrogramApp* app, uint16_t* line, const float* bars) {
    const uint16_t W = app->panel_w;
    const uint16_t bar_h = app->band_bottom - app->band_top;
    furi_hal_spi_bus_lock();
    for(uint16_t row = 0; row < bar_h; row++) {
        /* threshold decreases from 1.0 at top to ~0 at bottom */
        float threshold = 1.0f - (float)row / (float)bar_h;
        float rssi_at_row = SPECTROGRAM_RSSI_MIN +
            threshold * (SPECTROGRAM_RSSI_MAX - SPECTROGRAM_RSSI_MIN);
        uint16_t filled_color = spectrogram_color_for_rssi(rssi_at_row);
        for(uint16_t col = 0; col < W; col++) {
            line[col] = (bars[col] >= threshold) ? filled_color : 0;
        }
        uint16_t y = app->band_top + row;
        esp_lcd_panel_draw_bitmap(app->panel, 0, y, W, y + 1, line);
    }
    furi_hal_spi_bus_unlock();
}

static int32_t spectrogram_worker_thread(void* context) {
    SpectrogramWorker* w = context;
    SpectrogramApp* app = w->app;

    publish_status(app, SpectrogramStatusStarting);

    subghz_devices_init();

    const SubGhzDevice* device = subghz_devices_get_by_name("cc1101_int");
    if(!device) {
        FURI_LOG_E(TAG, "cc1101_int not found");
        publish_status(app, SpectrogramStatusErrDevice);
        subghz_devices_deinit();
        return -1;
    }

    subghz_devices_reset(device);
    subghz_devices_idle(device);
    subghz_devices_load_preset(device, FuriHalSubGhzPresetCustom, (uint8_t*)spectrogram_preset_regs);

    const uint16_t W = app->panel_w;
    uint16_t* line = heap_caps_malloc((size_t)W * sizeof(uint16_t), MALLOC_CAP_DMA);
    if(!line) {
        FURI_LOG_E(TAG, "DMA alloc failed");
        publish_status(app, SpectrogramStatusErrAlloc);
        subghz_devices_sleep(device);
        subghz_devices_end(device);
        subghz_devices_deinit();
        return -1;
    }

    /* Persistent bar levels for bar-chart mode — zero-init, kept across sweeps */
    float* bars = calloc(W, sizeof(float));

    publish_status(app, SpectrogramStatusRunning);

    uint16_t y = app->band_top;
    float row_max_rssi = -127.0f;
    uint32_t row_max_freq = 0;
    uint32_t last_max_publish_ms = 0;
    uint32_t last_live_publish_ms = 0;
    uint32_t last_band_center = 0; /* detect RF path changes for antenna settling */

    while(w->running) {
        uint32_t f_start, f_end;
        SpectrogramDisplayMode display_mode;
        bool do_clear = false;

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        f_start      = app->f_start_hz;
        f_end        = app->f_end_hz;
        display_mode = app->display_mode;
        if(app->params_dirty) {
            app->params_dirty = false;
            row_max_rssi = -127.0f;
            row_max_freq = 0;
            app->max_rssi    = -127.0f;
            app->max_freq_hz = 0;
            app->max_redraw_due    = true;
            app->header_redraw_due = true;
            app->footer_redraw_due = true;
            y = app->band_top;
            do_clear = true;
        }
        furi_mutex_release(app->mutex);

        if(do_clear) {
            spectrogram_clear_band(app, line);
            if(bars) memset(bars, 0, (size_t)W * sizeof(float));

            /* Move to the band centre and wait for the RF path switch to settle.
             * furi_hal_subghz_set_frequency_and_path switches the CC1101 antenna
             * relay; Bruce measured a 10 ms settling requirement on T-Embed. */
            uint32_t center = f_start + (f_end - f_start) / 2;
            if(center != last_band_center) {
                subghz_devices_idle(device);
                subghz_devices_set_frequency(device, center);
                furi_delay_ms(SPECTROGRAM_ANTENNA_MS);
                last_band_center = center;
            }
        }

        if(f_end <= f_start) {
            furi_delay_ms(50);
            continue;
        }
        uint32_t span = f_end - f_start;

        float last_rssi_seen = -127.0f;
        uint32_t last_hz_seen = f_start;

        for(uint16_t col = 0; col < W; col++) {
            uint32_t hz = f_start + (uint32_t)((uint64_t)span * col / (W - 1));
            subghz_devices_idle(device);
            subghz_devices_set_frequency(device, hz);
            subghz_devices_set_rx(device);
            furi_delay_ms(SPECTROGRAM_DWELL_MS);
            float rssi = subghz_devices_get_rssi(device);

            line[col] = spectrogram_color_for_rssi(rssi);
            last_rssi_seen = rssi;
            last_hz_seen   = hz;

            if(rssi > row_max_rssi) {
                row_max_rssi = rssi;
                row_max_freq = hz;
            }

            /* Update bar level: peak-hold with exponential decay */
            if(bars) {
                float level = (rssi - SPECTROGRAM_RSSI_MIN) /
                    (SPECTROGRAM_RSSI_MAX - SPECTROGRAM_RSSI_MIN);
                if(level < 0.f) level = 0.f;
                if(level > 1.f) level = 1.f;
                bars[col] *= SPECTROGRAM_BAR_DECAY;
                if(level > bars[col]) bars[col] = level;
            }
        }

        /* Render the completed sweep row */
        if(display_mode == SpectrogramDisplayWaterfall) {
            furi_hal_spi_bus_lock();
            esp_lcd_panel_draw_bitmap(app->panel, 0, y, W, y + 1, line);
            furi_hal_spi_bus_unlock();
            y++;
            if(y >= app->band_bottom) y = app->band_top;
        } else if(bars) {
            spectrogram_draw_bars(app, line, bars);
        }

        uint32_t now_ms = furi_get_tick();

        if(now_ms - last_live_publish_ms >= SPECTROGRAM_LIVE_REDRAW_INTERVAL_MS) {
            last_live_publish_ms = now_ms;
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->last_rssi    = last_rssi_seen;
            app->last_freq_hz = last_hz_seen;
            app->scans_done++;
            app->footer_redraw_due = true;
            furi_mutex_release(app->mutex);
        }

        if(now_ms - last_max_publish_ms >= SPECTROGRAM_MAX_REDRAW_INTERVAL_MS) {
            last_max_publish_ms = now_ms;
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(row_max_rssi > app->max_rssi || app->max_freq_hz == 0) {
                app->max_rssi    = row_max_rssi;
                app->max_freq_hz = row_max_freq;
                app->max_redraw_due    = true;
                app->footer_redraw_due = true;
            }
            furi_mutex_release(app->mutex);
            row_max_rssi = -127.0f;
            row_max_freq = 0;
        }
    }

    if(bars) free(bars);
    heap_caps_free(line);
    subghz_devices_idle(device);
    subghz_devices_sleep(device);
    subghz_devices_end(device);
    subghz_devices_deinit();
    publish_status(app, SpectrogramStatusStopped);
    return 0;
}

SpectrogramWorker* spectrogram_worker_alloc(SpectrogramApp* app) {
    SpectrogramWorker* w = malloc(sizeof(SpectrogramWorker));
    w->app     = app;
    w->running = false;
    w->thread  = furi_thread_alloc_ex("SpectrogramWorker", 8192, spectrogram_worker_thread, w);
    return w;
}

void spectrogram_worker_free(SpectrogramWorker* w) {
    furi_thread_free(w->thread);
    free(w);
}

void spectrogram_worker_start(SpectrogramWorker* w) {
    w->running = true;
    furi_thread_start(w->thread);
}

void spectrogram_worker_stop(SpectrogramWorker* w) {
    w->running = false;
    furi_thread_join(w->thread);
}
