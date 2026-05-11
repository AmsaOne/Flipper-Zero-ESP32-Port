#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <esp_lcd_panel_ops.h>

#define SPECTROGRAM_HEADER_H  18
#define SPECTROGRAM_FOOTER_H  22
#define SPECTROGRAM_RSSI_MIN  (-115.0f)  /* CC1101 usable floor ~-115 dBm at 232 kHz BW */
#define SPECTROGRAM_RSSI_MAX  (-20.0f)

typedef enum {
    SpectrogramFieldStart = 0,
    SpectrogramFieldEnd,
} SpectrogramField;

/* Band modes: Custom uses preset navigation; the three named modes sweep
 * the full hardware-valid range for that band in one display width. */
typedef enum {
    SpectrogramBandCustom = 0,
    SpectrogramBand315,
    SpectrogramBand433,
    SpectrogramBand868,
    SpectrogramBandCount,
} SpectrogramBandMode;

typedef enum {
    SpectrogramDisplayWaterfall = 0,
    SpectrogramDisplayBars,
} SpectrogramDisplayMode;

typedef enum {
    SpectrogramStatusStarting = 0,
    SpectrogramStatusRunning,
    SpectrogramStatusErrDevice,
    SpectrogramStatusErrBegin,
    SpectrogramStatusErrAlloc,
    SpectrogramStatusStopped,
} SpectrogramStatus;

typedef struct SpectrogramWorker SpectrogramWorker;

typedef struct {
    Gui* gui;
    Canvas* canvas;
    FuriPubSub* input;
    FuriPubSubSubscription* input_sub;
    FuriMutex* mutex;

    esp_lcd_panel_handle_t panel;
    uint16_t panel_w;
    uint16_t panel_h;
    uint16_t band_top;
    uint16_t band_bottom;

    SpectrogramWorker* worker;

    /* Shared state - guarded by mutex */
    uint32_t f_start_hz;
    uint32_t f_end_hz;
    SpectrogramField selected;
    SpectrogramBandMode band_mode;
    SpectrogramDisplayMode display_mode;
    bool params_dirty;
    bool stop;

    /* Saved custom range, restored when returning to SpectrogramBandCustom */
    uint32_t custom_start_hz;
    uint32_t custom_end_hz;

    /* Worker->main: max RSSI tracking */
    float max_rssi;
    uint32_t max_freq_hz;
    bool max_redraw_due;

    /* Worker->main: live diagnostic */
    SpectrogramStatus status;
    float last_rssi;
    uint32_t last_freq_hz;
    uint32_t scans_done;

    /* UI redraw flags (main thread) */
    bool header_redraw_due;
    bool footer_redraw_due;
} SpectrogramApp;
