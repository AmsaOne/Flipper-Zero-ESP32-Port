# Pre-built Binaries

## spectrogram-t-embed-cc1101.bin

Merged firmware for the **LilyGO T-Embed CC1101 Plus** (ESP32-S3, 16 MB flash).
Built from the `pr/spectrogram` branch. Includes bootloader, partition table,
and full firmware in a single file -- flash at offset `0x0`.

### Flash with esptool

```bash
# Linux / macOS
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
    --before default_reset --after hard_reset \
    write_flash 0x0 spectrogram-t-embed-cc1101.bin
```

```cmd
:: Windows
esptool.py --chip esp32s3 -p COM14 -b 460800 ^
    --before default_reset --after hard_reset ^
    write_flash 0x0 spectrogram-t-embed-cc1101.bin
```

Replace `/dev/ttyACM0` or `COM14` with your actual port.

If esptool is not installed: `pip install esptool`

### What's in the firmware

The full sor3nt Flipper-Zero-ESP32-Port firmware plus the RF Spectrogram app.
See the main README for app controls and what to report back.
