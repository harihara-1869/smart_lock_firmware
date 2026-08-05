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
 * @file app_module.h
 * @brief Application Module coordinator — the business-logic layer.
 *
 * The Application owns command dispatch, authorization, the provisioning
 * workflow, the peer-key iterator over the key store, actuation intent +
 * boot recovery, and the integrity cadence. It contains NO peripheral drivers
 * and NO register access; it talks to peers only through their public
 * interfaces (comm_module.h, aai.h, display.h, integrity.h, key_store.h,
 * intent_log.h).
 *
 * app_main() calls AppModule_Init() (after the peers it needs are initialised
 * in dependency order) and then AppModule_Start() to spawn the single
 * Application task. The task registers with the comm facade and takes over
 * as coordinator.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "comm_module.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_MODULE_OK = 0,
    APP_MODULE_ERR_INVALID_ARG,
    APP_MODULE_ERR_INIT,     /* a peer failed to initialise               */
    APP_MODULE_ERR_TASK,     /* task creation failed                       */
} app_module_err_t;

/**
 * Initialise the Application Module.
 *
 * Preconditions (dependency order, set by app_main):
 *   - Storage (key store + intent log) initialised
 *   - Display initialised
 *   - Actuator initialised (incl. boot-state verification)
 *   - Integrity initialised
 *   - comm_module_init() called with cfg.peer_key_provider already wired to
 *     AppModule's iterator (AppModule_GetPeerKeyByIndex)
 *
 * @param comm_cfg  The comm_module_config_t used for comm_module_init().
 *                  Needed for timing constants (app_response_timeout_ms).
 * @return APP_MODULE_OK or APP_MODULE_ERR_*.
 */
app_module_err_t AppModule_Init(const comm_module_config_t *comm_cfg);

/**
 * Spawn the Application task.
 *
 * Must be called after AppModule_Init(). The task calls
 * comm_module_register_app_task() then comm_module_start() and takes over the
 * bounded-wait loop. Returns immediately.
 */
app_module_err_t AppModule_Start(void);

/**
 * @return a comm_module_config_t pre-filled with the Application's identity
 *         (local keys, timing, peer_key_provider wired to the key store
 *         iterator). app_main() fills in the LLI/I2C GPIOs and identity
 *         bytes, then passes the result to comm_module_init().
 */
comm_module_config_t AppModule_GetCommConfig(void);

/**
 * peer_key_provider implementation — enumerates authorized keys from the key
 * store. Signature-compatible with comm_peer_key_provider_t.
 */
bool AppModule_GetPeerKeyByIndex(size_t index, uint8_t pubkey_out[32],
                                 void *provider_ctx);

/**
 * @return pointer to the lock's 32-byte Ed25519 public key, copied from the
 *         comm config at init. Owned by the Application.
 */
const uint8_t *AppModule_GetLockPk(void);

#ifdef __cplusplus
}
#endif
