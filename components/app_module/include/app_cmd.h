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
 * @file app_cmd.h
 * @brief Application command protocol — the source of truth for opcodes,
 *        status bytes, and request/response layouts.
 *
 * Every layout here follows Application_Module_Master.md §3 exactly:
 *
 *   CMD_PROVISION  0x01  req: OPCODE ‖ SECRET(32) ‖ PHONE_PK(32)        (65 B)
 *                        ok:  0x00 ‖ LOCK_PK(32)                       (33 B)
 *                        err: 0x01                                     ( 1 B)
 *   CMD_UNLOCK     0x02  req: OPCODE                                    ( 1 B)
 *                        ok:  0x00 (immediate; actuation is async)      ( 1 B)
 *   CMD_LOCK       0x03  req: OPCODE                                    ( 1 B)
 *                        ok:  0x00 (immediate; actuation is async)      ( 1 B)
 *   CMD_GET_STATUS 0x04  req: OPCODE                                    ( 1 B)
 *                        ok:  0x00 ‖ BATTERY_PCT ‖ LOCK_STATE ‖ LAST_ERROR (4 B)
 *   CMD_REVOKE_KEY 0x05  req: OPCODE ‖ TARGET_PK(32)                   (33 B)
 *                        ok:  0x00                                     ( 1 B)
 *                        err: 0x01                                     ( 1 B)
 *
 * The response contract: EVERY application-level outcome is encoded inside
 * the response bytes. comm_module_complete_response has no "this failed"
 * signal — a denial/fault/invalid command is an app status byte delivered
 * through a normal non-empty response.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Opcodes ----------------------------------------------------------- */
#define CMD_PROVISION   0x01
#define CMD_UNLOCK      0x02
#define CMD_LOCK        0x03
#define CMD_GET_STATUS  0x04
#define CMD_REVOKE_KEY  0x05

/* --- Application status bytes ----------------------------------------- */
#define APP_STATUS_OK               0x00
#define APP_STATUS_INVALID_SECRET   0x01 /* provisioning secret mismatch        */
#define APP_STATUS_UNAUTHORIZED     0x02 /* session key not in key store        */
#define APP_STATUS_INVALID_CMD      0x03 /* unknown opcode / malformed request   */
#define APP_STATUS_FAULT            0x04 /* mechanical/tamper fault              */
#define APP_STATUS_BUSY             0x05 /* actuation already in progress        */
#define APP_STATUS_INTERNAL         0x06 /* app-level internal error             */
#define APP_STATUS_KEY_FULL         0x07 /* key store at capacity                */
#define APP_STATUS_KEY_EXISTS       0x08 /* provisioning: key already present    */
#define APP_STATUS_NOT_FOUND        0x09 /* revoke: target key not found         */

/* --- Fixed protocol sizes --------------------------------------------- */
#define APP_KEY_LEN          32
#define APP_SECRET_LEN       32
#define APP_RESPONSE_BUF     227 /* matches the facade mailbox budget          */

/* --- Lock state / error codes for CMD_GET_STATUS ----------------------- */
typedef enum {
    APP_LOCK_STATE_LOCKED = 0,
    APP_LOCK_STATE_UNLOCKED,
    APP_LOCK_STATE_MOVING,
    APP_LOCK_STATE_JAMMED,
    APP_LOCK_STATE_FAULT,
    APP_LOCK_STATE_UNKNOWN,
} app_lock_state_t;

typedef enum {
    APP_LAST_ERROR_NONE = 0,
    APP_LAST_ERROR_MOTOR_STALL,   /* jam detected during actuation          */
    APP_LAST_ERROR_MOTOR_FAULT,   /* driver fault                            */
    APP_LAST_ERROR_TAMPER,        /* integrity violation                     */
    APP_LAST_ERROR_INTERNAL,
} app_last_error_t;

/* Default battery level reported by CMD_GET_STATUS until a real ADC read
 * lands (TODO(hardware):). */
#define APP_BATTERY_PCT_DEFAULT 100

#ifdef __cplusplus
}
#endif
