#include "spectrogram_render.h"
#include "spectrogram_font.h"

#include <furi.h>
#include <furi_hal_spi_bus.h>
#include <stdio.h>
#include <string.h>


uint16_t spectrogram_color_for_rssi(float rssi) {
    /* Map [-100, -30] dBm → [0, 255] level (Bruce inverts so strong = high level) */
    float span = SPECTROGRAM_RSSI_MAX - SPECTROGRAM_RSSI_MIN;
    float t = (rssi - SPECTROGRAM_RSSI_MIN) / span;
    if(t < 0.f) t = 0.f;
    if(t > 1.f) t = 1.f;
    int level = (int)(t * 255.0f + 0.5f);

    uint8_t r = 0, g = 0, b = 0;
    if(level <= 63) {
        /* Noise floor starts at a mid blue so a dead band is still visible */
        b = (uint8_t)(128 + (level * (255 - 128)) / 63);
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
    const uint16_t BG        = spectrogram_rgb565(0,   0,   0);
    const uint16_t FG        = spectrogram_rgb565(255, 255, 255);
    const uint16_t YELLOW    = spectrogram_rgb565(255, 255, 0);
    const uint16_t ORANGE    = spectrogram_rgb565(255, 165, 0);
    const uint16_t CYAN      = spectrogram_rgb565(0,   220, 220);
    const uint16_t ERR_COLOR = spectrogram_rgb565(255, 64,  64);

    spectrogram_fill_rect(line_buf, W, 0, 0, W, H, BG);

    SpectrogramField sel;
    SpectrogramStatus status;
    SpectrogramBandMode band_mode;
    SpectrogramDisplayMode display_mode;
    uint32_t f_start, f_end, f_max, f_now;
    float r_max, r_now;
    uint32_t scans;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    sel          = app->selected;
    status       = app->status;
    band_mode    = app->band_mode;
    display_mode = app->display_mode;
    f_start = app->f_start_hz;
    f_end   = app->f_end_hz;
    f_max   = app->max_freq_hz;
    r_max   = app->max_rssi;
    f_now   = app->last_freq_hz;
    r_now   = app->last_rssi;
    scans   = app->scans_done;
    furi_mutex_release(app->mutex);

    /* Row 1: status line */
    char line1[64];
    uint16_t status_color = YELLOW;
    if(status == SpectrogramStatusRunning) {
        char fbuf[16], mbuf[16];
        format_freq_mhz(fbuf, sizeof(fbuf), f_now);
        if(f_max != 0) {
            format_freq_mhz(mbuf, sizeof(mbuf), f_max);
            snprintf(line1, sizeof(line1),
                "Now %d @ %s  Max %d @ %s  #%lu",
                (int)r_now, fbuf, (int)r_max, mbuf, (unsigned long)scans);
        } else {
            snprintf(line1, sizeof(line1),
                "Now %d dBm @ %s  scans=%lu",
                (int)r_now, fbuf, (unsigned long)scans);
        }
    } else if(status == SpectrogramStatusStarting) {
        snprintf(line1, sizeof(line1), "Starting CC1101...");
    } else if(status == SpectrogramStatusErrDevice) {
        snprintf(line1, sizeof(line1), "ERROR: cc1101_int not found");
        status_color = ERR_COLOR;
    } else if(status == SpectrogramStatusErrBegin) {
        snprintf(line1, sizeof(line1), "ERROR: CC1101 begin failed");
        status_color = ERR_COLOR;
    } else if(status == SpectrogramStatusErrAlloc) {
        snprintf(line1, sizeof(line1), "ERROR: DMA alloc failed");
        status_color = ERR_COLOR;
    } else {
        snprintf(line1, sizeof(line1), "Stopped");
    }
    spectrogram_draw_str(line_buf, W, W, H, 2, 1, line1, status_color, BG);

    /* Row 2: mode-aware controls */
    int row_y = 1 + SPECTROGRAM_FONT_H + 2;
    int x = 2;

    if(band_mode == SpectrogramBandCustom) {
        /* Custom mode: show frequency range + selected field */
        char sbuf[16], ebuf[16];
        format_freq_mhz(sbuf, sizeof(sbuf), f_start);
        format_freq_mhz(ebuf, sizeof(ebuf), f_end);
        char range[36];
        snprintf(range, sizeof(range), "%s-%s ", sbuf, ebuf);
        spectrogram_draw_str(line_buf, W, W, H, x, row_y, range, FG, BG);
        x += (int)strlen(range) * SPECTROGRAM_FONT_W;

        const char* sel_tag = (sel == SpectrogramFieldStart) ? "[START]" : "[END]";
        spectrogram_draw_str(line_buf, W, W, H, x, row_y, sel_tag, ORANGE, BG);
        x += (int)strlen(sel_tag) * SPECTROGRAM_FONT_W + 2;

        const char* keys = " OK:sel Rot:freq LongOK:band";
        int kw = (int)strlen(keys) * SPECTROGRAM_FONT_W;
        int kx = W - kw - 1;
        if(kx > x)
            spectrogram_draw_str(line_buf, W, W, H, kx, row_y, keys, FG, BG);
    } else {
        /* Full-band or bars mode: show band name + display type */
        const char* bname = spectrogram_band_name(band_mode);
        const char* dtype = (display_mode == SpectrogramDisplayBars) ? "BARS" : "WFALL";
        char mode_str[24];
        snprintf(mode_str, sizeof(mode_str), "%s %s", bname, dtype);
        spectrogram_draw_str(line_buf, W, W, H, x, row_y, mode_str, CYAN, BG);
        x += (int)strlen(mode_str) * SPECTROGRAM_FONT_W + 4;

        const char* keys = "OK:view LongOK:band Back:exit";
        int kw = (int)strlen(keys) * SPECTROGRAM_FONT_W;
        int kx = W - kw - 1;
        if(kx > x)
            spectrogram_draw_str(line_buf, W, W, H, kx, row_y, keys, FG, BG);
    }

    int y0 = app->band_bottom;
    furi_hal_spi_bus_lock();
    esp_lcd_panel_draw_bitmap(app->panel, 0, y0, W, y0 + H, line_buf);
    furi_hal_spi_bus_unlock();
}
