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
 * @file app_config.h
 * @brief Application timing and policy constants.
 *
 * Timing rule (master doc §10): pick INTEGRITY_PERIOD_MS FIRST, then set
 * app_response_timeout_ms comfortably above it so integrity checks never slip
 * while a command is being processed.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Application task stack size (bytes). */
#define APP_TASK_STACK_SIZE 4096

/** Application task priority. */
#define APP_TASK_PRIORITY   5

/**
 * Integrity-check cadence. The bounded-wait loop runs perform_integrity_checks()
 * every INTEGRITY_PERIOD_MS regardless of NFC activity.
 */
#define INTEGRITY_PERIOD_MS 50

/**
 * comm_module_config_t.app_response_timeout_ms — the comm task's bounded wait
 * for the Application to answer. Must be comfortably above INTEGRITY_PERIOD_MS
 * so a command being processed never slips an integrity check.
 */
#define APP_RESPONSE_TIMEOUT_MS 200

/** Boot-state resolution timeout when recovering from a power-loss mid-actuation. */
#define APP_BOOT_RECOVERY_TIMEOUT_MS 10000

/** How often the Application polls AAI_GetStatus() while an actuation is in flight. */
#define APP_ACTUATION_POLL_MS 20

/** Provisioning window validity (Application-side) when armed via the button. */
#define APP_PROVISION_WINDOW_MS 60000

#ifdef __cplusplus
}
#endif
