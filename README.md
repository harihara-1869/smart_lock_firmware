# Smart Lock Firmware

ESP32-S3 firmware for an NFC-based smart lock. The lock emulates an ISO 14443-4
Type A card via a PN532 controller over I2C. A mobile application taps the lock
to authenticate and unlock.

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
├── doc/
│   ├── Implementation/
│   │   ├── lli.md                  # LLI implementation details & API reference
│   │   └── transport.md            # Transport layer details & API reference
│   └── Specifications/
│       └── mutual-auth-protocol.md # Mutual authentication protocol spec
├── components/
│   ├── pn532/                      # PN532 NFC driver
│   │   ├── include/
│   │   │   ├── pn532.h             # Core driver API (transport-agnostic)
│   │   │   ├── pn532_cmd.h         # Command layer (12 NFC commands)
│   │   │   └── pn532_i2c.h         # I2C transport backend
│   │   ├── src/
│   │   │   ├── pn532.c             # Frame builder/parser, ACK, resync
│   │   │   ├── pn532_cmd.c         # 12 PN532 command wrappers
│   │   │   └── pn532_i2c.c         # I2C bus lifecycle, IRQ/polling, bus recovery
│   │   └── docs/                   # Driver documentation (6 files)
│   ├── lli/                        # Link Layer Interface
│   │   ├── include/
│   │   │   └── lli.h               # Public API (no PN532 types exposed)
│   │   └── src/
│   │       └── lli.c               # Card emulation, APDU exchange, abort
│   └── transport/                  # Transport layer (mutual auth state machine)
│       ├── include/
│       │   └── transport.h         # Public API (no PN532 or LLI internals exposed)
│       └── src/
│           └── transport.c         # State machine, C-APDU parsing, secure session
└── main/
    ├── CMakeLists.txt
    └── smart_lock_firmware.c       # Application entry point / test harness
```

## Architecture

```
┌──────────────────────────┐
│      Application         │  Crypto: ECDH, AES-GCM, Ed25519, key storage
├──────────────────────────┤
│     Transport Layer      │  State machine, C-APDU parsing, secure session
├──────────────────────────┤
│           LLI            │  Card emulation, APDU I/O, link status, abort
├──────────────────────────┤
│   PN532 Command Layer    │  12 NFC commands (TgInitAsTarget, TgGetData, etc.)
├──────────────────────────┤
│    PN532 Core Driver     │  Frame format, checksums, ACK, error recovery
├──────────────────────────┤
│   PN532 I2C Transport    │  I2C master, IRQ/polling, bus recovery
├──────────────────────────┤
│      ESP32-S3 (I2C)      │
└──────────────────────────┘
```

Each layer only knows about the one directly below it. The PN532 driver is
transport-agnostic (vtable-based), the LLI exposes no PN532 types, and the
transport layer imports only `lli.h`.

## Protocol Overview

The lock and phone perform a three-message mutual-authentication handshake
over NFC, then exchange AES-256-GCM encrypted payloads:

1. **M1 (Phone → Lock):** Phone sends its ephemeral X25519 public key and a
   random nonce.
2. **M2 (Lock → Phone):** Lock sends its ephemeral X25519 public key, nonce,
   and an Ed25519 signature over the full transcript. The phone verifies the
   lock's identity.
3. **M3 (Phone → Lock):** Phone sends an HMAC over the lock's nonce using
   the derived session key. The lock verifies the phone derived the same key.

Both sides derive AES-256-GCM session keys via HKDF-SHA-256 from the X25519
shared secret and both nonces.  All session material is wiped on every
teardown path (link loss, timeout, abort, or authentication failure).

Full specification: [doc/Specifications/mutual-auth-protocol.md](doc/Specifications/mutual-auth-protocol.md)

## Component Documentation

| Component | Doc | Standalone? |
|-----------|-----|-------------|
| PN532 driver | `components/pn532/docs/` (6 files) | Yes — vtable-based, SPI/UART backends can be added |
| LLI | [doc/Implementation/lli.md](doc/Implementation/lli.md) | Yes — works without transport layer |
| Transport | [doc/Implementation/transport.md](doc/Implementation/transport.md) | Yes — works with stub crypto callbacks |
| Protocol spec | [doc/Specifications/mutual-auth-protocol.md](doc/Specifications/mutual-auth-protocol.md) | N/A |

Each layer's documentation includes a complete API reference and a standalone
usage example so you can use a single layer without the rest of the stack.

## Current Status

- [x] PN532 driver (I2C, IRQ + polling, bus recovery)
- [x] Link Layer Interface (card emulation, APDU exchange, abort)
- [x] Transport layer (mutual auth state machine, C-APDU parsing, secure session)
- [x] Mutual authentication protocol specification
- [ ] Application crypto layer (ECDH, AES-GCM, Ed25519, key storage)
- [ ] Mobile application (iOS / Android)
- [ ] Lock actuator control (motor / solenoid driver)

## Future Plans

### Application Crypto Layer

Implements the `on_apdu` and `on_erase` callbacks expected by the transport
layer.  Handles ECDH key agreement (X25519), AES-256-GCM encrypt/decrypt,
Ed25519 signature generation/verification, HKDF-SHA-256 key derivation, and
NVS-backed key storage.

### Mobile Application

Companion app for iOS and Android.  Communicates with the lock over NFC using
the same protocol.  Manages the phone's Ed25519 keypair, stores the lock's
public key, and provides a UI for lock/unlock and key management.

### Lock Actuator Control

GPIO driver for the physical lock mechanism (motor or solenoid).  Triggered
by the application layer after a successful secure-session command.  Includes
debounce, timeout, and position feedback.

### Audit Log

Persistent log of successful and failed authentication attempts, stored in
NVS with timestamps and phone key IDs.  Accessible over the NFC session or
a future BLE companion channel.

### BLE Companion Channel

Out-of-band BLE channel for key provisioning, session auditing, and firmware
update delivery.  Complements the NFC channel for scenarios where NFC tap
proximity is inconvenient.

## License

Smart Lock Firmware is licensed under the GNU General Public License v3.0 or later (GPL-3.0-or-later).

Copyright (C) 2026 Harihara

See the LICENSE file for the full license text.
