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

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sda_gpio;
    int scl_gpio;
    int irq_gpio;       // -1 if not used
    int rst_gpio;       // -1 if not used
    int i2c_port;       // e.g. I2C_NUM_0

    uint8_t sens_res[2];
    uint8_t nfcid1[3];
    uint8_t sel_res;
    uint8_t nfcid2[8];
    uint8_t pad[8];
    uint8_t system_code[2];
    uint8_t nfcid3t[10];
    uint8_t gt[47];
    uint8_t gt_len;
    uint8_t tk[47];
    uint8_t tk_len;
} lli_config_t;

typedef enum {
    LLI_STATUS_ACTIVE,
    LLI_STATUS_RELEASED,
    LLI_STATUS_ERROR,
} lli_link_status_t;

typedef enum {
    LLI_OK                    = 0,
    LLI_ERR_TIMEOUT           = 1,
    LLI_ERR_FRAME_INTEGRITY   = 2,
    LLI_ERR_SEND_FAILED       = 3,
    LLI_ERR_NOT_SUPPORTED     = 4,
    LLI_ERR_INVALID_ARG       = 5,
    LLI_ERR_INTERNAL          = 6,
    LLI_ERR_LINK_RELEASED     = 7,
} lli_err_t;

typedef struct lli_t *lli_handle_t;

lli_err_t lli_init(const lli_config_t *cfg, lli_handle_t *handle_out);
lli_err_t lli_deinit(lli_handle_t handle);

lli_err_t lli_activate(lli_handle_t handle, uint32_t timeout_ms);
lli_err_t lli_receive_apdu(lli_handle_t handle, uint8_t *buf, size_t buf_len,
                           size_t *len_out, uint32_t timeout_ms);
lli_err_t lli_send_apdu(lli_handle_t handle, const uint8_t *data,
                        size_t len, uint32_t timeout_ms);
lli_link_status_t lli_get_link_status(lli_handle_t handle);
lli_err_t lli_abort(lli_handle_t handle);

#ifdef __cplusplus
}
#endif
