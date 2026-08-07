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
 * @file app_module.c
 * @brief Application Module coordinator.
 *
 * Owns exactly one dedicated task running the bounded-wait loop (master doc
 * §10 / comm_module.md worked example):
 *
 *     perform_integrity_checks()
 *     ulTaskNotifyTake(pdTRUE, INTEGRITY_PERIOD_MS)
 *     comm_module_poll_event()
 *     comm_module_has_command() / comm_module_get_command()
 *     dispatch
 *     comm_module_complete_response()
 *
 * Both the event AND the command are drained on every wake (notifications
 * coalesce). Integrity/tamper monitoring runs on cadence regardless of NFC
 * activity.
 *
 * Business logic only: no peripheral register access, no drivers. Talks to
 * peers exclusively through comm_module.h, aai.h, display.h, integrity.h,
 * key_store.h, intent_log.h.
 */

#include "app_module.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "aai.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_dispatch.h"
#include "display.h"
#include "integrity.h"
#include "intent_log.h"
#include "key_store.h"
#include "provision_button.h"
#include "provision_mgr.h"

static const char *TAG = "APP_MOD";

/* ------------------------------------------------------------------ */
/* Static state (single Application task owns all of it)               */
/* ------------------------------------------------------------------ */

/* The lock's long-term Ed25519 public key (32 bytes), copied from the comm
 * config at init. Used for the CMD_PROVISION success response. */
static uint8_t s_lock_pk[APP_KEY_LEN];

/* Mechanical / diagnostic state. */
static uint8_t s_last_error = APP_LAST_ERROR_NONE;

/* Async actuation state: a target drives the bolt after the digital reply. */
static uint8_t s_actuation_target = APP_LOCK_STATE_LOCKED; /* target state */
static bool    s_actuation_pending = false;

/* Provision button is event-driven (ISR doorbell); no poll state here. */

/* ------------------------------------------------------------------ */
/* Peer-key provider (iterator over the key store)                     */
/* ------------------------------------------------------------------ */

bool AppModule_GetPeerKeyByIndex(size_t index, uint8_t pubkey_out[32],
                                 void *provider_ctx)
{
    (void)provider_ctx;
    return key_store_get(index, pubkey_out);
}

const uint8_t *AppModule_GetLockPk(void)
{
    return s_lock_pk;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint8_t aai_state_to_app_lock_state(aai_state_t s)
{
    switch (s) {
    case AAI_STATE_LOCKED:   return APP_LOCK_STATE_LOCKED;
    case AAI_STATE_UNLOCKED: return APP_LOCK_STATE_UNLOCKED;
    case AAI_STATE_MOVING:   return APP_LOCK_STATE_MOVING;
    case AAI_STATE_JAMMED:   return APP_LOCK_STATE_JAMMED;
    case AAI_STATE_FAULT:    return APP_LOCK_STATE_FAULT;
    default:                 return APP_LOCK_STATE_UNKNOWN;
    }
}

static uint8_t current_lock_state(void)
{
    return aai_state_to_app_lock_state(AAI_GetStatus());
}

/* Write the actuation intent BEFORE driving the motor (§2.3). */
static bool set_intent(intent_target_t target)
{
    return intent_log_write(target) == INTENT_OK;
}

/* Kick off an async actuation toward @p target. */
static void start_actuation(intent_target_t target)
{
    if (!set_intent(target)) {
        ESP_LOGE(TAG, "failed to persist actuation intent; aborting actuation");
        s_last_error = APP_LAST_ERROR_INTERNAL;
        return;
    }
    s_actuation_target =
        (target == INTENT_TARGET_UNLOCKED) ? APP_LOCK_STATE_UNLOCKED
                                           : APP_LOCK_STATE_LOCKED;
    s_actuation_pending = true;

    aai_err_t err = (target == INTENT_TARGET_UNLOCKED) ? AAI_Open()
                                                       : AAI_Close();
    if (err != AAI_OK) {
        ESP_LOGE(TAG, "AAI start failed: %d", err);
        s_last_error = APP_LAST_ERROR_MOTOR_FAULT;
        s_actuation_pending = false;
        intent_log_write(INTENT_TARGET_NONE);
    }
}

/* ------------------------------------------------------------------ */
/* Boot recovery (§2.3): resolve an intermediate bolt position using    */
/* the persisted intent.                                                */
/* ------------------------------------------------------------------ */

static void boot_recovery(void)
{
    intent_target_t intent = intent_log_get_cached();
    aai_state_t     phys   = AAI_GetStatus();

    ESP_LOGI(TAG, "boot recovery: physical=%d intent=%d",
             (int)phys, (int)intent);

    if (phys == AAI_STATE_LOCKED || phys == AAI_STATE_UNLOCKED) {
        /* Consistent; clear any stale intent. */
        intent_log_write(INTENT_TARGET_NONE);
        return;
    }

    if (phys == AAI_STATE_JAMMED || phys == AAI_STATE_FAULT ||
        phys == AAI_STATE_UNKNOWN) {
        /* Cannot safely resolve automatically — report the fault. */
        ESP_LOGE(TAG, "cannot resolve state %d automatically; lock fault",
                 (int)phys);
        s_last_error = APP_LAST_ERROR_MOTOR_STALL;
        intent_log_write(INTENT_TARGET_NONE);
        return;
    }

    /* Intermediate (MOVING): drive toward the recorded intent, defaulting to
     * LOCKED for safety if none is recorded. */
    intent_target_t target = intent;
    if (target != INTENT_TARGET_LOCKED && target != INTENT_TARGET_UNLOCKED) {
        ESP_LOGW(TAG, "no intent recorded; resolving to LOCKED (safe default)");
        target = INTENT_TARGET_LOCKED;
    }
    ESP_LOGI(TAG, "resolving intermediate state -> %s",
             target == INTENT_TARGET_UNLOCKED ? "UNLOCKED" : "LOCKED");

    aai_err_t err = (target == INTENT_TARGET_UNLOCKED) ? AAI_Open() : AAI_Close();
    if (err != AAI_OK) {
        ESP_LOGE(TAG, "boot recovery actuation failed: %d", err);
        s_last_error = APP_LAST_ERROR_MOTOR_STALL;
        return;
    }

    /* Wait for the resolution to finish (bounded). */
    int64_t deadline = esp_timer_get_time()
                       + (int64_t)APP_BOOT_RECOVERY_TIMEOUT_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        aai_state_t st = AAI_GetStatus();
        if (st == AAI_STATE_LOCKED || st == AAI_STATE_UNLOCKED) {
            ESP_LOGI(TAG, "boot recovery complete -> %d", (int)st);
            break;
        }
        if (st == AAI_STATE_JAMMED || st == AAI_STATE_FAULT) {
            ESP_LOGE(TAG, "boot recovery failed: %d", (int)st);
            s_last_error = APP_LAST_ERROR_MOTOR_STALL;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(APP_ACTUATION_POLL_MS));
    }
    intent_log_write(INTENT_TARGET_NONE);
}

/* ------------------------------------------------------------------ */
/* Actuation state machine (polled on the app task's cadence)          */
/* ------------------------------------------------------------------ */

static void actuation_tick(void)
{
    if (!s_actuation_pending) {
        return;
    }

    aai_state_t st = AAI_GetStatus();

    if (st == s_actuation_target) {
        ESP_LOGI(TAG, "actuation complete -> %s",
                 st == AAI_STATE_UNLOCKED ? "UNLOCKED" : "LOCKED");
        s_actuation_pending = false;
        intent_log_write(INTENT_TARGET_NONE);
        Display_ShowIndication(DISPLAY_IND_SUCCESS);
        return;
    }

    if (st == AAI_STATE_JAMMED || st == AAI_STATE_FAULT) {
        ESP_LOGW(TAG, "actuation fault (%d) — auto-reversing to LOCKED",
                 (int)st);
        s_last_error = APP_LAST_ERROR_MOTOR_STALL;

        /* Auto-reversal: never leave the bolt partially engaged (§2.2). */
        AAI_Stop();
        if (AAI_GetStatus() != AAI_STATE_LOCKED) {
            AAI_Close();
        }
        s_actuation_pending = false;
        intent_log_write(INTENT_TARGET_NONE);
        Display_ShowIndication(DISPLAY_IND_ERROR);
        return;
    }

    /* MOVING or UNKNOWN — keep polling. */
}

/* ------------------------------------------------------------------ */
/* Integrity check (fixed cadence, independent of NFC)                 */
/* ------------------------------------------------------------------ */

static void perform_integrity_checks(void)
{
    integrity_status_t st = Integrity_RunChecks();
    if (st != INTEGRITY_OK) {
        ESP_LOGW(TAG, "integrity check: %d", (int)st);
        if (st == INTEGRITY_TAMPER_DETECTED) {
            s_last_error = APP_LAST_ERROR_TAMPER;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Provision button (event-driven — ISR doorbell, no polling)          */
/* ------------------------------------------------------------------ */

static void provisioning_button_tick(void)
{
    /* The ISR fired (coalesced with any comm notification). Debounce +
     * hold-to-arm runs here in task context; arm the window on a confirmed
     * hold. This is the physical-presence gate for provisioning (§11). */
    if (ProvisionButton_OnTaskNotified()) {
        if (provision_mgr_is_active()) {
            ESP_LOGW(TAG, "provisioning already active; ignoring button hold");
        } else if (provision_mgr_arm(APP_PROVISION_WINDOW_MS)) {
            Display_ShowIndication(DISPLAY_IND_PROVISIONING);
        } else {
            ESP_LOGE(TAG, "failed to arm provisioning window");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Session lifecycle handling                                          */
/* ------------------------------------------------------------------ */

static void handle_session_event(comm_app_event_t ev)
{
    /* Advisory only (master doc §10): never gate irreversible actions on
     * these events — UI indicator / logging only. */
    switch (ev) {
    case COMM_APP_EVENT_SESSION_STARTED:
        ESP_LOGI(TAG, "=== SESSION STARTED (advisory) ===");
        break;
    case COMM_APP_EVENT_SESSION_ENDED:
        ESP_LOGI(TAG, "=== SESSION ENDED (advisory) ===");
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Application task (bounded-wait loop)                                */
/* ------------------------------------------------------------------ */

static void app_task(void *arg)
{
    (void)arg;

    comm_err_t err = comm_module_register_app_task(xTaskGetCurrentTaskHandle());
    if (err != COMM_OK) {
        ESP_LOGE(TAG, "comm_module_register_app_task failed: %d", err);
        vTaskDelete(NULL);
        return;
    }

    /* Event-driven provision button: the ISR notifies THIS task. Init after
     * the task handle exists so the doorbell has a valid target. */
    if (ProvisionButton_Init(xTaskGetCurrentTaskHandle()) != PROV_BUTTON_OK) {
        ESP_LOGW(TAG, "provision button unavailable — provisioning via button disabled");
    }

    comm_module_start();
    ESP_LOGI(TAG, "app task registered; comm module started");

    const TickType_t period = pdMS_TO_TICKS(INTEGRITY_PERIOD_MS);

    while (1) {
        perform_integrity_checks();
        actuation_tick();

        /* Provision button: evaluate the ISR-doorbell state every iteration.
         * The ISR only records edges + notifies; the hold-elapsed check runs
         * here on the bounded-wait cadence, so a held button arms even without
         * a release edge. No polling of the pin happens anywhere. */
        provisioning_button_tick();

        if (ulTaskNotifyTake(pdTRUE, period)) {
            /* Drain BOTH the event and the command on every wake. */
            handle_session_event(comm_module_poll_event());

            if (comm_module_has_command()) {
                uint8_t cmd[APP_RESPONSE_BUF];
                size_t  cmd_len = 0;
                if (comm_module_get_command(cmd, sizeof(cmd), &cmd_len)
                        == COMM_OK) {
                    uint8_t resp[APP_RESPONSE_BUF];
                    size_t  resp_len = 0;

                    app_dispatch_ctx_t dctx = {
                        .lock_state          = current_lock_state(),
                        .last_error          = s_last_error,
                        .battery_pct         = APP_BATTERY_PCT_DEFAULT,
                        .session_authorized  = comm_module_session_active(),
                        .lock_pk             = s_lock_pk,
                        .provisioning_active = provision_mgr_is_active(),
                    };

                    bool produced =
                        app_dispatch(cmd, cmd_len, &dctx, resp, &resp_len);

                    /* Revoke-key removal is done here (post-dispatch) since
                     * app_dispatch is pure protocol. */
                    if (cmd[0] == CMD_REVOKE_KEY && cmd_len == (1 + APP_KEY_LEN)
                        && resp_len >= 1 && resp[0] == APP_STATUS_OK) {
                        const uint8_t *target = &cmd[1];
                        key_store_err_t kerr = key_store_revoke(target);
                        if (kerr == KEY_STORE_ERR_NOT_FOUND) {
                            resp[0] = APP_STATUS_NOT_FOUND;
                        } else if (kerr != KEY_STORE_OK) {
                            resp[0] = APP_STATUS_INTERNAL;
                        }
                    }

                    /* Reply IMMEDIATELY — the phone disconnects after the
                     * digital reply (§1 tap-and-go). Deliver the response
                     * first, THEN start the physical actuation. */
                    if (produced && resp_len > 0) {
                        comm_module_complete_response(resp, resp_len);
                    } else {
                        /* Empty response only when nothing was produced. */
                        comm_module_complete_response(resp, 0);
                    }

                    /* The ASYNC actuation for UNLOCK/LOCK starts only after
                     * the digital reply has been handed to the comm task. */
                    if (cmd[0] == CMD_UNLOCK && resp_len == 1
                        && resp[0] == APP_STATUS_OK) {
                        start_actuation(INTENT_TARGET_UNLOCKED);
                    } else if (cmd[0] == CMD_LOCK && resp_len == 1
                               && resp[0] == APP_STATUS_OK) {
                        start_actuation(INTENT_TARGET_LOCKED);
                    }
                } else {
                    ESP_LOGW(TAG, "get_command failed (comm wait may have timed out)");
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

comm_module_config_t AppModule_GetCommConfig(void)
{
    comm_module_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    /* Timing rule (§10): integrity period first, response timeout above it. */
    cfg.app_response_timeout_ms = APP_RESPONSE_TIMEOUT_MS;
    cfg.peer_key_provider       = AppModule_GetPeerKeyByIndex;
    cfg.peer_key_provider_ctx   = NULL;

    /* Load/generate the lock identity BEFORE this config is snapshotted by
     * comm_module_init — the session signs M2 with its copy of local_sk.
     * If this fails, local_sk/local_pk stay zero and AppModule_Init's own
     * (idempotent) call will report the failure. */
    if (key_store_identity_init() != KEY_STORE_OK) {
        ESP_LOGE(TAG, "identity init failed — lock identity unavailable");
    }

    /* Populate lock identity from the key store. */
    memcpy(cfg.local_sk, key_store_identity_sk(), 64);
    memcpy(cfg.local_pk, key_store_identity_pk(), 32);

    return cfg;
}

app_module_err_t AppModule_Init(const comm_module_config_t *comm_cfg)
{
    if (!comm_cfg) {
        return APP_MODULE_ERR_INVALID_ARG;
    }

    /* Identity init (idempotent): normally already done by
     * AppModule_GetCommConfig(), before comm_module_init snapshots
     * local_sk/local_pk. This call is a safety net for any path that
     * reaches AppModule_Init without assembling the comm config first. */
    if (key_store_identity_init() != KEY_STORE_OK) {
        ESP_LOGE(TAG, "identity init failed");
        return APP_MODULE_ERR_INIT;
    }

    /* Cache lock PK from the identity store, not from the (still-unpopulated)
     * comm_cfg. AppModule_GetCommConfig() fills local_pk afterward. */
    memcpy(s_lock_pk, key_store_identity_pk(), sizeof(s_lock_pk));

    s_last_error = APP_LAST_ERROR_NONE;

    /* Boot-state verification against the intent log (§2.3). */
    boot_recovery();

    return APP_MODULE_OK;
}

app_module_err_t AppModule_Start(void)
{
    BaseType_t res = xTaskCreate(app_task, "app_task",
                                 APP_TASK_STACK_SIZE, NULL,
                                 APP_TASK_PRIORITY, NULL);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "failed to create app task");
        return APP_MODULE_ERR_TASK;
    }
    return APP_MODULE_OK;
}
