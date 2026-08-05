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
 * @file smart_lock_firmware.c
 * @brief Firmware entry point — dependency-order init + test-mode harnesses.
 *
 * app_main() branches on CONFIG_SMART_LOCK_TEST_MODE (root Kconfig):
 *
 *   FULL_APPLICATION  : production path — init every peer in dependency
 *                       order, then hand control to the Application task.
 *   COMM_ONLY         : comm stack wired to a minimal echo/status loop. No
 *                       actuator/display/integrity init.
 *   ACTUATOR_ONLY     : AAI exercised in isolation.
 *   DISPLAY_ONLY      : Display peer exercised in isolation.
 *   INTEGRITY_ONLY    : Integrity peer exercised in isolation.
 *
 * Every mode must build; each harness initializes ONLY the modules it needs.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "aai.h"
#include "app_module.h"
#include "app_cmd.h"
#include "comm_module.h"
#include "display.h"
#include "integrity.h"
#include "intent_log.h"
#include "key_store.h"
#include "provision_mgr.h"

#ifdef MOCK_LLI_FOR_TESTING
#include "test_integration.h"
#endif

static const char *TAG = "APP";

/* ------------------------------------------------------------------ */
/* Test Identity Keys — clearly test-only                              */
/* ------------------------------------------------------------------ */

/* TEST KEY ONLY — DO NOT USE IN PRODUCTION. */
const uint8_t TEST_LOCK_SECRET_KEY[64] = {
    0x04, 0xce, 0xf8, 0x21, 0x03, 0xee, 0xd1, 0x20, 0x50, 0x96, 0x82, 0xc6,
    0x89, 0xb1, 0x76, 0xe1, 0x58, 0x59, 0x11, 0x99, 0xd3, 0xd8, 0x06, 0xbc,
    0xb5, 0x70, 0x57, 0xae, 0xe6, 0xd5, 0xa6, 0x63, 0x60, 0x86, 0x37, 0xc5,
    0x23, 0x61, 0x6c, 0xf5, 0xdd, 0xbb, 0xf9, 0xc4, 0xcf, 0xf9, 0xd1, 0x2a,
    0x45, 0x64, 0x19, 0x2b, 0x81, 0x25, 0x07, 0x66, 0xf2, 0x8d, 0x6d, 0x4c,
    0x00, 0x10, 0x52, 0x3f
};

/* TEST KEY ONLY — DO NOT USE IN PRODUCTION. */
const uint8_t TEST_LOCK_PUBLIC_KEY[32] = {
    0x60, 0x86, 0x37, 0xc5, 0x23, 0x61, 0x6c, 0xf5, 0xdd, 0xbb, 0xf9, 0xc4,
    0xcf, 0xf9, 0xd1, 0x2a, 0x45, 0x64, 0x19, 0x2b, 0x81, 0x25, 0x07, 0x66,
    0xf2, 0x8d, 0x6d, 0x4c, 0x00, 0x10, 0x52, 0x3f
};

/* TEST KEY ONLY — DO NOT USE IN PRODUCTION. */
const uint8_t TEST_PHONE_SECRET_KEY[64] = {
    0xa5, 0x83, 0x57, 0xf5, 0xe1, 0xfe, 0xd5, 0xca, 0x5b, 0xf8, 0xd1, 0x68,
    0x40, 0x3b, 0xa6, 0x45, 0xa1, 0x4e, 0x7e, 0xa6, 0x48, 0xa1, 0xdd, 0xd6,
    0x57, 0xac, 0x17, 0x6a, 0xb4, 0x25, 0xec, 0x79, 0xa1, 0x4e, 0xe9, 0x1a,
    0xea, 0x33, 0x54, 0x03, 0x1e, 0x50, 0xb1, 0x74, 0xd2, 0x01, 0x16, 0x43,
    0xf4, 0xa0, 0x34, 0xa3, 0xcb, 0x25, 0xc3, 0x4b, 0xdc, 0x07, 0xcd, 0xcb,
    0x20, 0x00, 0x8c, 0x49
};

/* TEST KEY ONLY — DO NOT USE IN PRODUCTION. */
const uint8_t TEST_PHONE_PUBLIC_KEY[32] = {
    0xa1, 0x4e, 0xe9, 0x1a, 0xea, 0x33, 0x54, 0x03, 0x1e, 0x50, 0xb1, 0x74,
    0xd2, 0x01, 0x16, 0x43, 0xf4, 0xa0, 0x34, 0xa3, 0xcb, 0x25, 0xc3, 0x4b,
    0xdc, 0x07, 0xcd, 0xcb, 0x20, 0x00, 0x8c, 0x49
};

/* ------------------------------------------------------------------ */
/* Comm config (identity + timing; GPIO/LLI fields filled by caller)   */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_TEST_MODE_FULL_APPLICATION) || defined(CONFIG_TEST_MODE_COMM_ONLY)
static comm_module_config_t build_comm_config(void)
{
    comm_module_config_t cfg = AppModule_GetCommConfig();

    /* LLI / I2C — see README "Default GPIO Mapping". */
    cfg.sda_gpio = 8;
    cfg.scl_gpio = 9;
    cfg.irq_gpio = 10;
    cfg.rst_gpio = 11;
    cfg.i2c_port = I2C_NUM_0;
    cfg.i2c_clk_hz = 0; /* 400 kHz default */

    /* Card identity (test values). */
    const uint8_t sens_res[2]    = {0x04, 0x00};
    const uint8_t nfcid1[3]      = {0x01, 0x02, 0x03};
    const uint8_t nfcid2[8]      = {0x01, 0xFE, 0xA5, 0x01, 0x02, 0x03, 0x04, 0x05};
    const uint8_t pad[8]         = {0};
    const uint8_t system_code[2] = {0x88, 0xB4};
    const uint8_t nfcid3t[10]    = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
    memcpy(cfg.sens_res,    sens_res,    sizeof(cfg.sens_res));
    memcpy(cfg.nfcid1,      nfcid1,      sizeof(cfg.nfcid1));
    cfg.sel_res = 0x20;
    memcpy(cfg.nfcid2,      nfcid2,      sizeof(cfg.nfcid2));
    memcpy(cfg.pad,         pad,         sizeof(cfg.pad));
    memcpy(cfg.system_code, system_code, sizeof(cfg.system_code));
    memcpy(cfg.nfcid3t,     nfcid3t,     sizeof(cfg.nfcid3t));

    /* Timing. */
    cfg.handshake_timeout_ms = 2000;
    cfg.activate_timeout_ms  = 30000;
    cfg.apdu_timeout_ms      = 5000;

    /* Lock identity. */
    memcpy(cfg.local_sk, TEST_LOCK_SECRET_KEY, sizeof(cfg.local_sk));
    memcpy(cfg.local_pk, TEST_LOCK_PUBLIC_KEY, sizeof(cfg.local_pk));

    /* Unpinned comm task (0 = facade default stack/priority). */
    cfg.task_stack_size = 0;
    cfg.task_priority   = 0;
    cfg.task_core_id    = tskNO_AFFINITY;

    return cfg;
}
#endif /* FULL || COMM_ONLY */

/* ------------------------------------------------------------------ */
/* Peer init (dependency order)                                        */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_TEST_MODE_FULL_APPLICATION) || defined(CONFIG_TEST_MODE_COMM_ONLY)
static bool init_storage(void)
{
    if (KeyStore_Init() != KEY_STORE_OK) {
        ESP_LOGE(TAG, "KeyStore_Init failed");
        return false;
    }
    if (IntentLog_Init() != INTENT_OK) {
        ESP_LOGE(TAG, "IntentLog_Init failed");
        return false;
    }
    return true;
}
#endif

#if defined(CONFIG_TEST_MODE_FULL_APPLICATION) || defined(CONFIG_TEST_MODE_DISPLAY_ONLY)
static bool init_display(void)
{
    if (Display_Init() != DISPLAY_OK) {
        ESP_LOGE(TAG, "Display_Init failed");
        return false;
    }
    return true;
}
#endif

#if defined(CONFIG_TEST_MODE_FULL_APPLICATION) || defined(CONFIG_TEST_MODE_ACTUATOR_ONLY)
static bool init_actuator(void)
{
    if (AAI_Init() != AAI_OK) {
        ESP_LOGE(TAG, "AAI_Init failed");
        return false;
    }
    return true;
}
#endif

#if defined(CONFIG_TEST_MODE_FULL_APPLICATION) || defined(CONFIG_TEST_MODE_INTEGRITY_ONLY)
static bool init_integrity(void)
{
    if (Integrity_Init() != INTEGRITY_INIT_OK) {
        ESP_LOGE(TAG, "Integrity_Init failed");
        return false;
    }
    return true;
}
#endif

/* ------------------------------------------------------------------ */
/* Test-mode harnesses                                                 */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_TEST_MODE_ACTUATOR_ONLY)

static void actuator_test_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "ACTUATOR_ONLY harness started");

    /* Exercise the AAI interface through its full lifecycle. */
    for (int i = 0; i < 3; i++) {
        ESP_LOGI(TAG, "--- cycle %d: open ---", i);
        aai_err_t err = AAI_Open();
        ESP_LOGI(TAG, "AAI_Open -> %d", (int)err);
        for (int t = 0; t < 50; t++) {
            aai_state_t st = AAI_GetStatus();
            ESP_LOGI(TAG, "status: %d", (int)st);
            if (st != AAI_STATE_MOVING) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        ESP_LOGI(TAG, "--- cycle %d: close ---", i);
        err = AAI_Close();
        ESP_LOGI(TAG, "AAI_Close -> %d", (int)err);
        for (int t = 0; t < 50; t++) {
            aai_state_t st = AAI_GetStatus();
            ESP_LOGI(TAG, "status: %d", (int)st);
            if (st != AAI_STATE_MOVING) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    ESP_LOGI(TAG, "--- stop test ---");
    AAI_Stop();
    ESP_LOGI(TAG, "final status: %d", (int)AAI_GetStatus());

    ESP_LOGI(TAG, "ACTUATOR_ONLY harness complete");
    vTaskDelete(NULL);
}

#elif defined(CONFIG_TEST_MODE_DISPLAY_ONLY)

static void display_test_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "DISPLAY_ONLY harness started");

    Display_ShowText("Smart Lock — display test");
    Display_ShowIndication(DISPLAY_IND_SUCCESS);
    vTaskDelay(pdMS_TO_TICKS(200));
    Display_ShowIndication(DISPLAY_IND_ERROR);
    vTaskDelay(pdMS_TO_TICKS(200));
    Display_ShowIndication(DISPLAY_IND_PROVISIONING);
    vTaskDelay(pdMS_TO_TICKS(200));
    Display_PlayTone(880, 200);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Render a sample QR payload. */
    const uint8_t qr[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    Display_RenderQR(qr, sizeof(qr));
    vTaskDelay(pdMS_TO_TICKS(200));
    Display_Clear();

    ESP_LOGI(TAG, "DISPLAY_ONLY harness complete");
    vTaskDelete(NULL);
}

#elif defined(CONFIG_TEST_MODE_INTEGRITY_ONLY)

static void integrity_test_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "INTEGRITY_ONLY harness started");

    for (int i = 0; i < 10; i++) {
        integrity_status_t st = Integrity_RunChecks();
        ESP_LOGI(TAG, "check %d: %d (tampered=%d)",
                 i, (int)st, (int)Integrity_IsTampered());
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "INTEGRITY_ONLY harness complete");
    vTaskDelete(NULL);
}

#elif defined(CONFIG_TEST_MODE_COMM_ONLY)

/* Minimal echo/status loop for the comm stack — no actuator/display/integrity. */
static void comm_only_task(void *arg)
{
    (void)arg;

    comm_err_t err = comm_module_register_app_task(xTaskGetCurrentTaskHandle());
    if (err != COMM_OK) {
        ESP_LOGE(TAG, "register_app_task failed: %d", err);
        vTaskDelete(NULL);
        return;
    }
    comm_module_start();

    const TickType_t period = pdMS_TO_TICKS(50);
    while (1) {
        if (ulTaskNotifyTake(pdTRUE, period)) {
            comm_app_event_t ev = comm_module_poll_event();
            if (ev == COMM_APP_EVENT_SESSION_STARTED) {
                ESP_LOGI(TAG, "=== SESSION STARTED ===");
            } else if (ev == COMM_APP_EVENT_SESSION_ENDED) {
                ESP_LOGI(TAG, "=== SESSION ENDED ===");
            }

            if (comm_module_has_command()) {
                uint8_t cmd[227];
                size_t  cmd_len = 0;
                if (comm_module_get_command(cmd, sizeof(cmd), &cmd_len)
                        == COMM_OK) {
                    ESP_LOGI(TAG, "echo command (%u bytes):", (unsigned)cmd_len);
                    ESP_LOG_BUFFER_HEX(TAG, cmd, cmd_len);
                    /* Echo the command back as the response. */
                    comm_module_complete_response(cmd, cmd_len);
                }
            }
        }
    }
}

#endif /* test-mode harness selection */

/* ------------------------------------------------------------------ */
/* Full Application path                                               */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_TEST_MODE_FULL_APPLICATION)

static void run_full_application(void)
{
    ESP_LOGI(TAG, "=== Full Application: dependency-order init ===");

    /* 1. Storage (key store + intent log) — no nvs_flash here; the storage
     *    component owns persistence. */
    if (!init_storage()) {
        ESP_LOGE(TAG, "storage init failed; aborting");
        return;
    }
    /* 2. Display. */
    if (!init_display()) {
        ESP_LOGE(TAG, "display init failed; aborting");
        return;
    }
    /* 3. Actuator — boot-state verification against the intent log happens
     *    inside AppModule_Init (after this, so the intent log is readable). */
    if (!init_actuator()) {
        ESP_LOGE(TAG, "actuator init failed; aborting");
        return;
    }
    /* 4. Integrity. */
    if (!init_integrity()) {
        ESP_LOGE(TAG, "integrity init failed; aborting");
        return;
    }

    /* 5. Comm module. */
    comm_module_config_t cfg = build_comm_config();
    comm_err_t err = comm_module_init(&cfg);
    if (err != COMM_OK) {
        ESP_LOGE(TAG, "comm_module_init failed: %d", err);
        return;
    }

    /* 6. Application coordinator (boot recovery runs inside AppModule_Init). */
    if (AppModule_Init(&cfg) != APP_MODULE_OK) {
        ESP_LOGE(TAG, "AppModule_Init failed");
        return;
    }

    /* 7. Spawn the Application task; it registers with the facade and takes
     *    over as coordinator. */
    if (AppModule_Start() != APP_MODULE_OK) {
        ESP_LOGE(TAG, "AppModule_Start failed");
        return;
    }

#ifdef MOCK_LLI_FOR_TESTING
    /* Let the app task register/start, then run the mock-phone integration
     * test (provisioning flow, key persistence). */
    vTaskDelay(pdMS_TO_TICKS(100));
    xTaskCreate((TaskFunction_t)test_integration_run, "test_task",
                8192, NULL, 5, NULL);
#endif

    /* Keep main task alive so it doesn't clean up memory under other tasks. */
    vTaskSuspend(NULL);
}

#endif /* CONFIG_TEST_MODE_FULL_APPLICATION */

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Smart Lock Firmware ===");
    ESP_LOGI(TAG, "test mode: %s",
#if defined(CONFIG_TEST_MODE_FULL_APPLICATION)
             "FULL_APPLICATION");
    run_full_application();
#elif defined(CONFIG_TEST_MODE_COMM_ONLY)
             "COMM_ONLY");

    /* Storage only (key provider needs it); no actuator/display/integrity. */
    if (!init_storage()) {
        ESP_LOGE(TAG, "storage init failed");
        return;
    }
    comm_module_config_t cfg = build_comm_config();
    if (comm_module_init(&cfg) != COMM_OK) {
        ESP_LOGE(TAG, "comm_module_init failed");
        return;
    }
    xTaskCreate(comm_only_task, "comm_only", 4096, NULL, 5, NULL);
    vTaskSuspend(NULL);
#elif defined(CONFIG_TEST_MODE_ACTUATOR_ONLY)
             "ACTUATOR_ONLY");
    if (!init_actuator()) {
        ESP_LOGE(TAG, "actuator init failed");
        return;
    }
    xTaskCreate(actuator_test_task, "actuator_test", 4096, NULL, 5, NULL);
    vTaskSuspend(NULL);
#elif defined(CONFIG_TEST_MODE_DISPLAY_ONLY)
             "DISPLAY_ONLY");
    if (!init_display()) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }
    xTaskCreate(display_test_task, "display_test", 4096, NULL, 5, NULL);
    vTaskSuspend(NULL);
#elif defined(CONFIG_TEST_MODE_INTEGRITY_ONLY)
             "INTEGRITY_ONLY");
    if (!init_integrity()) {
        ESP_LOGE(TAG, "integrity init failed");
        return;
    }
    xTaskCreate(integrity_test_task, "integrity_test", 4096, NULL, 5, NULL);
    vTaskSuspend(NULL);
#endif
}
