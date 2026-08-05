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
 * @file integrity_backend_stub.c
 * @brief Simulated integrity backend (default).
 *
 * TODO(hardware): no tamper sensor. Reports INTEGRITY_OK unless
 * INTEGRITY_SIM_TAMPER is enabled, in which case it reports
 * INTEGRITY_TAMPER_DETECTED so the Application's tamper-lockout policy can be
 * exercised without hardware.
 */

#include "integrity_backend.h"

#include <stdbool.h>

#include "esp_log.h"

static const char *TAG = "INTEGRITY_STUB";

/* Bool Kconfig symbols are only defined when set — resolve at compile time. */
#if CONFIG_INTEGRITY_SIM_TAMPER
static const bool s_sim_tamper = true;
#else
static const bool s_sim_tamper = false;
#endif

static integrity_init_err_t stub_init(void)
{
    ESP_LOGI(TAG, "stub backend init (sim-tamper %s)",
             s_sim_tamper ? "on" : "off");
    return INTEGRITY_INIT_OK;
}

static integrity_status_t stub_run_checks(void)
{
    if (s_sim_tamper) {
        return INTEGRITY_TAMPER_DETECTED;
    }
    return INTEGRITY_OK;
}

const integrity_backend_ops_t integrity_backend_stub_ops = {
    .init        = stub_init,
    .run_checks  = stub_run_checks,
};
