# Hardware Test Notes — PN532 recovery + cold-boot workarounds

Status: **RESOLVED — driver fixes landed** (`components/pn532/`). The `main/`
workaround (`ensure_pn532_released()`) has been removed. This file keeps the
historical analysis for reference; the fix is now in the driver.

Summary of the landed fixes:

1. **Cold-boot probe race (§1)** — `pn532_i2c_create` now pulses RST
   (`PN532_RST_PULSE_MS` LOW, `PN532_RST_SETTLE_MS` HIGH) BEFORE the bus probe,
   so the chip is deterministically reset + oscillator-started before probing.
   No app-side RST release needed; power-cycle/capacitor-drain ritual obsolete.
2. **Wedged-bus recovery (§2/§2b)** — `recover_i2c_bus` now always resets the
   PN532 hardware (regardless of SDA state) AND calls `i2c_master_bus_reset()`
   to resync the ESP-IDF I2C controller; `pn532_i2c_write` escalates through a
   device-handle rebuild and, after `PN532_RECOVERY_THRESHOLD` consecutive
   failures, returns a distinct fatal `ESP_ERR_INVALID_STATE`.
3. **Fatal propagation (smart-lock side)** — `LLI_ERR_BUS_FATAL` →
   `TRANSPORT_ERR_BUS_FATAL` → the comm task logs once and exits instead of
   looping forever on `TRANSPORT: activate failed: 6`.
4. **Boot-time SDA recovery** — `pn532_i2c_create` now checks SDA after the RST
   pulse. If a wedged PN532 from a previous session is holding SDA LOW across a
   reboot (the ESP32 reset button does NOT reset the chip — RST is a GPIO), the
   bus is bit-bang released (up to 9 SCL pulses + STOP) and the I2C controller
   resynced with `i2c_master_bus_reset()` before the probe. Fixes the
   `probe device timeout` on reboot-after-wedge; no capacitor drain needed.
5. **Full bus-rebuild escalation (final)** — the runtime wedge can leave the
   ESP-IDF I2C controller's hardware FSM stuck in a way that survives
   `i2c_master_bus_reset()` + device rebuild (every transmit returns
   `ESP_ERR_INVALID_STATE`; the `i2c_ll_is_bus_busy()` condition persists).
   `pn532_i2c_write` now escalates to **tearing down and recreating the entire
   I2C bus + device** (`rebuild_i2c_bus`, using the bus config retained in the
   context) before reporting the fatal error. This fully resets the I2C
   peripheral and internal driver state.

---

## 1. Cold-boot "PN532 not found at 0x24"

### Symptom

```
E (329) PN532: PN532 not found on I2C bus at 0x24: ESP_ERR_NOT_FOUND
E (329) LLI: pn532_i2c_create failed: ESP_ERR_NOT_FOUND
E (339) TRANSPORT: lli_init failed: 6
E (339) COMM_MOD: transport_init failed: 6
```

### Root cause

`pn532_i2c_create` releases RST (`gpio_set_level(rst, 1)`) and then **immediately
probes the bus** — no settle delay for the PN532's cold-boot oscillator start.
The probe runs against a chip still starting up, so the address NACKs.

Also: the RST pin is configured **after** the probe, so the chip may be held in
reset (floating/low RST) during the probe. Sequencing is backwards.

### Workaround (in `main/smart_lock_firmware.c`)

`ensure_pn532_released()` drives RST (GPIO 11) high and waits
`APP_PN532_RST_SETTLE_MS` (100 ms) **before** `comm_module_init`, in both
`FULL_APPLICATION` and `COMM_ONLY` paths.

The capacitor issue: a charged PN532 power rail can hold the chip in a bad
state across a quick reset — power down and wait for the rail to drain
(~minutes) before re-flashing.

### Driver-side fix (deferred)

In `pn532_i2c_create`:
1. Configure + release RST **before** the probe.
2. Add a cold-boot settle delay (e.g. 100 ms) after releasing RST.
3. Then probe.

Remove the `ensure_pn532_released()` workaround from `main/` once this lands.

---

## 2. No recovery from a wedged I2C / ISO-DEP state

### Symptom (after a phone tap corrupts the exchange)

```
W (158439) LLI: TgGetData status=0x09 (frame integrity)
W (158439) TRANSPORT: activated receive failed: 2
...
W (163409) PN532: write failed after 5 retries, attempting bus recovery
E (163459) PN532: write failed even after bus recovery: ESP_ERR_INVALID_STATE
E (163459) PN532: command write failed: ESP_ERR_TIMEOUT
W (163709) PN532: write failed after 5 retries, attempting bus recovery
E (163759) PN532: write failed even after bus recovery: ESP_ERR_INVALID_STATE
... (repeats forever, every ~260 ms) ...
E (172069) PN532: write failed even after bus recovery: ESP_ERR_INVALID_STATE
```

### What happens

1. A phone tap produces frame-integrity errors (`TgGetData` status 0x09/0x13/0x0B)
   — corrupt/partial ISO-DEP frames.
2. A subsequent I2C write fails 5× → `pn532_i2c_write` calls `recover_i2c_bus()`.
3. `recover_i2c_bus` checks SDA: if SDA is **high** (not stuck), it returns
   **immediately without resetting the PN532** (line ~322-326). The PN532 is
   left in whatever broken mid-frame ISO-DEP state it was in.
4. If SDA *was* stuck, the bit-banged 9-pulse recovery desyncs the **ESP-IDF
   I2C master driver's internal state** (the driver still thinks a transaction
   is pending). Every subsequent `i2c_master_transmit` on the device handle
   returns `ESP_ERR_INVALID_STATE` **permanently**.
5. The one final post-recovery transmit fails → the driver gives up. Every
   later `configure_pn532` / `SAMConfiguration` fails the same way →
   `TRANSPORT: activate failed: 6` → loop repeats forever.

The `ESP_ERR_INVALID_STATE` is a **stuck driver state**, not a transient chip
NACK — nothing the frozen stack does after that point can clear it. The chip
never gets a hardware reset because `recover_i2c_bus` only resets when it
thinks SDA is stuck, and even then the driver state is already corrupted.

### Immediate recovery (test-side, today)

- **Reboot the board** (`Ctrl+]` + re-flash, or power-cycle and wait for the
  rail to drain). This is the only reliable recovery with the current frozen
  driver.
- Avoid leaving a phone over the antenna during a failed exchange; the
  frame-integrity errors are what start the spiral.

### Driver-side fix (deferred — this is the real fix)

In the PN532 driver repo, rework `pn532_i2c_write`'s recovery path:

1. **Always hardware-reset the PN532 after recovery** (RST pulse when
   `rst_gpio` is wired), not only when SDA was stuck — the chip's ISO-DEP
   state machine must be cleared regardless.
2. **Reset the ESP-IDF I2C master controller after bit-banging** — call
   `i2c_master_bus_reset(c->bus)` (or delete + recreate the bus and device) so
   the driver's state matches the physical bus again. Reconnecting the pins
   with `esp_rom_gpio_connect_*` is not enough.
3. **Escalate on persistent failure**: if the post-recovery transmit still
   returns `ESP_ERR_INVALID_STATE`, tear down and recreate the device (or
   bus) and retry once more before giving up.
4. **Cap consecutive recovery attempts** (e.g. N failures → hard fault / stop
   the comm task) instead of looping and spamming forever.

### What NOT to do (frozen stack constraint)

The app cannot recreate the I2C device — it is owned by the frozen
`comm_module`/`pn532` stack and not reachable from `main/`. Pulsing RST from
the app does **not** clear the driver's `ESP_ERR_INVALID_STATE` (it resets the
chip, not the ESP32's I2C controller state). Hence: reboot, or fix the driver.

---

## 2b. Datapoint (2026-08-06): wedge after a *successful* session

### Symptom

A full session completed correctly first (provisioning from the prior boot
stored a key; this tap authenticated normally, dispatched CMD_UNLOCK, and the
stub actuated), and THEN the wedge started at the next receive:

```
I (447029) PN532_CMD: TgInitAsTarget: activated, mode=0x08
I (448789) APP_MOD: === SESSION STARTED (advisory) ===
W (448879) INTENT_LOG: TODO(storage): RAM placeholder — intent is NOT persisted
I (448879) AAI_STUB: TODO(hardware): [stub] would open (drive to UNLOCKED) ...
W (448989) LLI: TgGetData status=0x13 (frame integrity)
W (448989) TRANSPORT: secure receive failed: 2
I (448989) APP_MOD: === SESSION ENDED (advisory) ===
I (449549) PN532_CMD: TgInitAsTarget: activated, mode=0x08
W (449629) LLI: TgGetData status=0x0B (frame integrity)
W (449629) TRANSPORT: activated receive failed: 2
W (449849) PN532: write failed after 5 retries, attempting bus recovery
E (449899) PN532: write failed even after bus recovery: ESP_ERR_INVALID_STATE
... (repeats forever, every ~260 ms) ...
```

### Analysis — same root cause, different trigger moment

- **Same underlying issue as §2**: a corrupt/partial ISO-DEP frame (`TgGetData`
  status `0x13`, then `0x0B`) → the next I2C write wedges the ESP-IDF I2C
  driver (`ESP_ERR_INVALID_STATE` forever). Nothing in the app caused it: the
  CMD_UNLOCK dispatch, intent-log write, and `AAI_Open()` are all app-task,
  non-blocking, zero I2C.
- **New trigger moment**: this time the corrupt frame arrived AFTER a fully
  successful exchange — on the read following the CMD_UNLOCK reply, during the
  phone's teardown / a follow-up exchange. The phone was likely still moving
  away from the antenna when it sent (or the lock tried to read) the next
  frame, producing `0x13`, then a re-activation got `0x0B`.
- **Implication**: this is not "the session logic broke the bus" — it's the
  physical tap/pull-away timing. The tap-and-go teardown (phone pulls away
  right after the digital reply, per Application_Module_Master.md §1) is
  exactly the risky window: the PN532 may be mid-frame when the field drops.

### Practical guidance for the test loop

- A **clean pull-away** (hold the phone until the lock has finished replying,
  then remove it) avoids the wedge; yanking the phone mid-exchange triggers it.
- After the wedge, **reboot** (power-cycle + drain) is still the only recovery.
- The wedge is a **frozen-driver bug to fix driver-side** (§2 fix list), not an
  application defect — the full protocol was verified successfully before it
  fired.

---

## 3. DRIVER FIX HANDOFF — self-contained context for the PN532 driver repo

Everything below is the frozen-stack context needed to fix the driver side.
The `components/pn532` sources referenced here are the current as-built files.

### 3.1 The layering violation in `main/` (to be removed)

`main/smart_lock_firmware.c` currently has `ensure_pn532_released()` (GPIO 11)
called before `comm_module_init`. This **violates the stack boundary** — GPIO 11
(PN532 RST) is owned by the comm stack (`pn532_i2c_create` configures it). It is
a test-enabling workaround for the driver bugs below, NOT a permanent feature.
**Delete it (both calls + the helper + `esp_driver_gpio` from main's
PRIV_REQUIRES) once the driver fix lands.**

### 3.2 Bug A — cold-boot probe race (`pn532_i2c_create`, ~lines 467–581)

**Current order in `pn532_i2c_create`:**
1. Create bus + add device (500–529)
2. **Probe** the bus (`i2c_master_probe`, line 534)  ← runs first
3. Configure IRQ (543)
4. **Configure RST** (`if (c->rst_gpio >= 0)`, line 556) ← runs LAST, just sets
   level high, no pulse, no settle

**Consequences:**
- The probe can run while the PN532 is still in reset (RST floating/low) or
  still booting → "PN532 not found at 0x24".
- The PN532's RST is a **GPIO output, not the ESP32's EN rail** → pressing the
  ESP32 reset button never resets the PN532, so a wedged chip survives a hard
  reset (requires power-cycle + capacitor drain to recover).

**Fix: reorder + pulse + settle, then probe.**
1. Configure RST as output (move the `if (c->rst_gpio >= 0)` block BEFORE the
   probe).
2. Pulse it: drive LOW, `vTaskDelay` ~50 ms, drive HIGH.
3. `vTaskDelay` ~100 ms cold-boot settle.
4. THEN `i2c_master_probe`.

This removes the need for both the `main/` workaround AND the capacitor-drain
ritual (the PN532 gets a real reset on every boot).

### 3.3 Bug B — no recovery from a wedged ISO-DEP / I2C state

**Fault path:** a corrupt ISO-DEP frame (`TgGetData` status 0x09/0x13/0x0B)
→ next `pn532_i2c_write` fails 5× → `recover_i2c_bus()` (line 317) runs → then
`ESP_ERR_INVALID_STATE` forever, every ~260 ms.

**Why `recover_i2c_bus` (lines 317–386) fails to recover:**
1. If **SDA is high** (not stuck), it returns immediately (line ~323) WITHOUT
   resetting the PN532 — the chip keeps its broken ISO-DEP state.
2. If **SDA was stuck**, the bit-banged 9-pulse recovery (line 336) desyncs the
   **ESP-IDF I2C master driver** (the driver thinks a transaction is pending).
   Reconnecting pins via `esp_rom_gpio_connect_*` (lines 359–362) does NOT clear
   the driver state.
3. The single post-recovery transmit (line 179) then fails → `pn532_i2c_write`
   returns `ESP_ERR_TIMEOUT` → `configure_pn532`/`SAMConfiguration` fails
   forever → `TRANSPORT: activate failed: 6` → repeat.

**Fix (in `pn532_i2c_write`'s recovery path / `recover_i2c_bus`):**
1. **Always hardware-reset the PN532** after recovery (RST pulse when
   `rst_gpio` is wired), not only when SDA was stuck.
2. **Reset the ESP-IDF I2C master controller after bit-banging** —
   `i2c_master_bus_reset(c->bus)` (or delete + recreate bus/device) so the
   driver's transaction state matches the physical bus. This is the key step
   that clears `ESP_ERR_INVALID_STATE`.
3. **Escalate on persistent failure**: if the post-recovery transmit still
   returns `ESP_ERR_INVALID_STATE`, recreate the device (or bus) and retry once
   before giving up.
4. **Cap consecutive recovery failures** (e.g. N → hard fault / stop the comm
   task) instead of looping and spamming forever.

### 3.4 Test evidence (both bugs observed on real hardware)

- **Bug A:** boot log `PN532 not found on I2C bus at 0x24: ESP_ERR_NOT_FOUND`
  (works only after power-cycle + drain).
- **Bug B:** after a phone tap teardown: `TgGetData status=0x13/0x0B` →
  `write failed even after bus recovery: ESP_ERR_INVALID_STATE` → repeat
  forever. Confirmed on BOTH a mid-provision tap AND a successful-session
  teardown (§2b).
- After Bug B, the only recovery is power-cycle + capacitor drain; a hard reset
  button press does NOT recover (PN532 RST is GPIO-driven, not on the EN rail).

### 3.5 How to verify the fix

- **Bug A:** flash → expect `PN532 detected on I2C bus at 0x24` on the FIRST
  boot after a hard reset (no power-cycle, no drain).
- **Bug B:** tap + pull phone away mid-exchange → expect recovery (SAM
  re-configured and the 30 s `TgInitAsTarget` wait resumes) instead of the
  `ESP_ERR_INVALID_STATE` loop.
- Remove the `main/` workaround and confirm the stack still initialises clean.

---

## Related notes

- **Polling mode**: `irq_gpio = -1` / `rst_gpio = -1` in `build_comm_config()`
  is fully supported by the stack (1 ms status polling, no ISR, no reset
  line). Useful to isolate wiring issues, but with no RST line the driver's
  only recovery fallback is a wake sequence — even weaker than today.
- **Capacitor / power**: if the PN532 doesn't answer after a reset, power down
  and wait for the rail to drain before assuming a code problem.
