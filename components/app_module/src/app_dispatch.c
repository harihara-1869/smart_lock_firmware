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

#include "app_dispatch.h"

#include <string.h>

#include "app_cmd.h"
#include "provision_mgr.h"

/* Whether a command may be acted on in the current physical state. */
static bool lock_state_allows_actuation(uint8_t state)
{
    switch (state) {
    case APP_LOCK_STATE_LOCKED:
    case APP_LOCK_STATE_UNLOCKED:
        return true;
    default:
        /* MOVING / JAMMED / FAULT / UNKNOWN: do not start another actuation. */
        return false;
    }
}

bool app_dispatch(const uint8_t *cmd, size_t cmd_len,
                  const app_dispatch_ctx_t *ctx,
                  uint8_t *resp, size_t *resp_len)
{
    *resp_len = 0;
    if (!cmd || !ctx || !resp || !resp_len) {
        return false;
    }
    if (cmd_len == 0) {
        resp[0] = APP_STATUS_INVALID_CMD;
        *resp_len = 1;
        return true;
    }

    /* Provisioning window: accept ONLY CMD_PROVISION; anything else aborts
     * provisioning and ends the session (§11.2.1). */
    if (ctx->provisioning_active) {
        if (cmd[0] != CMD_PROVISION) {
            provision_mgr_abort();
            resp[0] = APP_STATUS_INVALID_CMD;
            *resp_len = 1;
            return true;
        }
        provision_mgr_handle_cmd(cmd, cmd_len, resp, resp_len);
        return true;
    }

    switch (cmd[0]) {
    case CMD_UNLOCK:
    case CMD_LOCK: {
        if (cmd_len != 1) {
            resp[0] = APP_STATUS_INVALID_CMD;
            *resp_len = 1;
            break;
        }
        if (!ctx->session_authorized) {
            resp[0] = APP_STATUS_UNAUTHORIZED;
            *resp_len = 1;
            break;
        }
        if (!lock_state_allows_actuation(ctx->lock_state)) {
            resp[0] = APP_STATUS_BUSY;
            *resp_len = 1;
            break;
        }
        /* Reply 0x00 IMMEDIATELY; the physical actuation happens
         * asynchronously after the digital reply (§1 tap-and-go). */
        resp[0] = APP_STATUS_OK;
        *resp_len = 1;
        break;
    }

    case CMD_GET_STATUS: {
        if (cmd_len != 1) {
            resp[0] = APP_STATUS_INVALID_CMD;
            *resp_len = 1;
            break;
        }
        resp[0] = APP_STATUS_OK;
        resp[1] = ctx->battery_pct;
        resp[2] = ctx->lock_state;
        resp[3] = ctx->last_error;
        *resp_len = 4;
        break;
    }

    case CMD_REVOKE_KEY: {
        if (cmd_len != (1 + APP_KEY_LEN)) {
            resp[0] = APP_STATUS_INVALID_CMD;
            *resp_len = 1;
            break;
        }
        /* Revocation is decided by the Application (policy), not by the
         * session's authenticated identity alone. */
        if (!ctx->session_authorized) {
            resp[0] = APP_STATUS_UNAUTHORIZED;
            *resp_len = 1;
            break;
        }
        /* Key removal is performed by the Application task after dispatch
         * returns (app_dispatch cannot touch the key store; it is pure
         * protocol). The response is filled optimistically; the task corrects
         * it if removal fails. */
        resp[0] = APP_STATUS_OK;
        *resp_len = 1;
        break;
    }

    case CMD_PROVISION:
        /* No armed window: a bare CMD_PROVISION is invalid. */
        resp[0] = APP_STATUS_INVALID_CMD;
        *resp_len = 1;
        break;

    default:
        resp[0] = APP_STATUS_INVALID_CMD;
        *resp_len = 1;
        break;
    }

    return true;
}
