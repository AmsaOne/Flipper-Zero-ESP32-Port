#include "spectrogram_worker.h"
#include "spectrogram_render.h"

#include <furi.h>
#include <furi_hal_spi_bus.h>
#include <esp_heap_caps.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/preset.h>

#define TAG "Spectrogram"

#define SPECTROGRAM_DWELL_MS 2U
#define SPECTROGRAM_MAX_REDRAW_INTERVAL_MS 1000U
#define SPECTROGRAM_LIVE_REDRAW_INTERVAL_MS 200U

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

static int32_t spectrogram_worker_thread(void* context) {
    SpectrogramWorker* w = context;
    SpectrogramApp* app = w->app;

    publish_status(app, SpectrogramStatusStarting);

    subghz_devices_init();

    const SubGhzDevice* device = subghz_devices_get_by_name("cc1101_int");
    if(!device) {
        FURI_LOG_E(TAG, "cc1101_int device not registered");
        publish_status(app, SpectrogramStatusErrDevice);
        subghz_devices_deinit();
        return -1;
    }

    subghz_devices_reset(device);
    subghz_devices_idle(device);
    subghz_devices_load_preset(device, FuriHalSubGhzPresetOok650Async, NULL);

    const uint16_t W = app->panel_w;
    uint16_t* line = heap_caps_malloc((size_t)W * sizeof(uint16_t), MALLOC_CAP_DMA);
    if(!line) {
        FURI_LOG_E(TAG, "stripe alloc failed");
        publish_status(app, SpectrogramStatusErrAlloc);
        subghz_devices_sleep(device);
        subghz_devices_end(device);
        subghz_devices_deinit();
        return -1;
    }

    publish_status(app, SpectrogramStatusRunning);

    uint16_t y = app->band_top;
    float row_max_rssi = -127.0f;
    uint32_t row_max_freq = 0;
    uint32_t last_max_publish_ms = 0;
    uint32_t last_live_publish_ms = 0;

    while(w->running) {
        uint32_t f_start, f_end;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        f_start = app->f_start_hz;
        f_end = app->f_end_hz;
        if(app->params_dirty) {
            app->params_dirty = false;
            row_max_rssi = -127.0f;
            row_max_freq = 0;
            app->max_rssi = -127.0f;
            app->max_freq_hz = 0;
            app->max_redraw_due = true;
            app->header_redraw_due = true;
            app->footer_redraw_due = true;
            y = app->band_top;
        }
        furi_mutex_release(app->mutex);

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
            last_hz_seen = hz;
            if(rssi > row_max_rssi) {
                row_max_rssi = rssi;
                row_max_freq = hz;
            }
        }

        furi_hal_spi_bus_lock();
        esp_lcd_panel_draw_bitmap(app->panel, 0, y, W, y + 1, line);
        furi_hal_spi_bus_unlock();

        y++;
        if(y >= app->band_bottom) y = app->band_top;

        uint32_t now_ms = furi_get_tick();

        /* Live diagnostic: publish last RSSI / current freq every 200 ms so the
         * footer reflects what the radio is doing even if the waterfall isn't
         * showing variation (proves the worker is alive). */
        if(now_ms - last_live_publish_ms >= SPECTROGRAM_LIVE_REDRAW_INTERVAL_MS) {
            last_live_publish_ms = now_ms;
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->last_rssi = last_rssi_seen;
            app->last_freq_hz = last_hz_seen;
            app->scans_done++;
            app->footer_redraw_due = true;
            furi_mutex_release(app->mutex);
        }

        if(now_ms - last_max_publish_ms >= SPECTROGRAM_MAX_REDRAW_INTERVAL_MS) {
            last_max_publish_ms = now_ms;
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(row_max_rssi > app->max_rssi || app->max_freq_hz == 0) {
                app->max_rssi = row_max_rssi;
                app->max_freq_hz = row_max_freq;
                app->max_redraw_due = true;
                app->footer_redraw_due = true;
            }
            furi_mutex_release(app->mutex);
            row_max_rssi = -127.0f;
            row_max_freq = 0;
        }
    }

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
    w->app = app;
    w->running = false;
    w->thread = furi_thread_alloc_ex("SpectrogramWorker", 8192, spectrogram_worker_thread, w);
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
