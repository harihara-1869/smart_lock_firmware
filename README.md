# Smart Lock Firmware

ESP32-S3 firmware for an NFC-based smart lock. The lock emulates an ISO 14443-4 Type A card via a PN532 controller over I2C. A companion mobile application taps the lock to perform mutual authentication, provision new phones via QR code scanning, and secure unlocking.

## Companion Mobile Application

The companion mobile application (iOS / Android) is developed in a separate repository:
👉 **[Smart Lock Mobile Application](https://github.com/harihara-1869/smart_lock_application)**

---

## Hardware Architecture

| Component | Part | Interface | Notes |
|-----------|------|-----------|-------|
| MCU | ESP32-S3 | — | Native FreeRTOS & ESP-IDF support |
| NFC Frontend | PN532 | I2C (400 kHz) | Card emulation mode (ISO 14443-4 Type A) |
| Display | E-Paper / E-Ink (Planned) | SPI (250x122) | Console ASCII QR fallback currently active |

### Default GPIO Mapping

| Signal | GPIO | Description |
|--------|------|-------------|
| SDA | 8 | I2C Data line |
| SCL | 9 | I2C Clock line |
| IRQ | 10 | PN532 → ESP32 (active-low interrupt line). Set `-1` for polling mode |
| RST | 11 | PN532 Reset line (active-low). Set `-1` if unwired |

---

## Architecture & Layering

The codebase strictly enforces modular layer boundaries. Upper layers depend only on the public API of the layer directly beneath them.

```
┌─────────────────────────────────────────────────────────┐
│                    Application Module                   │
│   (Provision Manager, NVS Store, App Display, Commands) │
├─────────────────────────────────────────────────────────┤
│                   Comm Module Facade                    │
│     (Single entry-point for App, Mailbox Handoff Task)  │
├─────────────────────────────────────────────────────────┤
│                      Session Layer                      │
│ (Mutual-Auth Handshake, X25519/Ed25519, AES-256-GCM)   │
├─────────────────────────────────────────────────────────┤
│                     Transport Layer                     │
│    (APDU State Machine, Framing, Secure Payload Router) │
├─────────────────────────────────────────────────────────┤
│                   Link Layer Interface                  │
│       (ISO 14443-4 Card Emulation, Link Status, Mock)   │
├─────────────────────────────────────────────────────────┤
│                    PN532 Driver Layer                   │
│        (Command Layer, Frame Parser, I2C Transport)     │
├─────────────────────────────────────────────────────────┤
│                     ESP32-S3 Hardware                   │
└─────────────────────────────────────────────────────────┘
```

### Component Summary

- **`app_module`**: Owns Application logic, provision state machine (`provision_mgr`), persistent key storage (`nvs_store`), and display abstraction (`app_display`).
- **`comm_module`**: Unified facade encapsulating the lower protocol stack. Runs the main FreeRTOS comm task and manages mailbox handoffs.
- **`session`**: Handles cryptographic handshake (X25519, Ed25519, HKDF-SHA256, AES-256-GCM) and identity verification caching.
- **`transport`**: APDU state machine handling C-APDU/R-APDU parsing and encrypted payload routing.
- **`lli`**: Link Layer Interface providing card emulation & APDU transceive abstraction. Supports compile-time mocking for integration testing.
- **`pn532`**: Low-level PN532 chip driver over I2C with automatic bus recovery and IRQ/polling support.

---

## Provisioning Architecture (QR Code + Secret Verification)

The smart lock features a secure, single-session QR-based phone provisioning mechanism:

```
┌──────┐                                 ┌──────┐                                ┌───────┐
│ User │                                 │ Lock │                                │ Phone │
└──┬───┘                                 └──┬───┘                                └───┬───┘
   │  Press Provision Button                │                                        │
   │───────────────────────────────────────>│                                        │
   │                                        │ Generates 32B CSPRNG Secret            │
   │                                        │ Displays QR Code / ASCII Output        │
   │                                        │ Arms 60s Single-Session Window         │
   │                                        │                                        │
   │  Scans QR Code                         │                                        │
   │────────────────────────────────────────────────────────────────────────────────>│
   │                                        │                                        │
   │                                        │  NFC Mutual Auth (M1 -> M2 -> M3)      │
   │                                        │<──────────────────────────────────────>│
   │                                        │  (Lock caches M3 signature &           │
   │                                        │   defers identity verification)        │
   │                                        │                                        │
   │                                        │  Encrypted CMD_PROVISION               │
   │                                        │<───────────────────────────────────────│
   │                                        │  [0x01 | 32B Secret | 32B Phone PK]    │
   │                                        │                                        │
   │                                        │  Constant-time secret comparison       │
   │                                        │  Verifies M3 signature with Phone PK   │
   │                                        │  Persists Phone PK into NVS Store      │
   │                                        │                                        │
   │                                        │  Encrypted ACK (0x90 0x00)             │
   │                                        │───────────────────────────────────────>│
```

1. **Arming Window**: When activated, `provision_mgr` generates a 32-byte (256-bit) cryptographically secure random Provision Secret via `esp_fill_random` and displays it.
2. **Handshake Caching**: During an armed window, the `session` layer allows an unprovisioned phone to complete the M1->M2->M3 handshake by caching the phone's signature $Sig\_P$ and transcript, deferring identity validation.
3. **Execution**: The phone sends an encrypted `CMD_PROVISION` payload over the AES-256-GCM secure session.
4. **Verification & Storage**: `provision_mgr` validates the Provision Secret using constant-time comparison, verifies the phone's signature against its public key via `comm_module_provision_verify_identity`, and registers the phone's public key into `nvs_store`.

---

## Project Structure

```
smart_lock_firmware/
├── CMakeLists.txt                      # Root build configuration
├── doc/
│   ├── Implementation/                 # Master references & component specifications
│   │   ├── Communication_Module_Master.md
│   │   ├── comm_module.md
│   │   ├── lli.md
│   │   ├── session.md
│   │   └── transport.md
│   └── Specifications/                 # PDF protocol specifications
├── components/
│   ├── app_module/                     # Application logic, provision manager, NVS
│   ├── comm_module/                    # Communication module facade & task runner
│   ├── session/                        # Session crypto, handshake, secure channel
│   ├── transport/                      # Transport APDU state machine
│   ├── lli/                            # Link Layer Interface (with mock support)
│   ├── pn532/                          # PN532 driver over I2C
│   └── test_utils/                     # Mock phone client for integration testing
└── main/
    ├── CMakeLists.txt
    ├── smart_lock_firmware.c          # Application entry point
    ├── test_integration.c              # Automated integration test runner
    └── test_integration.h
```

---

## Documentation

Comprehensive documentation for the system architecture, component specifications, and protocol details is available in the [`doc/`](doc/) directory:

### Implementation Guides (`doc/Implementation/`)

| Document | Description |
|----------|-------------|
| [Communication_Module_Master.md](doc/Implementation/Communication_Module_Master.md) | **Master Reference**: Complete cross-layer architecture, state ownership, provisioning architecture, security invariants, and API contracts. |
| [comm_module.md](doc/Implementation/comm_module.md) | Public API reference, facade implementation, and FreeRTOS task mailbox queue handoff. |
| [session.md](doc/Implementation/session.md) | Cryptographic primitives (X25519, Ed25519, HKDF-SHA256, AES-256-GCM), handshake protocol, and provisioning cache. |
| [transport.md](doc/Implementation/transport.md) | APDU state machine, command framing, C-APDU/R-APDU parsing, and session teardown lifecycle. |
| [lli.md](doc/Implementation/lli.md) | Link Layer Interface details, card emulation parameters, and LLI mock interface. |

### Component & Driver Docs

- **PN532 Driver Docs**: Detailed driver architecture, I2C bus recovery mechanisms, and command references located in [`components/pn532/docs/`](components/pn532/docs/).

### Protocol Specifications (`doc/Specifications/`)

- **[smartlock_communication_module_spec.pdf](doc/Specifications/smartlock_communication_module_spec.pdf)**: Formal communication module protocol specification.

---

## Prerequisites & Building

### Prerequisites

- **ESP-IDF v5.4.4** — [Installation Guide](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/get-started/)
- ESP32-S3 Development Board
- PN532 Breakout Board wired via I2C

### Build Instructions

```bash
# 1. Export ESP-IDF environment
source ~/esp/esp-idf/export.sh

# 2. Set target chip
idf.py set-target esp32s3

# 3. Build firmware
idf.py build
```

### Flashing & Monitoring

Connect the ESP32-S3 via USB and run:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

*(Replace `/dev/ttyUSB0` with your target serial port. Press `Ctrl+]` to exit monitor)*

---

## Integration Testing

The codebase includes an automated **Full Stack Integration Test Suite** that tests the entire protocol stack (Transport, Session, Comm Module, Provision Manager, NVS Store) without requiring physical NFC hardware.

### Running Integration Tests

Mocking is enabled by default via `add_compile_options(-DMOCK_LLI_FOR_TESTING=1)` in `CMakeLists.txt`.

1. Flash the firmware to your board:
   ```bash
   idf.py -p /dev/ttyUSB0 flash monitor
   ```
2. The `test_task` will automatically spawn on boot, simulate a virtual phone client, execute the M1->M2->M3 handshake, transmit encrypted `CMD_PROVISION` payloads, and verify key persistence in NVS.

To switch back to physical PN532 NFC hardware, remove `# Enable LLI Mocking` from the root `CMakeLists.txt`.

---

## License

Copyright (C) 2026 Harihara. Licensed under the [GNU General Public License v3.0](licence).
