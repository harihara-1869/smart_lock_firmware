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
 * @file aai_backend_rmt.c
 * @brief RMT stepper backend (step/dir).
 *
 * TODO(hardware): NOT implemented — this is an honest skeleton. It compiles
 * when ACTUATOR_BACKEND_RMT is selected, logs that no motor driver is wired,
 * and reports AAI_STATE_FAULT / AAI_ERR_FAULT rather than faking a working
 * actuator. Implementation lands with the motor driver + wiring (RMT channel
 * for STEP pulses, GPIO for DIR, limit switches for position).
 */

#include "aai_backend.h"

#include "esp_log.h"

static const char *TAG = "AAI_RMT";

static aai_err_t rmt_init(void)
{
    ESP_LOGE(TAG, "TODO(hardware): RMT stepper backend not implemented — "
                  "no motor driver present; actuator is in FAULT state");
    return AAI_ERR_FAULT;
}

static aai_err_t rmt_open(void)
{
    ESP_LOGE(TAG, "TODO(hardware): RMT open not implemented");
    return AAI_ERR_FAULT;
}

static aai_err_t rmt_close(void)
{
    ESP_LOGE(TAG, "TODO(hardware): RMT close not implemented");
    return AAI_ERR_FAULT;
}

static aai_err_t rmt_stop(void)
{
    return AAI_OK; /* nothing driving; stop is trivially satisfiable */
}

static aai_state_t rmt_get_status(void)
{
    /* No motor, no limit switches: the only honest answer is FAULT. */
    return AAI_STATE_FAULT;
}

const aai_backend_ops_t aai_backend_rmt_ops = {
    .init       = rmt_init,
    .open       = rmt_open,
    .close      = rmt_close,
    .stop       = rmt_stop,
    .get_status = rmt_get_status,
};
