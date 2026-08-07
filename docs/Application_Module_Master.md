# Application Module Master Document

This document outlines the design principles, workflows, and error handling strategies for the Smart Lock Application Module.

## 1. Asynchronous Actuation Workflow (Tap-and-Go)

NFC connections require the user's phone to remain in close physical proximity (within ~2cm) to the lock. Because mechanical actuation (especially via stepper motors) can take several seconds, forcing the user to hold their phone against the lock until the door physically opens provides a poor user experience and leads to high transaction failure rates (e.g., pulling the phone away prematurely).

To solve this, the Application Module implements an **Asynchronous Actuation Workflow**:

1. **Digital Authentication:** The phone taps the lock and completes the cryptographic handshake (M1-M3) within milliseconds.
2. **Command Dispatch:** The phone sends `CMD_UNLOCK`. The Application Module verifies the command and authorization.
3. **Immediate Reply:** The Application Module immediately queues a success response (`0x90 0x00`) back to the Communication Module.
4. **Disconnect:** The phone receives the success response, provides UX feedback (haptics/chime), and drops the NFC connection.
5. **Physical Actuation:** *After* the digital transaction concludes, the Application Module begins driving the physical motor to unlock the door.

## 2. Mechanical Error Handling

Because the digital transaction reports "Success" before the mechanical action completes, the lock must rely on its local hardware to manage physical failures (motor stalls, jams, power loss). 

The Application Module handles these physical errors using the following strategies:

### 2.1 Local Auditory & Visual Feedback
Since the user is physically present at the door, the lock's hardware serves as the primary user interface.
* **Success:** If the mechanical limit switch indicates successful unlock, the lock flashes a green LED and emits a pleasant success chime.
* **Failure:** If the motor stalls or fails to reach the target limit switch, the lock flashes a red LED and emits harsh error beeps. This immediately communicates the mechanical failure to the user, overriding the earlier digital success on their phone.

### 2.2 Auto-Reversal for Jams
If the stepper motor detects a stall (via a motor driver stall-detection pin or by failing to hit a limit switch within a defined timeout), the Application Module must:
* Cease driving the motor in the current direction.
* Immediately reverse the motor back to the fully locked state. 
* *Never leave the deadbolt partially engaged*, as this creates severe security vulnerabilities and physical usability issues.

### 2.3 Intent Logging (Power Loss Resilience)
If power is lost during actuation (e.g., batteries are removed), the lock will boot into an unknown mechanical state. To prevent undefined behavior:
1. **Pre-Actuation Intent:** Before starting the motor, write a `TARGET_STATE` to Non-Volatile Storage (NVS).
2. **Boot Verification:** On boot, `app_main` reads the limit switches. If the mechanical state is intermediate (neither fully locked nor fully unlocked), the lock checks the `TARGET_STATE` from NVS and attempts to safely resolve the state (e.g., reverting to `LOCKED` or completing the `UNLOCKED` motion).

### 2.4 State Synchronization & Event History
When the lock fails mechanically, it must report the truth on the *next* connection.
* When the companion app polls the lock via `CMD_GET_STATUS` or attempts a subsequent unlock, the Application Module reports the true physical state (e.g., `STATE = LOCKED, LAST_ERROR = MOTOR_STALL`).
* This allows the mobile app to sync with the lock's reality and display accurate error dialogs (e.g., "The lock failed to open during your last attempt due to a mechanical jam.") to the user.

## 3. Application Command Dispatch & Response Contracts

All commands arrive decrypted from the Session Layer as plaintext. The Application Module processes the command and returns a plaintext response buffer.

### 3.1 `CMD_PROVISION` (`0x01`)
* **Request:** `OPCODE (0x01) ‖ PROVISION_SECRET (32 bytes) ‖ PHONE_ED25519_PK (32 bytes)`
* **Response (Success):** `STATUS_BYTE (0x00) ‖ LOCK_ED25519_PK (32 bytes)`
* **Response (Failure):** `STATUS_BYTE (0x01 = APP_STATUS_INVALID_SECRET)`
* **Behavior:** Verifies the Provision Secret. If valid, verifies the cached M3 signature (`Sig_P`) against `PHONE_ED25519_PK`. If valid, persists `PHONE_ED25519_PK` into NVS, disarms the provisioning window, and returns the Lock's 32-byte Ed25519 public key so the phone can verify `M2` (`Sig_L`) and store the Lock's key.

### 3.2 `CMD_UNLOCK` (`0x02`)
* **Request:** `OPCODE (0x02)`
* **Response:** `STATUS_BYTE (0x00 = Success)`
* **Behavior:** Checks authorization of the authenticated session key. Replies immediately with success, then triggers asynchronous motor actuation to unlock.

### 3.3 `CMD_LOCK` (`0x03`)
* **Request:** `OPCODE (0x03)`
* **Response:** `STATUS_BYTE (0x00 = Success)`
* **Behavior:** Checks authorization. Replies immediately with success, then triggers asynchronous motor actuation to lock.

### 3.4 `CMD_GET_STATUS` (`0x04`)
* **Request:** `OPCODE (0x04)`
* **Response:** `STATUS_BYTE (0x00) ‖ BATTERY_PCT (1 byte) ‖ LOCK_STATE (1 byte) ‖ LAST_ERROR (1 byte)`
* **Behavior:** Returns current hardware, bolt, battery, and diagnostic error states.

### 3.5 `CMD_REVOKE_KEY` (`0x05`)
* **Request:** `OPCODE (0x05) ‖ TARGET_PHONE_PK (32 bytes)`
* **Response:** `STATUS_BYTE (0x00 = Success)`
* **Behavior:** Removes the target phone public key from NVS.

## 4. Physical Provision Button Architecture (`provision_button.c` / `provision_button.h`)

To gate provisioning behind physical presence, the lock includes a dedicated event-driven **Provision Button** driver.

### 4.1 Architecture & Doorbell Pattern
* **ISR & Task Doorbell:** Pressing the physical button triggers a falling-edge GPIO interrupt (`provision_button_isr`). The ISR performs non-blocking validation and sends a FreeRTOS task notification (`xTaskNotifyFromISR`) to the `app_task` event loop.
* **Debounce Filtering:** Software debouncing (`APP_PROVISION_BUTTON_DEBOUNCE_MS`, default 50 ms) ignores spurious mechanical contact bounces.
* **Press-and-Hold Threshold:** To prevent accidental arming, the user must hold the button for `APP_PROVISION_BUTTON_HOLD_MS` (default 3000 ms). If released prematurely, the event is discarded.
* **Arming Action:** Upon reaching the hold threshold, `provision_mgr_arm_window()` is invoked:
  1. Generates a fresh 32-byte CSPRNG Provision Secret via `esp_fill_random`.
  2. Arms the Communication Module provisioning window for 60 seconds (`APP_PROVISION_WINDOW_MS`).
  3. Renders the Provision Secret as an ASCII QR code via `Display_RenderQR`.

### 4.2 Kconfig Parameters
| Parameter | Default | Purpose |
|---|---|---|
| `CONFIG_APP_PROVISION_BUTTON_GPIO` | `17` (`-1` = disabled) | GPIO pin for active-low provision button |
| `CONFIG_APP_PROVISION_BUTTON_HOLD_MS` | `2000` | Minimum press-and-hold duration (ms) |
| `CONFIG_APP_PROVISION_BUTTON_DEBOUNCE_MS` | `30` | Hardware ISR debounce interval (ms) |

## 5. Production Cleanup (after all testing)

The Application Module exposes two init functions — `AppModule_GetCommConfig()`
(assembles the comm config, including the lock identity) and
`AppModule_Init()` (caches the lock PK, runs boot recovery). This split exists
so the `COMM_ONLY` test harness can assemble the config without pulling in
actuator-dependent boot recovery.

**For production (after all test modes are retired): merge the two functions
into one**, e.g. `AppModule_InitAndGetCommConfig()`, that assembles the config,
loads/generates the identity, runs boot recovery, and returns the finalized
config. `main` then becomes:

```c
comm_module_config_t cfg = AppModule_InitAndGetCommConfig();
comm_module_init(&cfg);   /* snapshots local_sk/local_pk — identity ready */
```

This makes the "config finalized before `comm_module_init`" ordering
**structural** instead of commented, and removes the need for `main` to know
the identity-init ordering at all. The merge is only deferred because
`COMM_ONLY` needs config assembly without boot recovery — once the harnesses
are gone, there is no reason to keep the two-step sequence.



