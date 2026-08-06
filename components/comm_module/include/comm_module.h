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
 * @file comm_module.h
 * @brief Comm Module Facade — sole header the Application Module depends on.
 *
 * Wires LLI → Transport → Session, owns one FreeRTOS comm task, and hands
 * commands and session lifecycle events to the Application via a mailbox +
 * task-notification handoff. The Application includes only this header.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COMM_OK = 0,
    COMM_ERR_TIMEOUT,
    COMM_ERR_INVALID_ARG,
    COMM_ERR_INTERNAL,
} comm_err_t;

typedef enum {
    COMM_APP_EVENT_NONE = 0,
    COMM_APP_EVENT_SESSION_STARTED,
    COMM_APP_EVENT_SESSION_ENDED,
} comm_app_event_t;

/**
 * Enumerates candidate long-term Ed25519 public keys for M3 verification.
 *
 * Signature-compatible with session_peer_key_provider_t so that the value
 * flows directly into session_config_t without an adapter.
 *
 * @param index        0-based candidate index.
 * @param pubkey_out   Receives one 32-byte Ed25519 public key.
 * @param provider_ctx Opaque pointer from comm_module_config_t.
 * @return true if pubkey_out was written; false if no more candidates.
 */
typedef bool (*comm_peer_key_provider_t)(
        size_t index, uint8_t pubkey_out[32], void *provider_ctx);

typedef struct {
    /* → lli_config_t (GPIOs, I2C port/clock, card identity bytes) */
    int        sda_gpio;
    int        scl_gpio;
    int        irq_gpio;
    int        rst_gpio;
    i2c_port_t i2c_port;
    uint32_t   i2c_clk_hz;

    uint8_t sens_res[2];
    uint8_t nfcid1[3];
    uint8_t sel_res;
    uint8_t nfcid2[8];
    uint8_t pad[8];
    uint8_t system_code[2];
    uint8_t nfcid3t[10];
    uint8_t gt[47];
    uint8_t gt_len;
    uint8_t tk[47];
    uint8_t tk_len;

    /* → transport_config_t timing */
    uint32_t handshake_timeout_ms;
    uint32_t activate_timeout_ms;
    uint32_t apdu_timeout_ms;

    /* → session_config_t identity */
    uint8_t local_sk[64];
    uint8_t local_pk[32];
    comm_peer_key_provider_t peer_key_provider;
    void *peer_key_provider_ctx;

    /* Bounds the "waiting for the Application task to answer" step. */
    uint32_t app_response_timeout_ms;

    /* Internal task ownership (0 = facade default). */
    uint32_t    task_stack_size;
    UBaseType_t task_priority;
    BaseType_t  task_core_id;
} comm_module_config_t;

/**
 * Initialise the Comm Module (singleton — one PN532, one lock).
 *
 * Builds lli_config_t → transport_config_t (wiring session_on_apdu /
 * session_on_erase) → session_config_t (wiring internal mailbox trampolines)
 * and calls transport_init internally. Does NOT spawn a task.
 *
 * @param cfg  Configuration; caller may discard after return.
 * @return COMM_OK on success.
 */
comm_err_t comm_module_init(const comm_module_config_t *cfg);

/**
 * Tear down the Comm Module (deinits transport/session/lli).
 * NULL-safe; returns COMM_OK.
 */
comm_err_t comm_module_deinit(void);

/**
 * Register the Application task handle.
 *
 * Must be called once after the Application task exists and before
 * comm_module_start(). Calling more than once returns COMM_ERR_INVALID_ARG.
 *
 * @param app_task  FreeRTOS task handle of the Application task.
 * @return COMM_OK or COMM_ERR_INVALID_ARG.
 */
comm_err_t comm_module_register_app_task(TaskHandle_t app_task);

/**
 * Spawn the comm task.  Returns immediately.
 *
 * comm_module_register_app_task must have been called first.
 */
void comm_module_start(void);

/**
 * Request the comm task to exit at the top of its next loop iteration.
 * Does not interrupt a session mid-flight.
 *
 * Note: the comm task may also exit on its own if the transport reports a
 * fatal bus error (TRANSPORT_ERR_BUS_FATAL — the I2C bus/controller is wedged
 * beyond recovery) COMM_FATAL_RETRY_LIMIT consecutive times. A single fatal is
 * retried (the session re-activates and waits for a reader again), so a
 * transient wedge (e.g. a phone left on the pad) does not kill the task; only
 * a persistently dead bus stops it. On exit it logs once and deletes itself;
 * the Application can detect this and decide on recovery.
 */
void comm_module_stop(void);

/**
 * Best-effort abort: sets the comm task's stop flag.  Does NOT interrupt a
 * blocking lli_activate call — there is no cancellation checkpoint inside it,
 * and the LLI handle is owned by the transport layer and is not directly
 * reachable from here — and does not interrupt a session mid-flight; it takes
 * effect only at the top of the comm task's next loop iteration. Currently
 * functionally equivalent to comm_module_stop(); kept as a distinct entry
 * point for a future cancel-safe primitive below LLI.
 *
 * @return COMM_OK.
 */
comm_err_t comm_module_force_abort(void);

/**
 * @return true if a session is currently in ESTABLISHED stage.
 */
bool comm_module_session_active(void);

/**
 * Return and clear the pending lifecycle event (if any).
 *
 * Must be called from the Application task context only, after being
 * notified by the comm task.
 *
 * @return The pending event (COMM_APP_EVENT_NONE if none).
 */
comm_app_event_t comm_module_poll_event(void);

/**
 * @return true if an undelivered command is waiting for a response.
 */
bool comm_module_has_command(void);

/**
 * Read the pending command bytes into @p buf.
 *
 * @param buf      Destination buffer.
 * @param buf_cap  Capacity of @p buf.
 * @param len_out  Receives the command length.
 * @return COMM_OK or COMM_ERR_INVALID_ARG.
 */
comm_err_t comm_module_get_command(uint8_t *buf, size_t buf_cap,
                                   size_t *len_out);

/**
 * Supply the response to the pending command and notify the comm task.
 *
 * Copies @p response bytes into the mailbox and performs the notify in a
 * single call — no stale-pointer window.
 *
 * @param response         Response bytes (opaque to the comm module).
 * @param response_length  Number of bytes to copy.
 */
void comm_module_complete_response(const uint8_t *response,
                                   size_t response_length);

/**
 * Arms exactly one upcoming session to skip resolver-based M3 verification
 * and instead cache Sig_P + the M3 transcript for a later deferred check.
 *
 * @param timeout_ms  The validity window in milliseconds.
 * @return COMM_OK or COMM_ERR_INTERNAL.
 */
comm_err_t comm_module_arm_provisioning_window(uint32_t timeout_ms);

/**
 * Verifies the CACHED (unverified-at-handshake-time) Sig_P from the most
 * recently completed provisioning-mode M3 against a caller-supplied
 * candidate public key.
 *
 * @param claimed_pubkey  The 32-byte Ed25519 public key claimed by the peer.
 * @return true if the signature over the cached transcript is valid, false otherwise.
 */
bool comm_module_provision_verify_identity(const uint8_t claimed_pubkey[32]);

#ifdef __cplusplus
}
#endif
