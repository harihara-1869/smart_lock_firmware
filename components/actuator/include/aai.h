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
 * @file aai.h
 * @brief Actuator Abstraction Interface (AAI) — the ONLY public surface of
 *        the actuator module.
 *
 * The Actuator reports facts (bolt position, stall). It does NOT own policy:
 * auto-reversal decisions, NVS intent logging, and boot-state recovery are
 * orchestrated by the Application calling these functions.
 *
 * Backend selection: ACTUATOR_BACKEND in Kconfig (STUB / RMT / MCPWM). Exactly
 * one is compiled in; swapping backends is a config change, never an interface
 * change. The backend lives entirely inside the actuator component.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AAI_STATE_UNKNOWN = 0, /* not initialised, or position not yet known   */
    AAI_STATE_LOCKED,      /* bolt fully retracted / locked                */
    AAI_STATE_UNLOCKED,    /* bolt fully extended / unlocked               */
    AAI_STATE_MOVING,      /* actuation in progress                        */
    AAI_STATE_JAMMED,      /* stall detected (stall pin or limit timeout)  */
    AAI_STATE_FAULT,       /* driver-level fault (OCP/OTP, comm loss, ...) */
} aai_state_t;

typedef enum {
    AAI_OK = 0,
    AAI_ERR_INVALID_STATE, /* request impossible in current state          */
    AAI_ERR_BUSY,          /* another actuation already in progress        */
    AAI_ERR_FAULT,         /* backend fault; see AAI_GetStatus             */
    AAI_ERR_STORAGE,       /* backend could not persist/record state       */
    AAI_ERR_INTERNAL,      /* backend internal error                       */
} aai_err_t;

/**
 * Initialise the actuator backend.
 */
aai_err_t AAI_Init(void);

/**
 * Drive the bolt toward the UNLOCKED position (async — returns immediately).
 * The Application polls AAI_GetStatus() to observe progress.
 */
aai_err_t AAI_Open(void);

/**
 * Drive the bolt toward the LOCKED position (async — returns immediately).
 */
aai_err_t AAI_Close(void);

/**
 * Cease driving the motor immediately. Used by the Application's
 * jam-recovery path (auto-reversal) and by boot-state resolution.
 */
aai_err_t AAI_Stop(void);

/**
 * @return the TRUE physical state as reported by the backend. Never
 *         fabricates success: a backend that cannot sense its position
 *         reports UNKNOWN/JAMMED/FAULT, not LOCKED/UNLOCKED.
 */
aai_state_t AAI_GetStatus(void);

#ifdef __cplusplus
}
#endif
