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
 * @file integrity.h
 * @brief Integrity peer — tamper/integrity sensor sampling and check
 *        execution.
 *
 * The Application calls Integrity_RunChecks() on a fixed cadence from its
 * task loop, regardless of NFC activity. The module reports FINDINGS only;
 * policy (tamper lockout, refusing authorization) stays in the Application —
 * authentication is not authorization.
 *
 * Backend (stub now, real tamper sensors later) is selected by
 * INTEGRITY_BACKEND in Kconfig.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INTEGRITY_OK = 0,            /* no tamper/integrity violation          */
    INTEGRITY_TAMPER_DETECTED,   /* tamper sensor tripped                  */
    INTEGRITY_ERR,               /* check could not run (sensor fault)     */
} integrity_status_t;

typedef enum {
    INTEGRITY_INIT_OK = 0,
    INTEGRITY_INIT_ERR,          /* backend failed to initialise           */
} integrity_init_err_t;

/**
 * Initialise the integrity backend.
 */
integrity_init_err_t Integrity_Init(void);

/**
 * Run integrity/tamper checks now. Suitable for fixed-cadence calls from the
 * Application task loop (e.g. every INTEGRITY_PERIOD_MS). Must be cheap and
 * non-blocking.
 *
 * @return the current integrity status.
 */
integrity_status_t Integrity_RunChecks(void);

/**
 * @return the most recently reported integrity status without re-sampling.
 */
integrity_status_t Integrity_GetStatus(void);

/**
 * @return true if the last check reported a tamper condition.
 */
bool Integrity_IsTampered(void);

#ifdef __cplusplus
}
#endif
