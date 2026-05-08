# Upstream provenance

This directory is a vendored copy of [xtruan/FlipBIP](https://github.com/xtruan/FlipBIP).

| Field | Value |
|---|---|
| Upstream repo | https://github.com/xtruan/FlipBIP |
| Commit SHA | `462c7fa9aa273225abcc6bd6510720c4e07ad06a` |
| Vendored on | 2026-05-08 |
| Method | Plain copy (no git subtree). `.git/`, `.github/`, and `.gitignore` were dropped. Upstream `README.md` was renamed to `README-upstream.md` to make room for a port-specific README. |

## Diff from upstream (this port)

- Replaced `application.fam` — adjusted `name`, `stack_size`, `fap_description`, `fap_author`, `fap_weburl`; removed `fap_private_libs` (Sor3nt's `buildFap.sh` doesn't parse it; the `find lib/crypto/*.c` auto-discovers crypto sources anyway).
- Patched `lib/crypto/rand.c` — defines `USE_FLIPPER_HAL_RANDOM=1` unconditionally because Sor3nt's `buildFap.sh` doesn't parse `cdefines=` from the manifest. The Furi RNG path then maps to `esp_random()` on ESP32-S3.
- Removed `lib/crypto/chacha_drbg.{c,h}` — GPLv3 in upstream trezor-crypto, unused by FlipBIP. Kept removed to leave the .fap MIT-clean.
- Renamed upstream `README.md` to `README-upstream.md`.
- Added port-specific `README.md`, `UPSTREAM.md`, `LICENSES.md`.

## Re-syncing

To pull a newer upstream:

```sh
git clone https://github.com/xtruan/FlipBIP /tmp/flipbip
# diff against /tmp/flipbip and reapply the four deltas above
```
