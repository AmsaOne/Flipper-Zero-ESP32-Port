#include "spectrogram_worker.h"
#include "spectrogram_render.h"

#include <furi.h>
#include <furi_hal_spi_bus.h>
#include <furi_hal_spi_types.h>
#include <furi_hal_spi.h>
#include <esp_heap_caps.h>
#include <cc1101.h>
#include <cc1101_regs.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/preset.h>

#define TAG "Spectrogram"

#define SPECTROGRAM_DWELL_US    400U
#define SPECTROGRAM_MAX_REDRAW_INTERVAL_MS  1000U
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

/* Calibrate the CC1101 VCO at the given centre frequency, then disable
 * auto-calibration so subsequent IDLE→RX transitions take ~100 µs instead
 * of ~750 µs.  Call with the device already in IDLE. */
static void spectrogram_calibrate(const SubGhzDevice* device, uint32_t center_hz) {
    subghz_devices_set_frequency(device, center_hz);
    /* SCAL strobe: calibrate synthesiser from IDLE without needing
     * FS_AUTOCAL=1.  Calibration takes ~750 µs. */
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    cc1101_strobe(&furi_hal_spi_bus_handle_subghz, CC1101_STROBE_SCAL);
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
    furi_delay_ms(2);
    /* Disable auto-calibration (MCSM0 bits[5:4]=00, keep PO_TIMEOUT=10). */
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_MCSM0, 0x08);
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
}

/* Fill the waterfall band with black so old data doesn't bleed through
 * after a frequency range change. */
static void spectrogram_clear_band(SpectrogramApp* app, uint16_t* line) {
    const uint16_t W = app->panel_w;
    memset(line, 0, (size_t)W * sizeof(uint16_t));
    furi_hal_spi_bus_lock();
    for(uint16_t cy = app->band_top; cy < app->band_bottom; cy++) {
        esp_lcd_panel_draw_bitmap(app->panel, 0, cy, W, cy + 1, line);
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
        FURI_LOG_E(TAG, "cc1101_int device not registered");
        publish_status(app, SpectrogramStatusErrDevice);
        subghz_devices_deinit();
        return -1;
    }

    subghz_devices_reset(device);
    subghz_devices_idle(device);
    subghz_devices_load_preset(device, FuriHalSubGhzPresetOok650Async, NULL);

    /* One-time calibration at the centre of the initial sweep range.
     * Also writes MCSM0=0x08 to disable future auto-calibration so
     * IDLE→RX transitions no longer pay the ~750 µs calibration cost. */
    {
        uint32_t init_start, init_end;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        init_start = app->f_start_hz;
        init_end   = app->f_end_hz;
        furi_mutex_release(app->mutex);
        spectrogram_calibrate(device, init_start + (init_end - init_start) / 2);
    }

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
        bool do_recal = false;
        uint32_t recal_center = 0;

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        f_start = app->f_start_hz;
        f_end   = app->f_end_hz;
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
            do_recal = true;
            recal_center = f_start + (f_end - f_start) / 2;
        }
        furi_mutex_release(app->mutex);

        if(do_recal) {
            spectrogram_clear_band(app, line);
            subghz_devices_idle(device);
            spectrogram_calibrate(device, recal_center);
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
            furi_delay_us(SPECTROGRAM_DWELL_US);
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
