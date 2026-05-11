#pragma once

#include <stdint.h>
#include <esp_lcd_panel_ops.h>
#include "spectrogram_app.h"

/* RGB565 with the byte-swap the ST7789 driver expects on this port */
static inline uint16_t spectrogram_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

/* Bruce-style RSSI → RGB565 colour ramp (blue → cyan → green → yellow → red).
 * RSSI is dBm in [SPECTROGRAM_RSSI_MIN, SPECTROGRAM_RSSI_MAX]. */
uint16_t spectrogram_color_for_rssi(float rssi);

/* Draw an 8x8 char into a row-major RGB565 framebuffer of given stride */
void spectrogram_draw_char(
    uint16_t* fb,
    uint16_t fb_stride_px,
    uint16_t fb_w,
    uint16_t fb_h,
    int x,
    int y,
    char c,
    uint16_t fg,
    uint16_t bg);

/* Draw a 0-terminated string with the 8x8 font */
void spectrogram_draw_str(
    uint16_t* fb,
    uint16_t fb_stride_px,
    uint16_t fb_w,
    uint16_t fb_h,
    int x,
    int y,
    const char* s,
    uint16_t fg,
    uint16_t bg);

/* Fill a region of the framebuffer with one colour */
void spectrogram_fill_rect(
    uint16_t* fb,
    uint16_t fb_stride_px,
    int x,
    int y,
    int w,
    int h,
    uint16_t color);

/* Compose and push the top header band (title + axis ticks) */
void spectrogram_draw_header(SpectrogramApp* app, uint16_t* line_buf);

/* Compose and push the bottom footer band (max RSSI / range / hints) */
void spectrogram_draw_footer(SpectrogramApp* app, uint16_t* line_buf);

/* Returns the human-readable band name for a given band mode */
const char* spectrogram_band_name(SpectrogramBandMode m);
