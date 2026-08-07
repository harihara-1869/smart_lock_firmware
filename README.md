# Smart Lock Firmware

ESP32-S3 firmware for an NFC-based smart lock. The lock emulates an ISO 14443-4 Type A card via a PN532 controller over I2C. A companion mobile application taps the lock to perform mutual authentication, provision new phones via QR code scanning, and secure unlocking.

The Application Module (command dispatch, provisioning, actuation orchestration) and its peer modules (Actuator, Display, Integrity, Storage) are built as of the current scaffold; the deep design lives in [`Application_Module_Master.md`](docs/Application_Module_Master.md) and [`MODULE_RESPONSIBILITIES.md`](docs/MODULE_RESPONSIBILITIES.md). This README covers commands, test modes, and the project layout.

## Companion Mobile Application

The companion mobile application (iOS / Android) is developed in a separate repository:
 **[Smart Lock Mobile Application](https://github.com/harihara-1869/smart_lock_application)**

---

## Hardware Architecture

| Component | Part | Interface | Notes |
|-----------|------|-----------|-------|
| MCU | ESP32-S3 | — | Native FreeRTOS & ESP-IDF support |
| NFC Frontend | PN532 | I2C (400 kHz) | Card emulation mode (ISO 14443-4 Type A) |
| Actuator | Stepper / DC motor (planned) | RMT / MCPWM | Stub backend active (simulated) |
| Display | E-Paper / E-Ink (Planned) | SPI (250x122) | Console ASCII QR fallback currently active |
| Integrity | Tamper switch (planned) | GPIO | Stub backend active (simulated) |
| Storage | NVS / secure element (planned) | — | RAM placeholder backend active |

### Default GPIO Mapping

| Signal | GPIO | Description |
|--------|------|-------------|
| SDA | 8 | I2C Data line |
| SCL | 9 | I2C Clock line |
| IRQ | 10 | PN532 → ESP32 (active-low interrupt line). Set `-1` for polling mode |
| RST | 11 | PN532 Reset line (active-low). Set `-1` if unwired |
| Limit switch LOCKED | 4 | Actuator — fully locked position |
| Limit switch UNLOCKED | 5 | Actuator — fully unlocked position |
| Green LED | 14 | Display — success indication |
| Red LED | 15 | Display — error indication |
| Buzzer | 16 | Display — success chime / error beeps |

All GPIO assignments live in each component's `Kconfig` (e.g. `components/actuator/Kconfig`) — one central location per module, not scattered through code.

---

## Architecture & Layering

The codebase strictly enforces modular layer boundaries. Upper layers depend only on the public API of the layer directly beneath them.

```
┌─────────────────────────────────────────────────────────────┐
│                     Application Module                      │
│   (coordinator — dispatch, provisioning, actuation policy)  │
│   depends ONLY on comm_module.h + peer interfaces           │
├─────────────┬──────────────┬──────────────┬─────────────────┤
│  Comm Stack │   Actuator   │   Display    │    Integrity    │
│  (frozen)   │   (AAI)      │              │                 │
├─────────────┴──────────────┴──────────────┴─────────────────┤
│                        Storage (seam)                       │
│       key_store.h / intent_log.h — RAM placeholder now      │
└─────────────────────────────────────────────────────────────┘

Comm stack detail:
┌─────────────────────────────────────────────────────────┐
│                    Application Module                   │
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

- **`app_module`**: Application coordinator. Owns command dispatch (`app_dispatch`), provisioning workflow (`provision_mgr`), the authorized-key iterator for M3 verification, actuation intent + boot recovery, and the integrity cadence. **No peripheral drivers** — it talks only to the peer interfaces.
- **`comm_module`**: Unified facade encapsulating the lower protocol stack. Runs the main FreeRTOS comm task and manages mailbox handoffs.
- **`session`**: Handles cryptographic handshake (X25519, Ed25519, HKDF-SHA256, AES-256-GCM) and identity verification caching.
- **`transport`**: APDU state machine handling C-APDU/R-APDU parsing and encrypted payload routing.
- **`lli`**: Link Layer Interface providing card emulation & APDU transceive abstraction. Supports compile-time mocking for integration testing.
- **`pn532`**: Low-level PN532 chip driver over I2C with automatic bus recovery and IRQ/polling support.
- **`actuator`**: Actuator Abstraction Interface (`aai.h`). Reports bolt position/stall facts; no policy. Backends: stub (default), RMT stepper, MCPWM DC — swapped via Kconfig.
- **`display`**: All user-feedback peripherals (LEDs, buzzer, QR panel). Application decides WHAT/WHEN; Display owns HOW. Backends: console (default), e-paper panel.
- **`integrity`**: Tamper/integrity sampling on a fixed cadence. Reports findings; policy stays in the Application. Backends: stub (default), tamper GPIO.
- **`storage`**: Persistence **seam** — `key_store.h` / `intent_log.h` interfaces. RAM cache + write-through persistence over a private HAL. NVS flash backend (default, power-loss safe); Secure Element stub planned.
- **`test_utils`**: Mock phone client for integration testing.

---

## Application Commands

All commands arrive as plaintext through the encrypted session and are dispatched by the Application. Every outcome is encoded in the response bytes (there is no comm-visible failure code — denial/fault/invalid command is an app status byte).

| Opcode | Name | Request | Success response | Failure |
|---|---|---|---|---|
| `0x01` | `CMD_PROVISION` | `OP ‖ SECRET(32) ‖ PHONE_PK(32)` (65 B) | `0x00 ‖ LOCK_PK(32)` (33 B) | `0x01` invalid secret |
| `0x02` | `CMD_UNLOCK` | `OP` (1 B) | `0x00` (immediate; actuation async) | `0x02`/`0x04`/`0x05` |
| `0x03` | `CMD_LOCK` | `OP` (1 B) | `0x00` (immediate; actuation async) | `0x02`/`0x04`/`0x05` |
| `0x04` | `CMD_GET_STATUS` | `OP` (1 B) | `0x00 ‖ BATTERY_PCT ‖ LOCK_STATE ‖ LAST_ERROR` (4 B) | `0x03` malformed |
| `0x05` | `CMD_REVOKE_KEY` | `OP ‖ TARGET_PK(32)` (33 B) | `0x00` | `0x02`/`0x09` |

Status bytes: `0x00` OK, `0x01` invalid secret, `0x02` unauthorized, `0x03` invalid cmd, `0x04` fault, `0x05` busy, `0x06` internal, `0x07` key store full, `0x08` key exists, `0x09` not found.

**Async actuation (tap-and-go):** `CMD_UNLOCK`/`CMD_LOCK` reply `0x00` immediately (the phone disconnects), then the Application drives the motor on its own schedule. On a jam it auto-reverses to LOCKED — never leaving the bolt partially engaged. See `Application_Module_Master.md` §1–§2.

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
   │                                        │  Persists Phone PK into Key Store      │
   │                                        │                                        │
   │                                        │  Encrypted ACK (0x00 | Lock PK)        │
   │                                        │───────────────────────────────────────>│
```

1. **Arming Window**: `provision_mgr` generates a 32-byte (256-bit) cryptographically secure random Provision Secret via `esp_fill_random` and renders it as a QR via the Display peer.
2. **Handshake Caching**: During an armed window, the `session` layer allows an unprovisioned phone to complete the M1->M2->M3 handshake by caching the phone's signature `Sig_P` and transcript, deferring identity validation.
3. **Execution**: The phone sends an encrypted `CMD_PROVISION` payload over the AES-256-GCM secure session.
4. **Verification & Storage**: `provision_mgr` validates the Provision Secret using constant-time comparison, verifies the phone's signature against its public key via `comm_module_provision_verify_identity`, and registers the phone's public key into the key store (`key_store.h`). **No path skips signature verification unconditionally.**

---

## Project Structure

```
smart_lock_firmware/
├── CMakeLists.txt                      # Root build configuration (LLI mock flag)
├── Kconfig                             # (see main/Kconfig.projbuild — test-mode choice)
├── docs/
│   ├── Application_Module_Master.md    # Application design: dispatch, async actuation, errors
│   ├── Communication_Module_Master.md  # Master reference: comm stack + provisioning contract
│   ├── MODULE_RESPONSIBILITIES.md      # Ownership matrix, boundaries, init/boot flows, test modes
│   ├── comm_module.md                  # Facade API + mailbox semantics
│   ├── session.md                      # Crypto handshake + provisioning cache
│   ├── transport.md                    # APDU state machine
│   ├── lli.md                          # Link Layer Interface + mock
│   └── Smart_Lock.pdf                  # Protocol specification
├── components/
│   ├── app_module/                     # Coordinator: dispatch, provisioning, actuation policy
│   ├── actuator/                       # AAI + stub/RMT/MCPWM backends (Kconfig)
│   ├── display/                        # LEDs/buzzer/QR panel + console/panel backends
│   ├── integrity/                      # Tamper checks + stub/GPIO backends
│   ├── storage/                        # key_store / intent_log seam + RAM backend
│   ├── comm_module/                    # Comm facade & task runner
│   ├── session/                        # Session crypto, handshake, secure channel
│   ├── transport/                      # Transport APDU state machine
│   ├── lli/                            # Link Layer Interface (with mock support)
│   ├── pn532/                          # PN532 driver over I2C
│   └── test_utils/                     # Mock phone client for integration testing
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild               # Smart Lock Test Mode choice (5 modes)
    ├── smart_lock_firmware.c           # Entry point: dependency-order init + test-mode branch
    ├── test_integration.c              # Mock-phone integration test (provisioning POST)
    └── test_integration.h
```

---

## Documentation

Comprehensive documentation for the system architecture, component specifications, and protocol details is available in the [`docs/`](docs/) directory:

| Document | Description |
|----------|-------------|
| [Application_Module_Master.md](docs/Application_Module_Master.md) | **Application design**: async actuation workflow, mechanical error handling, command dispatch & response contracts. |
| [Communication_Module_Master.md](docs/Communication_Module_Master.md) | **Master Reference**: cross-layer architecture, mailbox model, provisioning design, security invariants, API contracts. |
| [MODULE_RESPONSIBILITIES.md](docs/MODULE_RESPONSIBILITIES.md) | **Ownership matrix, boundary rules, init/boot sequence, test modes, swap-in plan.** |
| [comm_module.md](docs/comm_module.md) | Comm facade public API + mailbox handoff semantics. |
| [session.md](docs/session.md) | Cryptographic primitives + provisioning cache. |
| [transport.md](docs/transport.md) | APDU state machine, framing, teardown lifecycle. |
| [lli.md](docs/lli.md) | LLI details, card emulation parameters, mock interface. |
| [Smart_Lock.pdf](docs/Smart_Lock.pdf) | Formal protocol specification. |

---

## Prerequisites & Building

### Prerequisites

- **ESP-IDF v5.4.4** — [Installation Guide](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/get-started/)
- ESP32-S3 Development Board
- PN532 Breakout Board wired via I2C *(only for real-NFC builds; not needed for test modes)*

### Build Instructions

```bash
# 1. Export ESP-IDF environment
source ~/esp/esp-idf/export.sh

# 2. Set target chip
idf.py set-target esp32s3

# 3. Build firmware (default: FULL_APPLICATION + mock LLI)
idf.py build
```

### Flashing & Monitoring

Connect the ESP32-S3 via USB and run:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

*(Replace `/dev/ttyUSB0` with your target serial port. Press `Ctrl+]` to exit monitor)*

---

## Test Modes

The firmware builds in **6 test modes**, selected by the `Smart Lock Test Mode` choice in `main/Kconfig.projbuild` (default `FULL_APPLICATION`). Each harness initializes ONLY the modules it needs; every mode must build.

| Mode | Kconfig symbol | Initializes | What runs |
|---|---|---|---|
| Full Application | `CONFIG_TEST_MODE_FULL_APPLICATION` | storage, display, actuator, integrity, comm, app | Production path + (under mock LLI) the mock-phone provisioning integration test |
| Comm Only | `CONFIG_TEST_MODE_COMM_ONLY` | storage, comm | Echo/status loop over the real NFC stack (no NFC hardware needed) |
| Actuator Only | `CONFIG_TEST_MODE_ACTUATOR_ONLY` | actuator | `AAI_Open`/`AAI_Close`/`AAI_Stop` state machine |
| Display Only | `CONFIG_TEST_MODE_DISPLAY_ONLY` | display | Indications, tones, QR render, clear |
| Integrity Only | `CONFIG_TEST_MODE_INTEGRITY_ONLY` | integrity | Cadence checks, tamper status |
| Storage Only | `CONFIG_TEST_MODE_STORAGE_ONLY` | storage | Key store + intent log with NVS persistence verified via HAL |

### Switching modes

```bash
idf.py menuconfig        # → Smart Lock Test Mode → pick mode
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### What each mode tests

| Mode | Hardware needed | What it exercises |
|---|---|---|
| **Full Application** | Bare ESP32-S3 board (mock LLI), or PN532 wired (real NFC) | Production path: dependency-order init, app task + comm task, mailbox dispatch, provisioning, actuation, integrity. With mock LLI runs the provisioning integration test. |
| **Comm Only** | PN532 wired to GPIO 8/9/10/11 | NFC stack end-to-end: PN532 I2C → LLI → Transport → Session → Facade → echo/status loop |
| **Actuator Only** | Bare ESP32-S3 board (stub), or motor + limit switches wired | `AAI_Open`/`AAI_Close`/`AAI_Stop` state machine via the stub backend |
| **Display Only** | Bare ESP32-S3 board (console), or LEDs/buzzer/e-paper wired | Indications, tones, QR render, clear via the console backend |
| **Integrity Only** | Bare ESP32-S3 board (stub), or tamper switch wired | Cadence checks, tamper status via the stub backend |
| **Storage Only** | Bare ESP32-S3 board | Key store (add/get/contains/revoke) + intent log (write/read/clear) with real NVS persistence verified through the HAL. No motor, display, or NFC needed. |

### Per-mode examples

**Full Application** (default):
```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
# Expected (mock LLI): provisioning integration test runs and reports PASS
```

**Storage Only** — test persistence with no hardware:
```bash
idf.py menuconfig            # Smart Lock Test Mode → Storage Only
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
# Expected: key store add/get/contains/revoke + intent log write/read/clear all PASS
```

**Comm Only** — exercise the real NFC stack:
```bash
idf.py menuconfig            # Smart Lock Test Mode → Communication Module Only
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
# Tap a phone; lock performs M1→M2→M3 handshake and echoes commands back
```

**Actuator Only** — exercise the motor state machine:
```bash
idf.py menuconfig            # Smart Lock Test Mode → Actuator Only
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
# monitor shows: AAI stub open/close cycles, simulated status transitions
```

**Display Only** — exercise indications and QR rendering:
```bash
idf.py menuconfig            # Smart Lock Test Mode → Display Only
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
# monitor shows: SUCCESS/ERROR/PROVISIONING indications, tone, QR render
```

**Integrity Only** — exercise the tamper cadence:
```bash
idf.py menuconfig            # Smart Lock Test Mode → Integrity Only
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
# monitor shows: cadence checks, tamper status
```

---

## Integration Testing (mock LLI — ESP32 only, no NFC hardware)

The codebase includes an automated **Full Stack Integration Test Suite** that tests the entire protocol stack (Transport, Session, Comm Module, Provision Manager, Key Store) without requiring physical NFC hardware.

Mocking is available via `add_compile_options(-DMOCK_LLI_FOR_TESTING=1)` in the root `CMakeLists.txt` — the line is commented out by default; uncomment it to enable. In mock mode the real PN532/I2C driver is compiled **out** — no NFC hardware is touched at all.

### Running the integration test

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

The `test_task` spawns automatically on boot (in `FULL_APPLICATION` mode), simulates a virtual phone client, executes the M1->M2->M3 handshake, transmits an encrypted `CMD_PROVISION`, and verifies key persistence in the key store. Expected monitor output:

```
TEST_INTEGRATION: --- Starting Integration Test ---
TEST_INTEGRATION: Arming provisioning window...
SESSION: M3: provisioning window used, authentication deferred
TEST_INTEGRATION: Handshake successful!
PROV_MGR: provisioning successful — key committed
TEST_INTEGRATION: Provisioning Success! (lock PK echoed)
TEST_INTEGRATION: Key successfully stored.
TEST_INTEGRATION: --- Integration Test Complete ---
```

The integration test runs on a bare ESP32-S3 dev board — no PN532, no actuator/display/integrity hardware required.

### Using real NFC hardware

To switch to the physical PN532, remove the `# Enable LLI Mocking` line from the root `CMakeLists.txt` and rebuild. The PN532 must be wired to SDA/SCL/IRQ/RST (GPIO 8/9/10/11). The single-module harnesses (Actuator/Display/Integrity) remain usable without any NFC hardware.

---

## License

Copyright (C) 2026 Harihara. Licensed under the [GNU General Public License v3.0](licence).
