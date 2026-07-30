# Smart Lock Firmware

ESP32-S3 firmware for an NFC-based smart lock. The lock emulates an ISO 14443-4
Type A card via a PN532 controller over I2C. A mobile application taps the lock
to authenticate and unlock.

This repository contains only the lock firmware. The companion mobile
application (iOS / Android) is developed in a separate repository — a link will
be provided once development is complete.

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
│   │   ├── Comms Module Master Reference Document.md
│   │   │                              # Cross-layer architecture & API contract
│   │   ├── comm_module.md             # Comm Module facade implementation & API reference
│   │   ├── lli.md                     # LLI implementation details & API reference
│   │   ├── transport.md               # Transport layer details & API reference
│   │   └── session.md                 # Session layer details & API reference
│   └── Specifications/
│       ├── smartlock_application_layer.pdf
│       ├── smartlock_hardware_link_layer_pn532_spec.pdf
│       ├── smartlock_session_layer.pdf
│       └── smartlock_transport_layer.pdf
├── components/
│   ├── pn532/                         # PN532 NFC driver
│   │   ├── include/
│   │   │   ├── pn532.h                # Core driver API (transport-agnostic)
│   │   │   ├── pn532_cmd.h            # Command layer (10 NFC commands)
│   │   │   └── pn532_i2c.h            # I2C transport backend
│   │   ├── src/
│   │   │   ├── pn532.c                # Frame builder/parser, ACK, resync
│   │   │   ├── pn532_cmd.c            # PN532 command wrappers
│   │   │   └── pn532_i2c.c            # I2C bus lifecycle, IRQ/polling, bus recovery
│   │   └── docs/                      # Driver documentation (6 files)
│   ├── lli/                           # Link Layer Interface
│   │   ├── include/
│   │   │   └── lli.h                  # Public API (no PN532 types exposed)
│   │   └── src/
│   │       └── lli.c                  # Card emulation, APDU exchange, abort
│   ├── transport/                     # Transport layer (mutual auth state machine)
│   │   ├── include/
│   │   │   └── transport.h            # Public API (no PN532 or LLI internals exposed)
│   │   └── src/
│   │       └── transport.c            # State machine, C-APDU parsing, secure session
│   ├── session/                       # Session layer (crypto, handshake, secure channel)
│   │   ├── include/
│   │   │   └── session.h              # Public API (implements transport callbacks)
│   │   └── src/
│   │       ├── session.c              # Handshake M1/M3, secure payloads, erase
│   │       ├── session_crypto.h/.c    # Crypto seam (Ed25519, X25519, HKDF, AES-GCM)
│   │       └── third_party/           # Vendored Monocypher 4.x (Ed25519)
│   └── comm_module/                   # Communication Module Facade
│       ├── include/
│       │   └── comm_module.h          # Sole header exposed to Application Module
│       └── src/
│           └── comm_module.c          # Stack wiring, FreeRTOS comm task, mailbox handoff
└── main/
    ├── CMakeLists.txt
    └── smart_lock_firmware.c           # Application entry point / smoke test harness
```

## Architecture

```
┌──────────────────────────┐
│   Application Module     │  Dispatch / authorization / AAI (external peer)
├──────────────────────────┤
│   Comm Module Facade     │  Single facade API (comm_module.h, mailbox handoff)
├──────────────────────────┤
│      Session Layer       │  Mutual-auth handshake, AES-256-GCM, secure erase
├──────────────────────────┤
│     Transport Layer      │  State machine, C-APDU parsing, secure session
├──────────────────────────┤
│           LLI            │  Card emulation, APDU I/O, link status, abort
├──────────────────────────┤
│   PN532 Command Layer    │  NFC commands (TgInitAsTarget, TgGetData, etc.)
├──────────────────────────┤
│    PN532 Core Driver     │  Frame format, checksums, ACK, error recovery
├──────────────────────────┤
│   PN532 I2C Transport    │  I2C master, IRQ/polling, bus recovery
├──────────────────────────┤
│      ESP32-S3 (I2C)      │
└──────────────────────────┘
```

Each layer only knows about the one directly below it. The PN532 driver is
transport-agnostic (vtable-based), the LLI exposes no PN532 types, the
transport layer imports only `lli.h`, the session layer imports only
`transport.h`, and the Comm Module Facade encapsulates the stack into `comm_module.h`.

## Protocol Overview

The lock and phone perform a three-message mutual-authentication handshake
over NFC, then exchange AES-256-GCM encrypted payloads:

1. **M1 (Phone → Lock):** Phone sends its ephemeral X25519 public key and a
   random challenge.
2. **M2 (Lock → Phone):** Lock sends its ephemeral X25519 public key, its own
   challenge, and an Ed25519 signature over the full transcript (domain-separated
   as `"SLOCK-HS-v1" ‖ version ‖ pk_eph_P ‖ pk_eph_L ‖ c_P ‖ c_L`). The
   phone verifies the lock's long-term identity.
3. **M3 (Phone → Lock):** Phone sends an Ed25519 signature over the same
   transcript. The lock verifies it against a list of provisioned public keys.

Both sides derive AES-256-GCM session keys via HKDF-SHA-256 from the X25519
shared secret and both challenges.  All session material is wiped on every
teardown path (link loss, timeout, abort, or authentication failure).

Full specification: [doc/Specifications/smartlock_session_layer.pdf](doc/Specifications/smartlock_session_layer.pdf)

## Component Documentation

| Component | Doc | Standalone? |
|-----------|-----|-------------|
| PN532 driver | `components/pn532/docs/` (6 files) | Yes — vtable-based, SPI/UART backends can be added |
| LLI | [doc/Implementation/lli.md](doc/Implementation/lli.md) | Yes — works without transport layer |
| Transport | [doc/Implementation/transport.md](doc/Implementation/transport.md) | Yes — works with stub crypto callbacks |
| Session | [doc/Implementation/session.md](doc/Implementation/session.md) | Yes — works with stub peer provider / app handler |
| Comm Module Facade | [doc/Implementation/comm_module.md](doc/Implementation/comm_module.md) | Unified facade & task mailbox handoff |
| Master Reference | [doc/Implementation/Comms Module Master Reference Document.md](doc/Implementation/Comms%20Module%20Master%20Reference%20Document.md) | Cross-layer architecture & API contract |
| Session spec | [doc/Specifications/smartlock_session_layer.pdf](doc/Specifications/smartlock_session_layer.pdf) | N/A |

Each layer's documentation includes a complete API reference and a standalone
usage example so you can use a single layer without the rest of the stack.

## Current Status

- [x] PN532 driver (I2C, IRQ + polling, bus recovery, vtable-based transport abstraction)
- [x] Link Layer Interface (card emulation, APDU exchange, abort)
- [x] Transport layer (mutual auth state machine, C-APDU parsing, secure session)
- [x] Session layer (X25519 + Ed25519 handshake, HKDF key derivation, AES-256-GCM secure channel)
- [x] Mutual authentication protocol specification
- [x] Communication module facade (wires Session + Transport + LLI + drivers behind one API & task mailbox handoff)
- [x] Application module (smoke test harness & mailbox contract implementation)

## Future Plans

### Production Application Module & ACL

Full command dispatch, authorization policy evaluation, and lock actuation (motor/solenoid driver with position feedback and tamper response).

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
