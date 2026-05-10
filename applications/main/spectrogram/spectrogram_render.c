#include "spectrogram_render.h"
#include "spectrogram_font.h"

#include <furi.h>
#include <furi_hal_spi_bus.h>
#include <stdio.h>
#include <string.h>

static int clampi(int v, int lo, int hi) {
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

uint16_t spectrogram_color_for_rssi(float rssi) {
    /* Map [-100, -30] dBm → [0, 255] level (Bruce inverts so strong = high level) */
    float span = SPECTROGRAM_RSSI_MAX - SPECTROGRAM_RSSI_MIN;
    float t = (rssi - SPECTROGRAM_RSSI_MIN) / span;
    if(t < 0.f) t = 0.f;
    if(t > 1.f) t = 1.f;
    int level = (int)(t * 255.0f + 0.5f);

    uint8_t r = 0, g = 0, b = 0;
    if(level <= 63) {
        b = (uint8_t)(64 + (level * (255 - 64)) / 63);
    } else if(level <= 127) {
        int k = level - 64;
        g = (uint8_t)((k * 255) / 63);
        b = (uint8_t)(255 - (k * 255) / 63);
    } else if(level <= 191) {
        int k = level - 128;
        r = (uint8_t)((k * 255) / 63);
        g = 255;
    } else {
        int k = level - 192;
        r = 255;
        g = (uint8_t)(255 - (k * 255) / 63);
    }
    return spectrogram_rgb565(r, g, b);
}

void spectrogram_draw_char(
    uint16_t* fb,
    uint16_t fb_stride_px,
    uint16_t fb_w,
    uint16_t fb_h,
    int x,
    int y,
    char c,
    uint16_t fg,
    uint16_t bg) {
    int idx = (unsigned char)c - 0x20;
    if(idx < 0 || idx > 0x5F) idx = 0;
    const uint8_t* glyph = spectrogram_font_8x8[idx];

    for(int row = 0; row < SPECTROGRAM_FONT_H; row++) {
        int py = y + row;
        if(py < 0 || py >= fb_h) continue;
        uint8_t bits = glyph[row];
        uint16_t* dst = fb + (size_t)py * fb_stride_px + x;
        for(int col = 0; col < SPECTROGRAM_FONT_W; col++) {
            int px = x + col;
            if(px < 0 || px >= fb_w) continue;
            dst[col] = (bits & (1 << col)) ? fg : bg;
        }
    }
}

void spectrogram_draw_str(
    uint16_t* fb,
    uint16_t fb_stride_px,
    uint16_t fb_w,
    uint16_t fb_h,
    int x,
    int y,
    const char* s,
    uint16_t fg,
    uint16_t bg) {
    int cx = x;
    while(*s) {
        spectrogram_draw_char(fb, fb_stride_px, fb_w, fb_h, cx, y, *s, fg, bg);
        cx += SPECTROGRAM_FONT_W;
        s++;
    }
}

void spectrogram_fill_rect(
    uint16_t* fb,
    uint16_t fb_stride_px,
    int x,
    int y,
    int w,
    int h,
    uint16_t color) {
    for(int row = 0; row < h; row++) {
        uint16_t* dst = fb + (size_t)(y + row) * fb_stride_px + x;
        for(int col = 0; col < w; col++) dst[col] = color;
    }
}

static void format_freq_mhz(char* out, size_t cap, uint32_t hz) {
    uint32_t mhz_int = hz / 1000000U;
    uint32_t khz_rem = (hz % 1000000U) / 1000U;
    snprintf(out, cap, "%lu.%03lu", (unsigned long)mhz_int, (unsigned long)khz_rem);
}

void spectrogram_draw_header(SpectrogramApp* app, uint16_t* line_buf) {
    const uint16_t W = app->panel_w;
    const uint16_t H = SPECTROGRAM_HEADER_H;
    const uint16_t BG = spectrogram_rgb565(0, 0, 0);
    const uint16_t FG = spectrogram_rgb565(255, 255, 255);
    const uint16_t TICK = spectrogram_rgb565(80, 80, 80);

    spectrogram_fill_rect(line_buf, W, 0, 0, W, H, BG);

    /* Title */
    spectrogram_draw_str(line_buf, W, W, H, 4, 1, "RF SPECTROGRAM", FG, BG);

    uint32_t f_start, f_end;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    f_start = app->f_start_hz;
    f_end = app->f_end_hz;
    furi_mutex_release(app->mutex);

    /* Frequency tick labels at 0%, 25%, 50%, 75%, 100% */
    char buf[16];
    int label_y = SPECTROGRAM_HEADER_H - SPECTROGRAM_FONT_H - 1;
    for(int i = 0; i < 5; i++) {
        uint32_t hz = f_start + (uint32_t)((uint64_t)(f_end - f_start) * i / 4U);
        format_freq_mhz(buf, sizeof(buf), hz);
        int label_w = (int)strlen(buf) * SPECTROGRAM_FONT_W;
        int x = (i * (W - 1)) / 4 - label_w / 2;
        if(i == 0) x = 0;
        if(i == 4) x = W - label_w;
        if(x < 0) x = 0;
        spectrogram_draw_str(line_buf, W, W, H, x, label_y, buf, TICK, BG);
    }

    furi_hal_spi_bus_lock();
    esp_lcd_panel_draw_bitmap(app->panel, 0, 0, W, H, line_buf);
    furi_hal_spi_bus_unlock();
}

void spectrogram_draw_footer(SpectrogramApp* app, uint16_t* line_buf) {
    const uint16_t W = app->panel_w;
    const uint16_t H = SPECTROGRAM_FOOTER_H;
    const uint16_t BG = spectrogram_rgb565(0, 0, 0);
    const uint16_t FG = spectrogram_rgb565(255, 255, 255);
    const uint16_t MAX_FG = spectrogram_rgb565(255, 255, 0);
    const uint16_t HIGHLIGHT = spectrogram_rgb565(255, 165, 0);

    spectrogram_fill_rect(line_buf, W, 0, 0, W, H, BG);

    SpectrogramField sel;
    uint32_t f_start, f_end, f_max;
    float r_max;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    sel = app->selected;
    f_start = app->f_start_hz;
    f_end = app->f_end_hz;
    f_max = app->max_freq_hz;
    r_max = app->max_rssi;
    furi_mutex_release(app->mutex);

    /* Top row: max RSSI / freq @ panel_w */
    char line1[48];
    if(f_max != 0) {
        char fbuf[16];
        format_freq_mhz(fbuf, sizeof(fbuf), f_max);
        snprintf(line1, sizeof(line1), "Max %d dBm @ %s MHz", (int)r_max, fbuf);
    } else {
        snprintf(line1, sizeof(line1), "Max --- dBm @ --- MHz");
    }
    spectrogram_draw_str(line_buf, W, W, H, 2, 1, line1, MAX_FG, BG);

    /* Bottom row: range + selected hint */
    char line2[48];
    char sbuf[16], ebuf[16];
    format_freq_mhz(sbuf, sizeof(sbuf), f_start);
    format_freq_mhz(ebuf, sizeof(ebuf), f_end);
    snprintf(line2, sizeof(line2), "%s - %s MHz   ", sbuf, ebuf);

    int row_y = 1 + SPECTROGRAM_FONT_H + 2;
    int x = 2;
    spectrogram_draw_str(line_buf, W, W, H, x, row_y, line2, FG, BG);
    x += (int)strlen(line2) * SPECTROGRAM_FONT_W;

    /* Selected-field indicator */
    const char* hint;
    switch(sel) {
    case SpectrogramFieldStart: hint = "[START]"; break;
    case SpectrogramFieldEnd: hint = "[END]"; break;
    default: hint = "[EXIT]"; break;
    }
    spectrogram_draw_str(line_buf, W, W, H, x, row_y, hint, HIGHLIGHT, BG);
    x += (int)strlen(hint) * SPECTROGRAM_FONT_W + 4;

    const char* keys = " OK:cycle  Rot:adj  Back:exit";
    int keys_w = (int)strlen(keys) * SPECTROGRAM_FONT_W;
    int kx = W - keys_w - 1;
    if(kx > x) {
        spectrogram_draw_str(line_buf, W, W, H, kx, row_y, keys, FG, BG);
    }

    int y0 = app->band_bottom;
    furi_hal_spi_bus_lock();
    esp_lcd_panel_draw_bitmap(app->panel, 0, y0, W, y0 + H, line_buf);
    furi_hal_spi_bus_unlock();

    /* Clamp values used for next compare in case the caller reads back */
    (void)clampi(0, 0, 0);
}
