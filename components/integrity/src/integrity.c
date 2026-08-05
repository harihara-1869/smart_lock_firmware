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
 * @file integrity.c
 * @brief Integrity dispatcher — forwards the public interface to the backend
 *        selected by INTEGRITY_BACKEND in Kconfig.
 */

#include "integrity.h"

#include "integrity_backend.h"

static const integrity_backend_ops_t *backend(void)
{
#if defined(CONFIG_INTEGRITY_BACKEND_TAMPER_GPIO)
    return &integrity_backend_tamper_gpio_ops;
#else
    return &integrity_backend_stub_ops;
#endif
}

static integrity_status_t s_last = INTEGRITY_OK;

integrity_init_err_t Integrity_Init(void)
{
    integrity_init_err_t err = backend()->init();
    if (err == INTEGRITY_INIT_OK) {
        s_last = INTEGRITY_OK;
    }
    return err;
}

integrity_status_t Integrity_RunChecks(void)
{
    s_last = backend()->run_checks();
    return s_last;
}

integrity_status_t Integrity_GetStatus(void)
{
    return s_last;
}

bool Integrity_IsTampered(void)
{
    return s_last == INTEGRITY_TAMPER_DETECTED;
}
