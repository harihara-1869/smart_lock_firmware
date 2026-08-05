/*
 * Smart Lock Firmware
 * Copyright (C) 2026 Harihara
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file provision_mgr.h
 * @brief Provisioning workflow state machine.
 *
 * The Application owns provisioning end-to-end (master doc §10/§11): the
 * button press, the CSPRNG Provision Secret, arming the comm provisioning
 * window, QR display, secret comparison, deferred identity verification, and
 * committing the new key to the key store. Comms' only role is the one-shot
 * handshake exception + deferred signature check.
 *
 * Response layouts follow Application_Module_Master.md §3.1:
 *   success: 0x00 ‖ LOCK_PK(32)
 *   failure: 0x01 (APP_STATUS_INVALID_SECRET)
 *
 * While a window is armed, the Application accepts ONLY CMD_PROVISION; any
 * other command aborts provisioning and ends the session (the Application
 * calls provision_mgr_abort()).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Arm the provisioning window: generate a fresh CSPRNG secret, arm the comm
 * module's one-shot provisioning window, and render the secret as a QR via
 * the Display peer.
 *
 * @param timeout_ms  Application-side window validity.
 * @return true if armed, false if the comm module refused to arm.
 */
bool provision_mgr_arm(uint32_t timeout_ms);

/** @return true while the Application's provisioning window is armed and unexpired. */
bool provision_mgr_is_active(void);

/**
 * Handle an incoming command while the window is armed. Accepts ONLY
 * CMD_PROVISION; anything else aborts provisioning (see provision_mgr_abort)
 * and writes APP_STATUS_INVALID_CMD into resp.
 *
 * On a successful provision the new key is committed to the key store and the
 * window is disarmed; resp receives 0x00 ‖ LOCK_PK(32). On failure resp
 * receives 0x01. The window is single-use: after ONE provision attempt
 * (success or failure) it closes, so a second CMD_PROVISION gets
 * APP_STATUS_INVALID_CMD.
 *
 * @param cmd_bytes  Plaintext command.
 * @param len        Command length.
 * @param resp_bytes Response buffer (capacity APP_RESPONSE_BUF).
 * @param resp_len   [out] Response length.
 */
void provision_mgr_handle_cmd(const uint8_t *cmd_bytes, size_t len,
                              uint8_t *resp_bytes, size_t *resp_len);

/**
 * Abort provisioning immediately: disarm the window, clear the display, and
 * ask the comm module to terminate the session (per §11.2.1). Call after any
 * non-CMD_PROVISION command arrives during an armed window.
 */
void provision_mgr_abort(void);

/**
 * @return pointer to the current Provision Secret (valid only while armed).
 */
const uint8_t *provision_mgr_get_secret(void);

/**
 * @return the lock's 32-byte Ed25519 public key, for the CMD_PROVISION
 *         success response. Owned by the Application.
 */
const uint8_t *provision_mgr_get_lock_pk(void);

#ifdef __cplusplus
}
#endif
