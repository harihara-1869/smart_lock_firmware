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

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lli.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRANSPORT_STATE_IDLE,
    TRANSPORT_STATE_ACTIVATED,
    TRANSPORT_STATE_HANDSHAKE,
    TRANSPORT_STATE_SECURE_SESSION,
    TRANSPORT_STATE_RELEASED,
} transport_state_t;

typedef enum {
    TRANSPORT_OK                    = 0,
    TRANSPORT_ERR_TIMEOUT           = 1,
    TRANSPORT_ERR_LINK_LOST         = 2,
    TRANSPORT_ERR_INVALID_STATE     = 3,
    TRANSPORT_ERR_INVALID_APDU      = 4,
    TRANSPORT_ERR_PAYLOAD_TOO_LARGE = 5,
    TRANSPORT_ERR_INTERNAL          = 6,
} transport_err_t;

typedef struct {
    uint8_t cla;
    uint8_t ins;
    uint8_t p1;
    uint8_t p2;
    uint8_t lc;
    uint8_t data[255];
} transport_capdu_t;

typedef struct {
    uint8_t data[256];
    uint8_t len;
    uint8_t sw1;
    uint8_t sw2;
} transport_rapdu_t;

typedef transport_err_t (*transport_apdu_handler_t)(
    const transport_capdu_t *capdu,
    transport_rapdu_t *rapdu,
    void *user_ctx
);

typedef void (*transport_erase_handler_t)(void *user_ctx);

typedef struct {
    lli_config_t    lli_cfg;
    uint32_t        handshake_timeout_ms;
    uint32_t        activate_timeout_ms;
    uint32_t        apdu_timeout_ms;
    transport_apdu_handler_t  on_apdu;
    transport_erase_handler_t on_erase;
    void           *user_ctx;
} transport_config_t;

typedef struct transport_t *transport_handle_t;

transport_err_t transport_init(const transport_config_t *cfg,
                               transport_handle_t *handle_out);
transport_err_t transport_deinit(transport_handle_t handle);

transport_err_t transport_run_session(transport_handle_t handle);

transport_state_t transport_get_state(transport_handle_t handle);

#ifdef __cplusplus
}
#endif
