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
 * @file aai_backend_mcpwm.c
 * @brief MCPWM DC-motor backend.
 *
 * TODO(hardware): NOT implemented — this is an honest skeleton. It compiles
 * when ACTUATOR_BACKEND_MCPWM is selected, logs that no motor driver is wired,
 * and reports AAI_STATE_FAULT / AAI_ERR_FAULT rather than faking a working
 * actuator. Implementation lands with the motor driver + wiring (MCPWM
 * channel pair for the H-bridge, limit switches for position).
 */

#include "aai_backend.h"

#include "esp_log.h"

static const char *TAG = "AAI_MCPWM";

static aai_err_t mcpwm_init(void)
{
    ESP_LOGE(TAG, "TODO(hardware): MCPWM backend not implemented — "
                  "no motor driver present; actuator is in FAULT state");
    return AAI_ERR_FAULT;
}

static aai_err_t mcpwm_open(void)
{
    ESP_LOGE(TAG, "TODO(hardware): MCPWM open not implemented");
    return AAI_ERR_FAULT;
}

static aai_err_t mcpwm_close(void)
{
    ESP_LOGE(TAG, "TODO(hardware): MCPWM close not implemented");
    return AAI_ERR_FAULT;
}

static aai_err_t mcpwm_stop(void)
{
    return AAI_OK; /* nothing driving; stop is trivially satisfiable */
}

static aai_state_t mcpwm_get_status(void)
{
    /* No motor, no limit switches: the only honest answer is FAULT. */
    return AAI_STATE_FAULT;
}

const aai_backend_ops_t aai_backend_mcpwm_ops = {
    .init       = mcpwm_init,
    .open       = mcpwm_open,
    .close      = mcpwm_close,
    .stop       = mcpwm_stop,
    .get_status = mcpwm_get_status,
};
