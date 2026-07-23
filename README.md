# Smart Lock Firmware

ESP32-S3 firmware for an NFC-based smart lock. The lock emulates an ISO 14443-4 Type A card via a PN532 controller over I2C. A mobile application taps the lock to authenticate and unlock.

## Hardware

| Component | Part | Interface |
|-----------|------|-----------|
| MCU | ESP32-S3 | — |
| NFC frontend | PN532 | I2C (400 kHz) |

### Default GPIO mapping

| Signal | GPIO | Notes |
|--------|------|-------|
| SDA | 8 | I2C data |
| SCL | 9 | I2C clock |
| IRQ | 10 | PN532 → ESP32 (active-low). Set to `-1` for polling mode |
| RST | 11 | PN532 reset (active-low). Set to `-1` if not wired |

## Prerequisites

- **ESP-IDF v5.4.4** — [installation guide](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/get-started/)
- An ESP32-S3 development board
- A PN532 breakout board wired to the ESP32-S3 via I2C

## Building

```bash
# 1. Activate the ESP-IDF environment
source ~/esp/esp-idf/export.sh

# 2. Set the target chip
idf.py set-target esp32s3

# 3. Build
idf.py build
```

## Flashing

Connect the ESP32-S3 via USB and run:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Replace `/dev/ttyUSB0` with the correct serial port for your system. Press `Ctrl+]` to exit the monitor.

To erase the entire flash before flashing:

```bash
idf.py -p /dev/ttyUSB0 erase-flash
```

## Project Structure

```
smart_lock_firmware/
├── CMakeLists.txt
├── components/
│   ├── pn532/                    # PN532 NFC driver
│   │   ├── include/
│   │   │   ├── pn532.h           # Core driver API (transport-agnostic)
│   │   │   ├── pn532_cmd.h       # Command layer (GetFirmwareVersion, TgInitAsTarget, etc.)
│   │   │   └── pn532_i2c.h       # I2C transport backend
│   │   ├── src/
│   │   │   ├── pn532.c           # Frame builder/parser, ACK handshake, resync
│   │   │   ├── pn532_cmd.c       # 12 PN532 command wrappers
│   │   │   └── pn532_i2c.c       # I2C bus lifecycle, IRQ/polling, bus recovery
│   │   └── docs/                 # Driver documentation
│   └── lli/                      # Link Layer Interface
│       ├── include/
│       │   └── lli.h             # Public API (no PN532 types exposed)
│       └── src/
│           └── lli.c             # Card emulation, APDU exchange, abort/re-arm
└── main/
    ├── CMakeLists.txt
    └── smart_lock_firmware.c     # Application entry point
```

## Architecture

```
┌──────────────────────────┐
│   Application (main/)    │   Smart lock logic
├──────────────────────────┤
│     LLI (components/lli) │   Card emulation, APDU I/O, abort
├──────────────────────────┤
│  PN532 Command Layer     │   12 NFC commands (TgInitAsTarget, TgGetData, etc.)
├──────────────────────────┤
│   PN532 Core Driver      │   Frame format, checksums, ACK, error recovery
├──────────────────────────┤
│   PN532 I2C Transport    │   I2C master, IRQ/polling, bus recovery
├──────────────────────────┤
│      ESP32-S3 (I2C)      │
└──────────────────────────┘
```

The PN532 driver is transport-agnostic — the core communicates through a vtable, so SPI or UART backends can be added without modifying the upper layers.

## Current Status

- [x] PN532 driver (I2C, IRQ + polling, bus recovery)
- [x] Link Layer Interface (card emulation, APDU exchange)
- [ ] Mutual authentication protocol
- [ ] Mobile application
- [ ] Lock actuator control

## Protocol

Details for the custom mutual authentication protocol will be added here.

## Mobile Application

A companion mobile app for iOS and Android is planned. Details will be added here.

## License

Smart Lock Firmware is licensed under the GNU General Public License v3.0 or later (GPL-3.0-or-later).

Copyright (C) 2026 Harihara

See the LICENSE file for the full license text.