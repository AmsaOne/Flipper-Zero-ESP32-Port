#include "spectrogram_worker.h"
#include "spectrogram_render.h"
#include "spectrogram_freqs.h"

#include <furi.h>
#include <furi_hal_spi_bus.h>
#include <esp_heap_caps.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/preset.h>

#define TAG "Spectrogram"

#define SPECTROGRAM_DWELL_US      50U     /* µs to wait after set_rx before reading RSSI */
#define SPECTROGRAM_BAR_DECAY     0.85f   /* multiplied per sweep when signal drops */
#define SPECTROGRAM_ANTENNA_MS    10U     /* RF path settling after a band change */
#define SPECTROGRAM_BAR_STRIPE_H  16U     /* rows per draw_bitmap in bar chart (9 calls vs 130) */

#define SPECTROGRAM_MAX_REDRAW_INTERVAL_MS  1000U
#define SPECTROGRAM_LIVE_REDRAW_INTERVAL_MS  200U

/* Spectrum-scan preset optimised for speed:
 *   MDMCFG4 = 0x0C  →  CHANBW_E=0, CHANBW_M=0  →  812.5 kHz BW (widest)
 *                       DRATE_E = 12
 *   MDMCFG3 = 0x22  →  DRATE_M = 34  →  ~115 kBaud
 *   T_SYM ≈ 8.7 µs, RSSI settle ≈ (2*1+2)*8.7 ≈ 35 µs
 *   MCSM0   = 0x08  →  FS_AUTOCAL disabled (no per-pixel calibration stall)
 * Together this lets us use a 50 µs dwell instead of 1 ms, giving ~8× faster
 * sweeps while still sampling well after RSSI has settled. */
static const uint8_t spectrogram_preset_regs[] = {
    0x02, 0x0D,   /* IOCFG0:   GD0 async serial */
    0x03, 0x07,   /* FIFOTHR:  ADC retention */
    0x08, 0x32,   /* PKTCTRL0: async, continuous, no whitening */
    0x0B, 0x06,   /* FSCTRL1:  IF = 152 kHz */
    0x14, 0x00,   /* MDMCFG0 */
    0x13, 0x00,   /* MDMCFG1 */
    0x12, 0x30,   /* MDMCFG2:  OOK, no preamble/sync */
    0x11, 0x22,   /* MDMCFG3:  DRATE_M=34  (~115 kBaud) */
    0x10, 0x0C,   /* MDMCFG4:  BW=812 kHz, DRATE_E=12 */
    0x18, 0x08,   /* MCSM0:    FS_AUTOCAL=00, PO_TIMEOUT=10 */
    0x19, 0x18,   /* FOCCFG */
    0x1D, 0x91,   /* AGCCTRL0: medium hysteresis, 16-sample AGC */
    0x1C, 0x00,   /* AGCCTRL1 */
    0x1B, 0x07,   /* AGCCTRL2: max LNA gain, MAIN_TARGET 42 dB */
    0x20, 0xFB,   /* WORCTRL */
    0x22, 0x11,   /* FREND0 */
    0x21, 0xB6,   /* FREND1 */
    0x00, 0x00,   /* end of registers */
    0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* PA table */
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

/* Draw the bar chart using a multi-row stripe buffer so we push
 * ceil(bar_h / STRIPE_H) draw_bitmap calls instead of bar_h calls.
 * At bar_h=130 and STRIPE_H=16 that is 9 calls vs 130 — ~10x faster.
 *
 * Bars grow from the bottom; color follows the RSSI ramp (blue baseline
 * → red/yellow at peaks) matching the waterfall color scheme. */
static void spectrogram_draw_bars(
    SpectrogramApp* app,
    uint16_t* stripe,   /* DMA buffer: W * SPECTROGRAM_BAR_STRIPE_H pixels */
    const float* bars) {
    const uint16_t W     = app->panel_w;
    const uint16_t bar_h = app->band_bottom - app->band_top;
    const uint16_t SH    = SPECTROGRAM_BAR_STRIPE_H;

    furi_hal_spi_bus_lock();
    for(uint16_t row = 0; row < bar_h; row += SH) {
        uint16_t rows_this = ((row + SH) > bar_h) ? (bar_h - row) : SH;
        for(uint16_t sr = 0; sr < rows_this; sr++) {
            uint16_t actual_row = row + sr;
            float threshold = 1.0f - (float)actual_row / (float)bar_h;
            float rssi = SPECTROGRAM_RSSI_MIN +
                threshold * (SPECTROGRAM_RSSI_MAX - SPECTROGRAM_RSSI_MIN);
            uint16_t filled = spectrogram_color_for_rssi(rssi);
            uint16_t* dst = stripe + (size_t)sr * W;
            for(uint16_t col = 0; col < W; col++) {
                dst[col] = (bars[col] >= threshold) ? filled : 0;
            }
        }
        uint16_t y = app->band_top + row;
        esp_lcd_panel_draw_bitmap(app->panel, 0, y, W, y + rows_this, stripe);
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

    /* Stripe buffer for fast bar rendering: STRIPE_H rows in one draw_bitmap */
    uint16_t* bar_stripe = heap_caps_malloc(
        (size_t)W * SPECTROGRAM_BAR_STRIPE_H * sizeof(uint16_t), MALLOC_CAP_DMA);

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
            furi_delay_us(SPECTROGRAM_DWELL_US);
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
            (void)y; /* bars mode uses full area, y not needed */
            /* Use the stripe buffer when available; fall back to single-row
             * rendering with the scan line buffer if DMA alloc failed. */
            uint16_t* draw_buf = bar_stripe ? bar_stripe : line;
            spectrogram_draw_bars(app, draw_buf, bars);
        }

        /* Yield once per row so input handling and other tasks stay responsive */
        furi_thread_yield();

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
    if(bar_stripe) heap_caps_free(bar_stripe);
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
