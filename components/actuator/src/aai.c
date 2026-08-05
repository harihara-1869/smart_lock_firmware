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
 * @file aai.c
 * @brief AAI dispatcher — forwards the public interface to the backend
 *        selected by ACTUATOR_BACKEND in Kconfig.
 */

#include "aai.h"

#include "aai_backend.h"

#include "esp_log.h"

static const char *TAG = "AAI";

static const aai_backend_ops_t *backend(void)
{
#if defined(CONFIG_ACTUATOR_BACKEND_RMT)
    return &aai_backend_rmt_ops;
#elif defined(CONFIG_ACTUATOR_BACKEND_MCPWM)
    return &aai_backend_mcpwm_ops;
#else
    return &aai_backend_stub_ops;
#endif
}

aai_err_t AAI_Init(void)
{
    aai_err_t err = backend()->init();
    if (err != AAI_OK) {
        ESP_LOGE(TAG, "backend init failed: %d", err);
    }
    return err;
}

aai_err_t AAI_Open(void)
{
    return backend()->open();
}

aai_err_t AAI_Close(void)
{
    return backend()->close();
}

aai_err_t AAI_Stop(void)
{
    return backend()->stop();
}

aai_state_t AAI_GetStatus(void)
{
    return backend()->get_status();
}
