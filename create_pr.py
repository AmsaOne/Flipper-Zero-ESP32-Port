#!/usr/bin/env python3
"""
RF Spectrogram PR creation script.

DO NOT RUN until:
  1. Testing on real hardware is confirmed good
  2. The branch has been rebased/squashed to remove development noise
     (the current pr/spectrogram branch contains NFC/EMV commits and
      multiple "fix" iterations that should be squashed before going upstream)

When ready, run from the repo root:
    python create_pr.py

Requires: gh CLI authed as AmsaOne
    gh auth status
"""

import subprocess
import sys

TITLE = "CC1101 RF Spectrogram -- waterfall + bar chart spectrum analyzer for T-Embed CC1101"

BODY = """Adds a Bruce-style RF spectrum analyzer to the T-Embed CC1101 PLUS.
The app appears in the main menu as **RF Spectrogram** and uses the onboard
CC1101 radio to sweep sub-GHz bands and display live signal activity.

## Display modes

**Waterfall** (default) — each row is one complete frequency sweep painted
as a heat-map (blue = noise floor, cyan/green = weak signal, yellow/red =
strong). Rows scroll continuously; the display fills in ~5 seconds then wraps.

**Bar chart** — vertical bars per frequency bin with peak-hold and exponential
decay (0.85x per sweep). Bars grow from the bottom with the same color ramp as
the waterfall, so signal history is visible even after a burst ends.

## Band modes

Long-press the encoder cycles the sweep range:

| Mode | Frequency range | Notes |
|---|---|---|
| 433 MHz (default) | 378 -- 481 MHz | Full CC1101 path range |
| 868 MHz | 749 -- 962 MHz | Full CC1101 path range |
| 315 MHz | 281 -- 361 MHz | Full CC1101 path range |
| Custom | Preset pair | Fine-tune with encoder rotation |

In full-band modes the entire hardware-valid range for that antenna path is
swept across all 320 display columns. Short-press toggles waterfall/bars.
The RF path relay is given a 10 ms settling delay when switching bands.

## Controls

| Input | Action |
|---|---|
| Rotate encoder | Step start/end frequency (custom mode only) |
| Short press | Toggle [START]/[END] selection (custom) or waterfall/bars (band mode) |
| Long press | Cycle band mode |
| Back | Exit |

## CC1101 configuration

| Register | Value | Purpose |
|---|---|---|
| MDMCFG4 | 0x7C | 232 kHz BW (CHANBW_E=1, M=3), DRATE_E=12 |
| MDMCFG3 | 0x22 | ~115 kBaud; RSSI settles in ~35 us |
| MCSM0 | 0x08 | FS_AUTOCAL disabled (no per-pixel VCO calibration) |
| AGCCTRL0 | 0x40 | 8-sample AGC, low hysteresis |
| AGCCTRL2 | 0x07 | Max LNA gain, MAIN_TARGET 42 dB |

Dwell is 30 us explicit + ~30 us SPI overhead = ~60 us total, safely above
the ~35 us RSSI settle time at 232 kHz BW / 115 kBaud. The display rate
is approximately 24 rows/sec (waterfall) and 39 sweeps/sec (bar chart).

RSSI display range: -115 to -20 dBm with an inverse-quadratic perceptual
boost curve so weak signals near the noise floor register as visible color
rather than mapping to near-black.

## Board compatibility

The app is board-gated via the existing `BOARD_HAS_CC1101` macro. It compiles
cleanly for all three supported boards:
- T-Embed CC1101 PLUS: full functionality
- esp32s3_generic: compiles, CC1101 device lookup fails gracefully
- Waveshare C6: compiles, app hidden (no CC1101 hardware)

## Files

```
applications/main/spectrogram/
    spectrogram_app.c/h     -- main loop, input, band/mode state
    spectrogram_worker.c/h  -- FuriThread sweep loop, CC1101 control, rendering
    spectrogram_render.c/h  -- color ramp, header/footer composition, bar draw
    spectrogram_freqs.h     -- CC1101 band definitions + 48 preset frequencies
    spectrogram_font.h      -- 8x8 monospace font for header/footer labels
    application.fam         -- app registration (MENUEXTERNAL, appid=spectrogram)
```
"""

def main():
    print("Creating PR to Sor3nt/Flipper-Zero-ESP32-Port ...")
    print("Title:", TITLE)
    print()

    cmd = [
        "gh", "pr", "create",
        "--repo", "Sor3nt/Flipper-Zero-ESP32-Port",
        "--head", "AmsaOne:pr/spectrogram",
        "--base", "main",
        "--title", TITLE,
        "--body", BODY,
    ]

    result = subprocess.run(cmd)
    if result.returncode != 0:
        print("ERROR: gh pr create failed (exit code %d)" % result.returncode)
        sys.exit(result.returncode)

if __name__ == "__main__":
    main()
