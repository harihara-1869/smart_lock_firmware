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

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "comm_module.h"
#include "provision_mgr.h"
#include "nvs_store.h"

static const char *TAG = "APP";

/* ------------------------------------------------------------------ */
/* Test Identity Keys — clearly test-only                              */
/* ------------------------------------------------------------------ */

/* TEST KEY ONLY — DO NOT USE IN PRODUCTION. Generated for stack smoke-testing. */
static const uint8_t TEST_LOCK_SECRET_KEY[64] = {
    0x04, 0xce, 0xf8, 0x21, 0x03, 0xee, 0xd1, 0x20, 0x50, 0x96, 0x82, 0xc6,
    0x89, 0xb1, 0x76, 0xe1, 0x58, 0x59, 0x11, 0x99, 0xd3, 0xd8, 0x06, 0xbc,
    0xb5, 0x70, 0x57, 0xae, 0xe6, 0xd5, 0xa6, 0x63, 0x60, 0x86, 0x37, 0xc5,
    0x23, 0x61, 0x6c, 0xf5, 0xdd, 0xbb, 0xf9, 0xc4, 0xcf, 0xf9, 0xd1, 0x2a,
    0x45, 0x64, 0x19, 0x2b, 0x81, 0x25, 0x07, 0x66, 0xf2, 0x8d, 0x6d, 0x4c,
    0x00, 0x10, 0x52, 0x3f
};

/* TEST KEY ONLY — DO NOT USE IN PRODUCTION. Generated for stack smoke-testing. */
static const uint8_t TEST_LOCK_PUBLIC_KEY[32] = {
    0x60, 0x86, 0x37, 0xc5, 0x23, 0x61, 0x6c, 0xf5, 0xdd, 0xbb, 0xf9, 0xc4,
    0xcf, 0xf9, 0xd1, 0x2a, 0x45, 0x64, 0x19, 0x2b, 0x81, 0x25, 0x07, 0x66,
    0xf2, 0x8d, 0x6d, 0x4c, 0x00, 0x10, 0x52, 0x3f
};

/* TEST KEY ONLY — DO NOT USE IN PRODUCTION. Generated for stack smoke-testing. */
static const uint8_t TEST_PHONE_SECRET_KEY[64] = {
    0xa5, 0x83, 0x57, 0xf5, 0xe1, 0xfe, 0xd5, 0xca, 0x5b, 0xf8, 0xd1, 0x68,
    0x40, 0x3b, 0xa6, 0x45, 0xa1, 0x4e, 0x7e, 0xa6, 0x48, 0xa1, 0xdd, 0xd6,
    0x57, 0xac, 0x17, 0x6a, 0xb4, 0x25, 0xec, 0x79, 0xa1, 0x4e, 0xe9, 0x1a,
    0xea, 0x33, 0x54, 0x03, 0x1e, 0x50, 0xb1, 0x74, 0xd2, 0x01, 0x16, 0x43,
    0xf4, 0xa0, 0x34, 0xa3, 0xcb, 0x25, 0xc3, 0x4b, 0xdc, 0x07, 0xcd, 0xcb,
    0x20, 0x00, 0x8c, 0x49
};

/* TEST KEY ONLY — DO NOT USE IN PRODUCTION. Generated for stack smoke-testing. */
static const uint8_t TEST_PHONE_PUBLIC_KEY[32] = {
    0xa1, 0x4e, 0xe9, 0x1a, 0xea, 0x33, 0x54, 0x03, 0x1e, 0x50, 0xb1, 0x74,
    0xd2, 0x01, 0x16, 0x43, 0xf4, 0xa0, 0x34, 0xa3, 0xcb, 0x25, 0xc3, 0x4b,
    0xdc, 0x07, 0xcd, 0xcb, 0x20, 0x00, 0x8c, 0x49
};

/* Single-candidate key provider for M3 authentication */
static bool test_peer_key_provider(size_t index, uint8_t pubkey_out[32], void *ctx)
{
    (void)ctx;
    if (index != 0) {
        return false;
    }
    memcpy(pubkey_out, TEST_PHONE_PUBLIC_KEY, 32);
    return true;
}

/* ------------------------------------------------------------------ */
/* Application Integrity Check Stand-in                                */
/* ------------------------------------------------------------------ */

static void perform_integrity_checks(void)
{
    vTaskDelay(pdMS_TO_TICKS(6));
}

/* ------------------------------------------------------------------ */
/* Application Task Loop                                              */
/* ------------------------------------------------------------------ */

static void app_task(void *arg)
{
    (void)arg;

    /* Must register task handle with comm_module BEFORE calling comm_module_start() */
    comm_err_t err = comm_module_register_app_task(xTaskGetCurrentTaskHandle());
    if (err != COMM_OK) {
        ESP_LOGE(TAG, "Failed to register app task: %d", err);
        vTaskDelete(NULL);
        return;
    }

    comm_module_start();
    ESP_LOGI(TAG, "Application task registered and comm_module started.");

    /* Simulate provisioning button press for test */
    ESP_LOGI(TAG, "Simulating physical button press to start provisioning...");
    provision_mgr_arm(60000); /* 60 seconds */

    const TickType_t period = pdMS_TO_TICKS(50);

    while (1) {
        perform_integrity_checks();

        if (ulTaskNotifyTake(pdTRUE, period)) {
            /* Service pending lifecycle event */
            comm_app_event_t ev = comm_module_poll_event();
            if (ev == COMM_APP_EVENT_SESSION_STARTED) {
                ESP_LOGI(TAG, "=== SESSION STARTED ===");
            } else if (ev == COMM_APP_EVENT_SESSION_ENDED) {
                ESP_LOGI(TAG, "=== SESSION ENDED ===");
            }

            /* Service pending command (both event and command can occur in one wake) */
            if (comm_module_has_command()) {
                uint8_t cmd[227];
                size_t cmd_len = 0;
                err = comm_module_get_command(cmd, sizeof(cmd), &cmd_len);
                if (err == COMM_OK) {
                    ESP_LOGI(TAG, "Received command (%zu bytes):", cmd_len);
                    ESP_LOG_BUFFER_HEX(TAG, cmd, cmd_len);

                    uint8_t resp[227];
                    size_t resp_len = 0;
                    
                    if (provision_mgr_is_active()) {
                        provision_mgr_handle_cmd(cmd, cmd_len, resp, &resp_len);
                    } else {
                        if (cmd_len + 1 <= sizeof(resp)) {
                            resp[0] = 0x00; /* Status OK */
                            memcpy(&resp[1], cmd, cmd_len);
                            resp_len = cmd_len + 1;
                        } else {
                            ESP_LOGW(TAG, "Response would exceed buffer (%zu bytes); truncating", cmd_len + 1);
                            resp[0] = 0x00;
                            memcpy(&resp[1], cmd, sizeof(resp) - 1);
                            resp_len = sizeof(resp);
                        }
                    }

                    comm_module_complete_response(resp, resp_len);
                } else {
                    ESP_LOGE(TAG, "comm_module_get_command failed: %d", err);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry Point                                                        */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Smart Lock Firmware — Application Module Smoke Test ===");

    ESP_LOGI(TAG, "--- Companion Client Test Keys ---");
    ESP_LOGI(TAG, "Phone Public Key (32 bytes):");
    ESP_LOG_BUFFER_HEX(TAG, TEST_PHONE_PUBLIC_KEY, 32);
    ESP_LOGI(TAG, "Phone Secret Key (64 bytes: seed 32 + pk 32):");
    ESP_LOG_BUFFER_HEX(TAG, TEST_PHONE_SECRET_KEY, 64);

    comm_module_config_t cfg = {
        .sda_gpio = 8,
        .scl_gpio = 9,
        .irq_gpio = 10,
        .rst_gpio = 11,
        .i2c_port = I2C_NUM_0,
        .i2c_clk_hz = 0, /* 400 kHz default */

        .sens_res    = {0x04, 0x00},
        .nfcid1      = {0x01, 0x02, 0x03},
        .sel_res     = 0x20, /* ISO14443-4 */
        .nfcid2      = {0x01, 0xFE, 0xA5, 0x01, 0x02, 0x03, 0x04, 0x05},
        .pad         = {0},
        .system_code = {0x88, 0xB4},
        .nfcid3t     = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A},
        .gt_len      = 0,
        .tk_len      = 0,

        .handshake_timeout_ms = 2000,
        .activate_timeout_ms  = 30000,
        .apdu_timeout_ms      = 5000,

        .peer_key_provider     = test_peer_key_provider,
        .peer_key_provider_ctx = NULL,

        /* app_response_timeout_ms: 200 ms.
         * With a 6 ms integrity check and 50 ms bounded wait period, 200 ms
         * gives up to 4 loop iterations for the Application task to wake, execute
         * the integrity check, and process the command before the comm task's
         * wait expires, avoiding false response timeouts while keeping
         * hung-command recovery fast. */
        .app_response_timeout_ms = 200,

        .task_stack_size = 0, /* Facade default: 8192 bytes */
        .task_priority   = 0, /* Facade default: 5 */
        /* Explicitly set tskNO_AFFINITY. Zero-initializing task_core_id would
         * evaluate to 0 (Core 0) rather than tskNO_AFFINITY (0x7FFFFFFF), which
         * would pin the comm task to Core 0 instead of leaving it unpinned. */
        .task_core_id    = tskNO_AFFINITY,
    };
    memcpy(cfg.local_sk, TEST_LOCK_SECRET_KEY, sizeof(cfg.local_sk));
    memcpy(cfg.local_pk, TEST_LOCK_PUBLIC_KEY, sizeof(cfg.local_pk));

    comm_err_t err = comm_module_init(&cfg);
    if (err != COMM_OK) {
        ESP_LOGE(TAG, "comm_module_init failed: %d", err);
        return;
    }

    BaseType_t res = xTaskCreate(app_task, "app_task", 4096, NULL, 5, NULL);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create app_task");
    }
}
