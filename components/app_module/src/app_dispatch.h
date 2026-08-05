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
 * @file app_dispatch.h
 * @brief Pure command dispatch — builds responses from plaintext commands.
 *
 * This module has NO hardware, NO peers, NO FreeRTOS — it is pure protocol
 * logic over the opcode/status/layout constants in app_cmd.h, so it can be
 * unit-tested without a board. The Application task feeds it decrypted
 * commands and forwards the encoded responses to comm_module_complete_response.
 *
 * Every application-level outcome is encoded into resp_bytes; there is no
 * "this failed" signal outside the bytes.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Dispatch context: everything the dispatch needs to know ------- */

typedef struct {
    /* Commands that actuate need the current physical state. */
    uint8_t  lock_state;          /* app_lock_state_t         */
    uint8_t  last_error;          /* app_last_error_t         */
    uint8_t  battery_pct;         /* 0..100                   */

    /* Authorization: is the session's resolved peer key in the store? */
    bool     session_authorized;

    /* The lock's Ed25519 public key for the CMD_PROVISION success response. */
    const uint8_t *lock_pk;       /* 32 bytes                 */

    /* Provisioning window state (handled by provision_mgr). */
    bool     provisioning_active;
} app_dispatch_ctx_t;

/**
 * Dispatch one plaintext command and build the response.
 *
 * @param cmd       Decrypted command bytes (OPCODE ‖ ARGS).
 * @param cmd_len   Command length.
 * @param ctx       Dispatch context (current state, authorization, ...).
 * @param resp      Response buffer (capacity APP_RESPONSE_BUF).
 * @param resp_len  [out] Response length written.
 * @return true if a non-empty response was produced (always true unless the
 *         command buffer was oversized).
 */
bool app_dispatch(const uint8_t *cmd, size_t cmd_len,
                  const app_dispatch_ctx_t *ctx,
                  uint8_t *resp, size_t *resp_len);

#ifdef __cplusplus
}
#endif
