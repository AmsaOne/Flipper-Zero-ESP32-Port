#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <esp_lcd_panel_ops.h>

#define SPECTROGRAM_HEADER_H 18
#define SPECTROGRAM_FOOTER_H 22
#define SPECTROGRAM_RSSI_MIN (-100.0f)
#define SPECTROGRAM_RSSI_MAX (-30.0f)

typedef enum {
    SpectrogramFieldStart = 0,
    SpectrogramFieldEnd,
    SpectrogramFieldExit,
    SpectrogramFieldCount,
} SpectrogramField;

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

    /* Shared state — guarded by mutex */
    uint32_t f_start_hz;
    uint32_t f_end_hz;
    SpectrogramField selected;
    bool params_dirty;
    bool stop;

    /* Worker → main: max RSSI tracking */
    float max_rssi;
    uint32_t max_freq_hz;
    bool max_redraw_due;

    /* UI redraw flags (main thread) */
    bool header_redraw_due;
    bool footer_redraw_due;
} SpectrogramApp;
