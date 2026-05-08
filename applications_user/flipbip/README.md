# FlipBIP (T-Embed Plus port)

[BIP39](https://en.bitcoin.it/wiki/BIP_0039) / [BIP44](https://en.bitcoin.it/wiki/BIP_0044) hierarchical-deterministic crypto wallet, ported from [xtruan/FlipBIP](https://github.com/xtruan/FlipBIP) to run on Sor3nt's [Flipper-Zero-ESP32-Port](https://github.com/Sor3nt/Flipper-Zero-ESP32-Port).

**Target hardware:** LilyGO **T-Embed Plus** (build target `t_embed`, hardware ID `32`).
**Distribution:** SD-card sideload as a `.fap` — no firmware reflash required to update the app.

## Why this port exists

Sor3nt's port re-implements the Furi/Flipper API surface on ESP32-S3, so a stock Flipper FAP that uses standard Furi calls (gui, view_dispatcher, scene_manager, storage, canvas, furi_hal_random) builds and runs with minimal changes. FlipBIP qualifies, with two adjustments:

1. **RNG:** [`lib/crypto/rand.c`](lib/crypto/rand.c) is patched to define `USE_FLIPPER_HAL_RANDOM=1` unconditionally. Sor3nt's `furi_hal_random_get/fill_buf` map to `esp_random()` / `esp_fill_random()`. (`buildFap.sh` does not parse `cdefines=` from `application.fam`, so the gate is set in source.)
2. **Stack:** raised from upstream's 3 KB to 8 KB to absorb deeper FreeRTOS-on-ESP32 frames in `secp256k1` and BIP39 PBKDF2. Sor3nt's `buildFap.sh` clamps stack to a 16 KB minimum anyway.

No source changes were needed for canvas/GUI, storage, view dispatcher, scene manager, or input.

## Hardware compatibility

| Board | Build target | Tested? |
|---|---|---|
| LilyGO T-Embed Plus | `t_embed` (esp32s3) | yes (this port) |
| LilyGO T-Embed CC1101 | `t_embed` (esp32s3) | build-compatible (same target) — not runtime-verified |
| Flipper-Zero-on-ESP32 generic (`generic_esp32s3`) | not built | not tested |
| Waveshare ESP32-C6-LCD-1.9 | n/a | not built (RISC-V; QR rendering and 320×172 layout untested) |

The LilyGO T-Embed Plus shares the `t_embed` board target with the CC1101 model — same ESP32-S3, same ST7789 320×170 display, same rotary encoder + side button. The "Plus" denotes extra flash and PSRAM only.

## Install

1. Flash the firmware in this fork to your T-Embed Plus (this fork adds one menu entry pointing to the SD-card path below):

    ```sh
    ./build.sh --board t_embed
    ./flash.sh --board t_embed   # or use the bundled web flasher
    ```

2. Build the FAP:

    ```sh
    ./buildFap.sh applications_user/flipbip
    # output: build_t_embed/fap/flipbip.fap
    ```

3. Copy the FAP to your SD card at:

    ```
    /ext/apps/Tools/flipbip.fap
    ```

4. Boot the T-Embed Plus, open the main menu, select **FlipBIP (T-Embed Plus)**.

## Verification

Use the canonical Trezor test-vector mnemonic to confirm the crypto path is correct end-to-end:

```
abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about
```

Derive BTC `m/44'/0'/0'/0/0`. Expected address: `1LqBGSKuX5yYUonjxT5qGfpUsXKYYWeabA`.

A non-matching address indicates a problem in the RNG/HAL shim, the trezor-crypto build flags, or storage encoding — not a problem in the wallet itself.

## Wallet data

Encrypted seed is stored on the SD card under `/ext/apps_data/flipbip/`. See upstream FlipBIP docs for the format.

## Licensing

- This port: MIT (inherits from upstream FlipBIP — see [LICENSE](LICENSE)).
- Vendored cryptography in [lib/crypto/](lib/crypto/) is from [trezor/trezor-firmware](https://github.com/trezor/trezor-firmware), MIT-licensed (see [`lib/crypto/LICENSE`](lib/crypto/LICENSE)).
- Upstream FlipBIP shipped one GPLv3 file (`chacha_drbg.c/.h`) inherited from trezor-crypto. It was unused by FlipBIP and has been removed in this port to keep the .fap MIT-clean. See [LICENSES.md](LICENSES.md) for the full audit.

## Provenance

See [UPSTREAM.md](UPSTREAM.md) for the upstream FlipBIP commit this port was derived from.
