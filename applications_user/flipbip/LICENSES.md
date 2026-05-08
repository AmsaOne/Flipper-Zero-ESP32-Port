# License audit — FlipBIP (T-Embed Plus port)

Audited 2026-05-08 against upstream FlipBIP commit `462c7fa9...`. Audit method: `grep -h "License\|Copyright\|GPL"` over every `.c`/`.h` in the vendored tree.

## Top-level

- This port and the FlipBIP application code: **MIT** (see [LICENSE](LICENSE), inherited from xtruan/FlipBIP).

## `lib/crypto/`

Imported from [trezor/trezor-firmware](https://github.com/trezor/trezor-firmware) `crypto/`. Top-level [`lib/crypto/LICENSE`](lib/crypto/LICENSE) is **MIT**. Per-file headers reviewed:

| Group | License | Notes |
|---|---|---|
| Core (sha2, ripemd160, hmac, pbkdf2, secp256k1, bip32, bip39, ecdsa, base58, etc.) | MIT | © 2013–2021 SatoshiLabs / Trezor authors |
| `aes/` (Brian Gladman's AES) | Brian Gladman BSD-style | Permissive, MIT-compatible |
| `chacha20poly1305/` | CC0 / Public Domain (per file) | Permissive |
| `ed25519_donna/` | CC0 / Apache-2.0 / OpenSSL (tri-license at user's option) | Permissive — Apache-2.0 chosen |
| `monero/` | MIT | Trezor-firmware vendored |
| ~~`chacha_drbg.{c,h}`~~ | ~~GPLv3+~~ | **Removed in this port.** Unused by FlipBIP; removed to leave the .fap MIT-clean |

## Outcome

The .fap binary distributed from this fork is MIT-only with permissive third-party components (Apache-2.0, BSD-style, CC0). No GPL components are linked in.

If the upstream FlipBIP project re-adds calls to the removed `chacha_drbg`, this audit must be re-run. The `find lib/crypto -name '*.c'` in `buildFap.sh` would otherwise re-introduce the GPL contamination silently.
