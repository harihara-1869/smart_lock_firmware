# Module Responsibilities — Smart Lock Firmware

This document is the responsibility-split contract for the Application Module
and its three peer modules (Actuator, Display, Integrity) plus the Storage
seam. It defines **who owns what** (GPIOs, peripherals, NVS namespaces, tasks),
**who may call whom**, and **how the modules are initialised and exercised**
(boot sequence, async actuation, boot recovery, test modes).

The Communication Module (PN532 → LLI → Transport → Session → Facade) is
frozen; this document only describes its boundary contract as consumed by the
Application.

---

## 1. Ownership matrix

### 1.1 GPIO / peripheral ownership

| GPIO / peripheral | Owner | Module | Notes |
|---|---|---|---|
| I2C0 SDA (8), SCL (9) | Comm Module | `comm_module` | PN532 bus; configured via `comm_module_config_t` |
| PN532 IRQ (10), RST (11) | Comm Module | `comm_module` | Owned by `pn532_i2c_create` (below the facade) |
| Motor STEP (12) / DIR (13) | Actuator | `actuator` (RMT backend) | `CONFIG_ACTUATOR_PIN_STEP/DIR` |
| Motor PWM A (12) / B (13) | Actuator | `actuator` (MCPWM backend) | `CONFIG_ACTUATOR_PIN_PWM_A/B` |
| Limit switch LOCKED (4) | Actuator | `actuator` | `CONFIG_ACTUATOR_PIN_LIMIT_LOCKED` |
| Limit switch UNLOCKED (5) | Actuator | `actuator` | `CONFIG_ACTUATOR_PIN_LIMIT_UNLOCKED` |
| Stall-detect (optional, -1) | Actuator | `actuator` | `CONFIG_ACTUATOR_PIN_STALL` |
| Green status LED (14) | Display | `display` | `CONFIG_DISPLAY_PIN_LED_GREEN` |
| Red status LED (15) | Display | `display` | `CONFIG_DISPLAY_PIN_LED_RED` |
| Buzzer/chime (16) | Display | `display` | `CONFIG_DISPLAY_PIN_BUZZER` |
| E-paper panel CS/DC/RST | Display | `display` (panel backend) | `CONFIG_DISPLAY_PIN_PANEL_CS/DC/RST` |
| Tamper switch (optional, -1) | Integrity | `integrity` | `CONFIG_INTEGRITY_PIN_TAMPER` |
| Provision button | Application | `app_module` | Planned; debounced, Kconfig-configured (see §7) |
| Battery ADC | Application | `app_module` | TODO(hardware): placeholder constant today |

**Rule:** every GPIO/pin/timer is declared in exactly one component's Kconfig.
No module reads a pin that another module owns.

### 1.2 NVS namespace ownership

| Namespace | Owner | Module | Status |
|---|---|---|---|
| (none yet) | Storage backend | `storage` | The RAM placeholder persists nothing. The planned NVS / secure-element backend owns all persistence namespaces (`keys`, `intent`, …) **inside the storage component only** |

**Rule:** `nvs_flash` / `nvs.h` may appear **only** inside `components/storage`
backend files. The Application and all peers call the semantic interfaces
(`key_store.h`, `intent_log.h`) and never touch NVS directly.

### 1.3 Task ownership

| Task | Owner | Stack | Priority | Notes |
|---|---|---|---|---|
| `app_task` | Application | 4096 B | 5 | The **only** task that calls `comm_module_*` mailbox accessors |
| `comm_task` | Comm Module | 8192 B (default) | 5 | Spawned by `comm_module_start()`; never executes app code |
| `test_task` (FULL mode, mock LLI) | `main` | 8192 B | 5 | Mock-phone integration test; `MOCK_LLI_FOR_TESTING` only |

**Rule:** exactly one Application task. It runs the bounded-wait loop and never
blocks indefinitely. The comm task never executes application code (mailbox +
task-notification handoff).

---

## 2. Boundary rules (who may call what)

```
                     ┌──────────────────────────────────────┐
                     │         app_module                   │
                     │  (coordinator — business logic only) │
                     └───┬──────┬──────┬──────┬──────┬──────┘
                         │      │      │      │      │
              comm_module.h  aai.h  display.h integrity.h key_store.h
                         │      │      │      │      │   intent_log.h
                         ▼      ▼      ▼      ▼      ▼
                    Comm stack Actuator Display Integrity Storage
                    (frozen)   (peer)   (peer)  (peer)   (peer)
```

1. **The Application depends only on the facades:**
   - `comm_module.h` — the sole comm-stack header. The Application may NEVER
     include `session.h`, `transport.h`, `lli.h`, or any `pn532_*.h`.
     Enforced at build level: those are `PRIV_REQUIRES` of `comm_module`, so
     their include dirs never propagate.
   - `aai.h`, `display.h`, `integrity.h`, `key_store.h`, `intent_log.h` — the
     peer interfaces.
2. **Peers never talk to each other.** An actuator backend does not touch the
   display; integrity does not touch storage. All coordination flows through
   the Application.
3. **The Application contains no peripheral drivers and no register access.**
   GPIO config, motor PWM, display rendering, tamper sampling are all inside
   the peers.
4. **Application → comm is strictly via the mailbox accessors**:
   `comm_module_poll_event`, `comm_module_has_command`,
   `comm_module_get_command`, `comm_module_complete_response`. The Application
   never touches the mailbox struct (it is internal to `comm_module.c`).
5. **Peers report facts; the Application owns policy.**
   - Actuator reports bolt position / stall (`AAI_GetStatus`). The Application
     decides auto-reversal, intent logging, boot recovery.
   - Integrity reports tamper findings. The Application decides lockout /
     authorization refusal ("authentication is not authorization").
   - Display renders what the Application tells it (including the Provision
     Secret QR content). Display owns HOW, never WHAT/WHEN.
   - Storage persists what the Application commits; it owns HOW (RAM today,
     NVS / secure element later).

---

## 3. Init / boot sequence

Dependency order, implemented in `main/smart_lock_firmware.c`:

```
app_main (FULL_APPLICATION mode)
  │
  ├─ 1. Storage         KeyStore_Init() + IntentLog_Init()   (RAM placeholder)
  ├─ 2. Display         Display_Init()
  ├─ 3. Actuator        AAI_Init()                           (stub → LOCKED)
  ├─ 4. Integrity       Integrity_Init()
  ├─ 5. Comm module     comm_module_init(&cfg)
  │                       cfg.peer_key_provider = AppModule_GetPeerKeyByIndex
  ├─ 6. Application     AppModule_Init(&cfg)
  │                       └─ boot_recovery(): read IntentLog_GetTarget();
  │                          resolve intermediate bolt position
  ├─ 7. App task        AppModule_Start()
  │                       └─ app_task:
  │                            comm_module_register_app_task(self)
  │                            comm_module_start()
  │                            └─ bounded-wait loop (coordinator)
  └─ (MOCK) test_task   test_integration_run()
```

**Why this order:** the Application's boot recovery reads the intent log
(storage) and drives the actuator, so storage and actuator must be up first.
The comm module only needs its config + the peer-key provider, which reads the
key store — so storage precedes `comm_module_init`. The Application task must
exist before `comm_module_register_app_task`, which must precede
`comm_module_start`.

---

## 4. Async actuation + boot recovery flows

### 4.1 Tap-and-go (async actuation)

```
Phone taps → M1/M2/M3 handshake → SESSION_STARTED (advisory)
Phone sends CMD_UNLOCK (0x02)
App: verifies authorization (session key in key store)
App: replies 0x00 IMMEDIATELY (digital success)     ← phone disconnects
App: (on its own schedule)
     1. IntentLog_SetTarget(UNLOCKED)               ← BEFORE AAI_Open
     2. AAI_Open()                                  ← returns immediately
     3. poll AAI_GetStatus() every APP_ACTUATION_POLL_MS
        - reaches UNLOCKED → IntentLog_ClearTarget(),
                            Display_ShowIndication(SUCCESS)
        - JAMMED/FAULT    → s_last_error = MOTOR_STALL,
                            AAI_Stop(); AAI_Close() (auto-reversal,
                            never leave bolt partially engaged),
                            IntentLog_ClearTarget(),
                            Display_ShowIndication(ERROR)
```

The lock state reported by `CMD_GET_STATUS` always reflects the TRUE physical
state (from `AAI_GetStatus`), plus the last mechanical error — so the phone
syncs with reality on the next connection.

### 4.2 Boot recovery (power loss mid-actuation)

```
Boot:
  intent = IntentLog_GetTarget()
  phys   = AAI_GetStatus()
  if phys is LOCKED or UNLOCKED:
      clear stale intent; proceed normally
  if phys is MOVING:
      target = intent (default LOCKED for safety if none)
      AAI_Open()/AAI_Close() toward target
      poll until LOCKED/UNLOCKED (bounded by APP_BOOT_RECOVERY_TIMEOUT_MS)
      on JAMMED/FAULT: record MOTOR_STALL, clear intent
  if phys is JAMMED/FAULT/UNKNOWN:
      cannot auto-resolve → record fault; operator intervention
```

---

## 5. Command dispatch (Application_Module_Master.md §3)

All commands arrive as plaintext via the mailbox; every outcome is encoded in
the response bytes (`comm_module_complete_response` has no failure signal).

| Opcode | Request | Success response | Failure |
|---|---|---|---|
| `CMD_PROVISION` `0x01` | `OP ‖ SECRET(32) ‖ PHONE_PK(32)` (65 B) | `0x00 ‖ LOCK_PK(32)` (33 B) | `0x01` (secret/identity fail) |
| `CMD_UNLOCK` `0x02` | `OP` (1 B) | `0x00` (immediate, async actuation) | `0x02` unauth / `0x04` fault / `0x05` busy |
| `CMD_LOCK` `0x03` | `OP` (1 B) | `0x00` (immediate, async actuation) | same as unlock |
| `CMD_GET_STATUS` `0x04` | `OP` (1 B) | `0x00 ‖ BATTERY_PCT ‖ LOCK_STATE ‖ LAST_ERROR` (4 B) | `0x03` malformed |
| `CMD_REVOKE_KEY` `0x05` | `OP ‖ TARGET_PK(32)` (33 B) | `0x00` | `0x02` unauth / `0x09` not found |

Status bytes: `0x00` OK, `0x01` invalid secret, `0x02` unauthorized, `0x03`
invalid cmd, `0x04` fault, `0x05` busy, `0x06` internal, `0x07` key store
full, `0x08` key exists, `0x09` not found.

**Provisioning** (master doc §11): button press → CSPRNG secret →
`comm_module_arm_provisioning_window()` → QR via Display → phone taps and sends
`CMD_PROVISION`. The Application (1) compares the secret constant-time, (2)
calls `comm_module_provision_verify_identity(claimed_pk)` — the deferred
signature check over the cached M3 — and only then (3) commits the key and
disarms. **No code path ever skips the signature verification unconditionally.**
While armed, any command other than `CMD_PROVISION` aborts provisioning and
ends the session.

---

## 6. Test modes

Selected by the `Smart Lock Test Mode` choice (`main/Kconfig.projbuild`),
default `FULL_APPLICATION`. Each harness initializes ONLY the modules it
needs; every mode must build.

| Mode | Symbol | Initializes | Exercises |
|---|---|---|---|
| Full Application | `CONFIG_TEST_MODE_FULL_APPLICATION` | storage, display, actuator, integrity, comm, app | production path + (under `MOCK_LLI_FOR_TESTING`) the mock-phone provisioning integration test |
| Comm Only | `CONFIG_TEST_MODE_COMM_ONLY` | storage, comm | echo/status loop over the real NFC stack |
| Actuator Only | `CONFIG_TEST_MODE_ACTUATOR_ONLY` | actuator | `AAI_Open`/`AAI_Close`/`AAI_Stop` state machine |
| Display Only | `CONFIG_TEST_MODE_DISPLAY_ONLY` | display | indications, tones, QR render, clear |
| Integrity Only | `CONFIG_TEST_MODE_INTEGRITY_ONLY` | integrity | cadence checks, tamper status |

### How to run

```bash
# Default (FULL_APPLICATION) — also runs the mock-phone integration test
idf.py build flash monitor

# Single-module modes: flip the choice, rebuild
# (menuconfig, or edit sdkconfig as the helper below does)
idf.py menuconfig          # → Smart Lock Test Mode → pick mode
idf.py build flash monitor
```

The mock-phone integration test (`test_integration.c`) drives the full
M1→M2→M3 handshake through the mocked LLI and verifies the provisioning flow
(secret + identity verification → key committed → `0x00 ‖ LOCK_PK` response).

---

## 7. Swap-in plan (stub → real hardware)

Every backend swap is a **file replacement + Kconfig change**, never an
interface change. The Application and peers compile unchanged.

| Component | Interface (stable) | Current backend | Swap-in work |
|---|---|---|---|
| Actuator | `aai.h` (`AAI_Init/Open/Close/Stop/GetStatus`) | `aai_backend_stub.c` (default) | Implement `aai_backend_rmt.c` (stepper) or `aai_backend_mcpwm.c` (DC) — drive the motor, read limit switches/stall pin, keep the same `aai_backend_ops_t` table. Select `CONFIG_ACTUATOR_BACKEND_RMT/MCPWM`. |
| Display | `display.h` (indication/QR/tone/clear/text) | `display_backend_console.c` (default) | Implement `display_backend_panel.c` (SPI e-ink) and wire LEDs/buzzer; keep the same `display_backend_ops_t` table. Select `CONFIG_DISPLAY_BACKEND_PANEL`. |
| Integrity | `integrity.h` (init/run_checks/status) | `integrity_backend_stub.c` (default) | Implement `integrity_backend_tamper_gpio.c` (debounced GPIO read); keep the same `integrity_backend_ops_t` table. Select `CONFIG_INTEGRITY_BACKEND_TAMPER_GPIO`. |
| Storage | `key_store.h` / `intent_log.h` | `key_store_ram.c` / `intent_log_ram.c` (RAM, no persistence) | Implement the NVS backend and later the secure-element + RAM + write-policy backend behind the same interfaces. `KEY_STORE_ERR_STORAGE` / `INTENT_ERR_STORAGE` already express write-denied. Select `CONFIG_STORAGE_BACKEND_*`. |

**Storage seam contract (deliberately loose):** the Application's call sites,
opcode handlers, and `peer_key_provider` iterator depend only on
`key_store.h` / `intent_log.h`. The upcoming secure-element + RAM + write-policy
design is a backend drop-in: add the Kconfig option, implement the ops behind
the same headers, and the firmware behaves identically from the Application's
perspective — only persistence semantics improve.

**Battery level:** `CMD_GET_STATUS` reports `APP_BATTERY_PCT_DEFAULT` (100)
until a real ADC read lands (`TODO(hardware)`), following the same
"report facts honestly" philosophy.

---

## 8. Honest-stub rules

Every stub:

- logs what it **would** do with a `// TODO(hardware):` marker;
- **never reports success for an action it did not perform** — e.g. the RMT /
  MCPWM skeletons report `AAI_STATE_FAULT` / `AAI_ERR_FAULT`; the console
  display never claims a panel rendered;
- is swappable by config alone.

The RAM storage placeholder logs `// TODO(storage): provisional RAM backend —
no power-loss persistence` on every mutating call, so a developer relying on
persistence is never silently misled.
