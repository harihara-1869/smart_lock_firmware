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

#include "comm_module.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "session.h"
#include "transport.h"
#include "lli.h"

static const char *TAG = "COMM_MOD";

/* ------------------------------------------------------------------ */
/* Internal mailbox (not exposed in the header)                        */
/* ------------------------------------------------------------------ */

typedef struct {
    bool    command_valid;
    uint8_t command[227];
    size_t  command_length;
    comm_app_event_t pending_event;

    uint8_t response[227];
    size_t  response_length;
} comm_mailbox_t;

/* ------------------------------------------------------------------ */
/* Static singleton state                                              */
/* ------------------------------------------------------------------ */

static comm_mailbox_t     g_mailbox;
static comm_module_config_t g_cfg;

static transport_handle_t g_transport;
static session_handle_t   g_session;

static TaskHandle_t       g_app_task      = NULL;
static TaskHandle_t       g_comm_task     = NULL;
static volatile bool      g_stop_requested = false;
static volatile bool      g_session_active = false;

/* ------------------------------------------------------------------ */
/* Internal trampolines (registered into session_config_t)             */
/* ------------------------------------------------------------------ */

static session_err_t comm_dispatch_via_mailbox(
        const uint8_t *plaintext_in, size_t len_in,
        uint8_t *plaintext_out, size_t *len_out, void *ctx)
{
    (void)ctx;

    g_mailbox.command_length = len_in;
    memcpy(g_mailbox.command, plaintext_in, len_in);
    g_mailbox.command_valid = true;

    xTaskNotifyGive(g_app_task);

    if (ulTaskNotifyTake(pdTRUE,
                         pdMS_TO_TICKS(g_cfg.app_response_timeout_ms)) == 0) {
        g_mailbox.command_valid = false;
        return SESSION_ERR_INTERNAL;
    }

    *len_out = g_mailbox.response_length;
    memcpy(plaintext_out, g_mailbox.response, *len_out);
    return SESSION_OK;
}

static void comm_on_established(void *ctx)
{
    (void)ctx;
    g_session_active = true;
    g_mailbox.pending_event = COMM_APP_EVENT_SESSION_STARTED;
    xTaskNotifyGive(g_app_task);
}

static void comm_on_terminated(void *ctx)
{
    (void)ctx;
    g_session_active = false;
    g_mailbox.pending_event = COMM_APP_EVENT_SESSION_ENDED;
    xTaskNotifyGive(g_app_task);
}

/* ------------------------------------------------------------------ */
/* Comm task body                                                      */
/* ------------------------------------------------------------------ */

static void comm_task_fn(void *arg)
{
    (void)arg;

    /* Capture our own handle immediately so xTaskNotifyGive(g_comm_task) from
     * comm_module_complete_response can never race the pxCreatedTask out-param
     * written by xTaskCreate, which on a preemptive SMP system may be assigned
     * only after this task has already started running. */
    g_comm_task = xTaskGetCurrentTaskHandle();

    while (!g_stop_requested) {
        transport_err_t err = transport_run_session(g_transport);
        if (err == TRANSPORT_ERR_BUS_FATAL) {
            /* The I2C bus/controller is wedged beyond recovery. Stop the comm
             * task (clear handle + self-delete below) instead of looping
             * forever; the Application's supervision/watchdog can decide how
             * to recover. */
            ESP_LOGE(TAG, "comm task stopping: fatal bus error");
            break;
        }
        /* TRANSPORT_ERR_TIMEOUT (no reader) and TRANSPORT_OK (session ran to
         * RELEASED) are both expected, steady-state outcomes — loop
         * immediately. */
    }

    g_comm_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

comm_err_t comm_module_init(const comm_module_config_t *cfg)
{
    if (!cfg) {
        return COMM_ERR_INVALID_ARG;
    }

    memset(&g_mailbox, 0, sizeof(g_mailbox));
    g_cfg = *cfg;

    /* --- lli_config_t --- */
    lli_config_t lli_cfg = {
        .sda_gpio     = cfg->sda_gpio,
        .scl_gpio     = cfg->scl_gpio,
        .irq_gpio     = cfg->irq_gpio,
        .rst_gpio     = cfg->rst_gpio,
        .i2c_port     = cfg->i2c_port,
        .i2c_clk_hz   = cfg->i2c_clk_hz,
    };
    memcpy(lli_cfg.sens_res, cfg->sens_res, sizeof(lli_cfg.sens_res));
    memcpy(lli_cfg.nfcid1,   cfg->nfcid1,   sizeof(lli_cfg.nfcid1));
    lli_cfg.sel_res = cfg->sel_res;
    memcpy(lli_cfg.nfcid2,      cfg->nfcid2,      sizeof(lli_cfg.nfcid2));
    memcpy(lli_cfg.pad,         cfg->pad,         sizeof(lli_cfg.pad));
    memcpy(lli_cfg.system_code, cfg->system_code, sizeof(lli_cfg.system_code));
    memcpy(lli_cfg.nfcid3t,     cfg->nfcid3t,     sizeof(lli_cfg.nfcid3t));
    memcpy(lli_cfg.gt,          cfg->gt,          sizeof(lli_cfg.gt));
    lli_cfg.gt_len = cfg->gt_len;
    memcpy(lli_cfg.tk,          cfg->tk,          sizeof(lli_cfg.tk));
    lli_cfg.tk_len = cfg->tk_len;

    /* --- session_config_t --- */
    session_config_t sess_cfg = {
        .local_sk            = {0},
        .local_pk            = {0},
        .peer_key_provider   = (session_peer_key_provider_t)cfg->peer_key_provider,
        .peer_key_provider_ctx = cfg->peer_key_provider_ctx,
        .app_handler         = comm_dispatch_via_mailbox,
        .app_handler_ctx     = NULL,
        .on_established      = comm_on_established,
        .on_terminated       = comm_on_terminated,
        .event_ctx           = NULL,
    };
    memcpy(sess_cfg.local_sk, cfg->local_sk, sizeof(sess_cfg.local_sk));
    memcpy(sess_cfg.local_pk, cfg->local_pk, sizeof(sess_cfg.local_pk));

    session_err_t serr = session_init(&sess_cfg, &g_session);
    if (serr != SESSION_OK) {
        ESP_LOGE(TAG, "session_init failed: %d", serr);
        return COMM_ERR_INTERNAL;
    }

    /* --- transport_config_t --- */
    transport_config_t tcfg = {
        .lli_cfg              = lli_cfg,
        .handshake_timeout_ms = cfg->handshake_timeout_ms,
        .activate_timeout_ms  = cfg->activate_timeout_ms,
        .apdu_timeout_ms      = cfg->apdu_timeout_ms,
        .on_apdu              = session_on_apdu,
        .on_erase             = session_on_erase,
        .user_ctx             = g_session,
    };

    transport_err_t terr = transport_init(&tcfg, &g_transport);
    if (terr != TRANSPORT_OK) {
        ESP_LOGE(TAG, "transport_init failed: %d", terr);
        session_deinit(g_session);
        g_session = NULL;
        return COMM_ERR_INTERNAL;
    }

    g_stop_requested = false;
    g_session_active = false;

    ESP_LOGI(TAG, "initialised");
    return COMM_OK;
}

comm_err_t comm_module_deinit(void)
{
    if (g_transport) {
        transport_deinit(g_transport);
        g_transport = NULL;
    }
    if (g_session) {
        session_deinit(g_session);
        g_session = NULL;
    }
    memset(&g_mailbox, 0, sizeof(g_mailbox));
    /* Clear task handles/flags so a subsequent init + register_app_task +
     * start is clean rather than inheriting the previous instance's state. */
    g_app_task = NULL;
    g_comm_task = NULL;
    g_stop_requested = false;
    g_session_active = false;
    return COMM_OK;
}

comm_err_t comm_module_register_app_task(TaskHandle_t app_task)
{
    if (!app_task || g_app_task) {
        return COMM_ERR_INVALID_ARG;
    }
    g_app_task = app_task;
    return COMM_OK;
}

void comm_module_start(void)
{
    if (!g_app_task) {
        ESP_LOGE(TAG, "start called before app task registered");
        return;
    }

    uint32_t stack = g_cfg.task_stack_size;
    if (stack == 0) {
        stack = 8192;
    }

    UBaseType_t prio = g_cfg.task_priority;
    if (prio == 0) {
        prio = 5;
    }

    BaseType_t core = g_cfg.task_core_id;

    if (core == tskNO_AFFINITY) {
        xTaskCreate(comm_task_fn, "comm_task", stack, NULL, prio,
                    &g_comm_task);
    } else {
        xTaskCreatePinnedToCore(comm_task_fn, "comm_task", stack, NULL,
                                prio, &g_comm_task, core);
    }

    ESP_LOGI(TAG, "comm task started (stack=%lu, prio=%lu)",
             (unsigned long)stack, (unsigned long)prio);
}

void comm_module_stop(void)
{
    g_stop_requested = true;
}

comm_err_t comm_module_force_abort(void)
{
    /* Best-effort: no way to interrupt a blocking lli_activate from here
     * (the transport layer owns the lli handle). Setting g_stop_requested
     * is the only checkpoint the comm task currently consults. */
    g_stop_requested = true;
    return COMM_OK;
}

bool comm_module_session_active(void)
{
    return g_session_active;
}

comm_app_event_t comm_module_poll_event(void)
{
    comm_app_event_t ev = g_mailbox.pending_event;
    g_mailbox.pending_event = COMM_APP_EVENT_NONE;
    return ev;
}

bool comm_module_has_command(void)
{
    return g_mailbox.command_valid;
}

comm_err_t comm_module_get_command(uint8_t *buf, size_t buf_cap,
                                   size_t *len_out)
{
    if (!buf || !len_out) {
        return COMM_ERR_INVALID_ARG;
    }
    if (!g_mailbox.command_valid) {
        return COMM_ERR_INVALID_ARG;
    }
    if (buf_cap < g_mailbox.command_length) {
        return COMM_ERR_INVALID_ARG;
    }

    memcpy(buf, g_mailbox.command, g_mailbox.command_length);
    *len_out = g_mailbox.command_length;
    return COMM_OK;
}

void comm_module_complete_response(const uint8_t *response,
                                   size_t response_length)
{
    /* Only complete a command that is actually pending. A late call after the
     * comm task's bounded wait already timed out (command_valid cleared by the
     * timeout path) must not write a response or notify — otherwise the stale
     * xTaskNotifyGive(g_comm_task) would prematurely satisfy the NEXT
     * command's ulTaskNotifyTake and deliver these bytes as its reply. */
    if (!g_mailbox.command_valid) {
        return;
    }
    g_mailbox.command_valid = false;

    if (response && response_length > 0) {
        if (response_length > sizeof(g_mailbox.response)) {
            response_length = sizeof(g_mailbox.response);
        }
        memcpy(g_mailbox.response, response, response_length);
    }
    g_mailbox.response_length = response_length;

    xTaskNotifyGive(g_comm_task);
}

comm_err_t comm_module_arm_provisioning_window(uint32_t timeout_ms)
{
    if (session_arm_provisioning_window(g_session, timeout_ms) != SESSION_OK) {
        return COMM_ERR_INTERNAL;
    }
    return COMM_OK;
}

bool comm_module_provision_verify_identity(const uint8_t claimed_pubkey[32])
{
    return session_provision_verify_identity(g_session, claimed_pubkey);
}
