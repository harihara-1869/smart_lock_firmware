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
 * power loss during actuation can be resolved at boot.
 *
 * Architecture:
 *   - RAM cache warmed at boot; intent_log_get_cached() is RAM-only.
 *   - intent_log_write() commits to the HAL FIRST (write-through), then mirrors
 *     to RAM. A failed write leaves RAM untouched.
 *   - mutating calls happen only on the Application task; the read path
 *     (get_cached) is also only called on the Application task during boot
 *     recovery, so no concurrency is needed.
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
 * Initialise the intent log: warm the RAM cache from the persistent backend.
 */
intent_err_t intent_log_init(void);

/**
 * Record a target state BEFORE calling AAI_Open()/AAI_Close().
 * Write-through: HAL first, then RAM. Pass INTENT_TARGET_NONE to clear.
 *
 * @return INTENT_OK or INTENT_ERR_STORAGE (caller should abort actuation).
 */
intent_err_t intent_log_write(intent_target_t target);

/**
 * Read the last recorded target from the RAM cache. Boot recovery uses this
 * to decide which way to resolve an intermediate bolt position.
 *
 * RAM-only — no HAL call.
 *
 * @return the last target, or INTENT_TARGET_NONE if none pending.
 */
intent_target_t intent_log_get_cached(void);

#ifdef __cplusplus
}
#endif
