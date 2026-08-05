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
 * @file intent_log.h
 * @brief Actuation-intent log — the persistence SEAM for TARGET_STATE.
 *
 * Application_Module_Master.md §2.3 requires the Application to write the
 * TARGET_STATE intent to non-volatile storage BEFORE driving the motor, so a
 * power loss during actuation can be resolved at boot. Like key_store.h, this
 * is an interface; the backend is selected by STORAGE_BACKEND in Kconfig.
 *
 * The RAM placeholder has no persistence (honestly logged); a future NVS or
 * secure-element backend makes the intent survive reboot without changing any
 * application call site.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INTENT_TARGET_NONE = 0,   /* no actuation in progress */
    INTENT_TARGET_LOCKED,     /* intent: bolt ends fully LOCKED   */
    INTENT_TARGET_UNLOCKED,   /* intent: bolt ends fully UNLOCKED */
} intent_target_t;

typedef enum {
    INTENT_OK = 0,
    INTENT_ERR_STORAGE,   /* backend failure (write denied, element busy, ...) */
} intent_err_t;

/**
 * Initialise the intent log (backend-dependent; may be a no-op for RAM).
 */
intent_err_t IntentLog_Init(void);

/**
 * Record the target state BEFORE calling AAI_Open()/AAI_Close().
 *
 * @return INTENT_OK or INTENT_ERR_STORAGE (caller should abort actuation).
 */
intent_err_t IntentLog_SetTarget(intent_target_t target);

/**
 * Read the last recorded target. Boot recovery uses this to decide which way
 * to resolve an intermediate bolt position.
 *
 * @return the last target, or INTENT_TARGET_NONE if none pending.
 */
intent_target_t IntentLog_GetTarget(void);

/**
 * Clear the intent once the physical state is confirmed to match.
 *
 * @return INTENT_OK or INTENT_ERR_STORAGE.
 */
intent_err_t IntentLog_ClearTarget(void);

#ifdef __cplusplus
}
#endif
