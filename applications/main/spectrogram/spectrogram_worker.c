#include "spectrogram_worker.h"
#include "spectrogram_render.h"

#include <furi.h>
#include <furi_hal_spi_bus.h>
#include <esp_heap_caps.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/preset.h>

#define TAG "Spectrogram"

#define SPECTROGRAM_DWELL_MS    1U
#define SPECTROGRAM_MAX_REDRAW_INTERVAL_MS  1000U
#define SPECTROGRAM_LIVE_REDRAW_INTERVAL_MS 200U

/* OOK 650 kHz preset with MCSM0=0x08 (FS_AUTOCAL=00: no auto-calibration).
 * Eliminating the ~750 µs VCO calibration on every IDLE→RX transition is
 * the main speed lever — it drops per-pixel overhead from ~850 µs to ~100 µs.
 * Frequency accuracy is acceptable for RSSI-based spectrum display without
 * per-step calibration. Format: [reg, val, ...], 0x00 0x00, then 8-byte PA table. */
static const uint8_t spectrogram_preset_regs[] = {
    0x02, 0x0D, /* IOCFG0:   GD0 async serial */
    0x03, 0x07, /* FIFOTHR:  ADC_RETENTION */
    0x08, 0x32, /* PKTCTRL0: async, continuous, no whitening */
    0x0B, 0x06, /* FSCTRL1:  IF = 152 kHz */
    0x14, 0x00, /* MDMCFG0:  channel spacing 25 kHz */
    0x13, 0x00, /* MDMCFG1 */
    0x12, 0x30, /* MDMCFG2:  OOK, no preamble/sync */
    0x11, 0x32, /* MDMCFG3:  data rate ~3.8 kBaud */
    0x10, 0x17, /* MDMCFG4:  RX BW 650 kHz */
    0x18, 0x08, /* MCSM0:    FS_AUTOCAL=00 (never), PO_TIMEOUT=10 */
    0x19, 0x18, /* FOCCFG */
    0x1D, 0x91, /* AGCCTRL0 */
    0x1C, 0x00, /* AGCCTRL1 */
    0x1B, 0x07, /* AGCCTRL2: MAX LNA gain, MAIN_TARGET 42 dB */
    0x20, 0xFB, /* WORCTRL */
    0x22, 0x11, /* FREND0 */
    0x21, 0xB6, /* FREND1 */
    0x00, 0x00, /* end of registers */
    /* PA table — RX-only, values unused but required by loader */
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
    subghz_devices_load_preset(device, FuriHalSubGhzPresetCustom, (uint8_t*)spectrogram_preset_regs);

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
        bool do_clear = false;

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
            do_clear = true;
        }
        furi_mutex_release(app->mutex);

        if(do_clear) spectrogram_clear_band(app, line);

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
