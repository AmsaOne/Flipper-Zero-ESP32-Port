# RF Spectrogram -- Tester Build Guide

Branch: `pr/spectrogram`
Fork:   https://github.com/AmsaOne/Flipper-Zero-ESP32-Port

Tests wanted on: **LilyGO T-Embed CC1101 Plus** (ESP32-S3, 16 MB flash).

---

## What You're Testing

A Bruce-style RF spectrum analyzer built into the sor3nt Flipper-Zero-ESP32-Port
firmware. Opens in the main menu as **RF Spectrogram**.

Controls:

| Input | Action |
|---|---|
| Rotate encoder | Tune start/end frequency (custom mode only) |
| Short press | Toggle [START]/[END] (custom) or swap waterfall/bars (band mode) |
| Long press | Cycle band: Custom -> 315 MHz -> 433 MHz -> 868 MHz -> Custom |
| Back button | Exit |

---

## Step 1 -- Clone the Fork

```bash
git clone https://github.com/AmsaOne/Flipper-Zero-ESP32-Port.git
cd Flipper-Zero-ESP32-Port
git checkout pr/spectrogram
```

---

## Step 2 -- Install ESP-IDF v5.4.1

**Exact version required: v5.4.1. No other version will work.**

### Windows

Download and run the Windows installer from Espressif:
https://dl.espressif.com/dl/esp-idf/?idf=5.4

Choose the **Offline installer** for v5.4.1. Install to the default path
`C:\Espressif\frameworks\esp-idf-v5.4.1`.

After install, verify in a new command prompt:

```cmd
C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat
idf.py --version
```

Expected output: `ESP-IDF v5.4.1`

### Linux / macOS

```bash
# Install system dependencies (Ubuntu/Debian)
sudo apt install git wget flex bison gperf python3 python3-pip python3-venv \
    cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

# Clone ESP-IDF at the exact tag
mkdir -p ~/esp && cd ~/esp
git clone --recursive --branch v5.4.1 \
    https://github.com/espressif/esp-idf.git esp-idf

# Install toolchains (downloads ~1 GB)
cd ~/esp/esp-idf
./install.sh esp32s3

# Verify
source ~/esp/esp-idf/export.sh
idf.py --version   # must print: ESP-IDF v5.4.1
```

---

## Step 3 -- Build

### Windows

```cmd
cd Flipper-Zero-ESP32-Port
python winbuild.py build
```

Build output goes to `build_t_embed/`. Takes 3-7 minutes on first run,
under 60 seconds on incremental rebuilds.

If your ESP-IDF is not at the default path, set the env var first:

```cmd
set ESP_IDF_DIR=C:\path\to\esp-idf-v5.4.1
python winbuild.py build
```

### Linux / macOS

```bash
source ~/esp/esp-idf/export.sh   # or add to .bashrc/.zshrc
cd Flipper-Zero-ESP32-Port
chmod +x build.sh
./build.sh --board t_embed --build-only
```

---

## Step 4 -- Flash

Connect the T-Embed via USB. Put it into download mode if needed (hold BOOT,
press RESET, release BOOT -- some units auto-enter download mode on connect).

### Windows

Find your COM port in Device Manager (usually COM14 or similar):

```cmd
python winbuild.py flash --port COM14
```

Replace `COM14` with your actual port number.

### Linux / macOS

The build script auto-detects the port:

```bash
./build.sh --board t_embed
```

Or specify explicitly:

```bash
./build.sh --board t_embed --port /dev/ttyACM0
```

---

## Step 5 -- Use the App

After flashing, the device reboots automatically. Navigate the main menu to
**RF Spectrogram** and launch it.

- Default view: **433 MHz WFALL** -- the full 378-481 MHz band as a waterfall
- **Long-press** the encoder to cycle bands (433 -> 868 -> 315 -> Custom)
- **Short-press** in a band mode to switch between waterfall and bar chart
- Hold a 433 MHz device (key fob, weather sensor remote) next to the T-Embed
  and press it -- you should see a colored spike in the waterfall

---

## What to Report

- Does the app launch without crashing?
- Do both waterfall and bar chart display correctly?
- Does the center encoder click work without accidentally exiting the app?
- Can you see signal spikes when a nearby 433 MHz device transmits?
- Approximate speed (roughly how many rows per second fill the waterfall)
- Any freezes, crashes, or unexpected exits -- and what you were doing when it happened

---

## Pre-built Binary (skip the compile)

If you'd rather not build from source, ask @AmsaOne on Discord for a
pre-built `furi_esp32.bin` and flash manually with esptool:

```bash
# Linux / macOS
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0 bootloader/bootloader.bin \
    0x10000 furi_esp32.bin \
    0x8000 partition_table/partition-table.bin
```

```cmd
:: Windows
esptool.py --chip esp32s3 -p COM14 -b 460800 ^
    --before default_reset --after hard_reset write_flash ^
    --flash_mode dio --flash_freq 80m --flash_size 16MB ^
    0x0 bootloader\bootloader.bin ^
    0x10000 furi_esp32.bin ^
    0x8000 partition_table\partition-table.bin
```

esptool is already installed by the ESP-IDF setup. If you only have Python,
install it standalone: `pip install esptool`
