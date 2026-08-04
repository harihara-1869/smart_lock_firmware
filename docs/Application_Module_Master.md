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

