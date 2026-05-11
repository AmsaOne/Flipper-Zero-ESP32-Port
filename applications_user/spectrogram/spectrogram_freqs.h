#pragma once

#include <stdint.h>
#include <stddef.h>

/* CC1101 tuneable bands. Outside these the radio refuses to set frequency, so
 * sweeping across the gaps would just paint dead air. We constrain the start/
 * end pickers AND the sweep to stay inside a single band. */
typedef struct {
    uint32_t lo_hz;
    uint32_t hi_hz;
} SpectrogramBand;

static const SpectrogramBand spectrogram_bands[] = {
    {281000000U, 361000000U}, /* 315 MHz band - full CC1101 path range */
    {378000000U, 481000000U}, /* 433 MHz band - full CC1101 path range */
    {749000000U, 962000000U}, /* 868 / 915 MHz band - full CC1101 path range */
};
#define SPECTROGRAM_BAND_COUNT \
    (sizeof(spectrogram_bands) / sizeof(spectrogram_bands[0]))

/* Common tuning targets per band, ported from Bruce rf_utils.cpp and filtered
 * to entries strictly inside the CC1101's tuneable ranges. */
static const uint32_t spectrogram_freq_presets_hz[] = {
    /* 315 MHz band (300 - 348 MHz) */
    300000000U, 302757000U, 303875000U, 304250000U, 307000000U, 307500000U, 307800000U,
    309000000U, 310000000U, 312000000U, 312100000U, 312200000U, 313000000U, 313850000U,
    314000000U, 314350000U, 314980000U, 315000000U, 318000000U, 330000000U, 345000000U,
    348000000U,
    /* 433 MHz band (387 - 464 MHz) */
    387000000U, 390000000U, 418000000U, 430000000U, 430500000U, 431000000U, 431500000U,
    432000000U, 432100000U, 432870000U, 433075000U, 433220000U, 433420000U, 433657070U,
    433889000U, 433920000U, 434075000U, 434176948U, 434190000U, 434390000U, 434420000U,
    434620000U, 438900000U, 440175000U, 464000000U,
    /* 868 / 915 MHz band (779 - 928 MHz) */
    779000000U, 868350000U, 868400000U, 868800000U, 868950000U, 906400000U, 915000000U,
    925000000U, 928000000U,
};

#define SPECTROGRAM_FREQ_PRESET_COUNT \
    (sizeof(spectrogram_freq_presets_hz) / sizeof(spectrogram_freq_presets_hz[0]))

/* Which band does this frequency live in? Returns SPECTROGRAM_BAND_COUNT if none. */
static inline size_t spectrogram_freq_band(uint32_t hz) {
    for(size_t i = 0; i < SPECTROGRAM_BAND_COUNT; i++) {
        if(hz >= spectrogram_bands[i].lo_hz && hz <= spectrogram_bands[i].hi_hz) return i;
    }
    return SPECTROGRAM_BAND_COUNT;
}

static inline size_t spectrogram_freq_nearest_index(uint32_t hz) {
    size_t best = 0;
    uint32_t best_diff = UINT32_MAX;
    for(size_t i = 0; i < SPECTROGRAM_FREQ_PRESET_COUNT; i++) {
        uint32_t f = spectrogram_freq_presets_hz[i];
        uint32_t d = (f > hz) ? (f - hz) : (hz - f);
        if(d < best_diff) {
            best_diff = d;
            best = i;
        }
    }
    return best;
}
