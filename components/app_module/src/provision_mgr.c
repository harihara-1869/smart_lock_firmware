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

#include "provision_mgr.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "app_cmd.h"
#include "app_module.h"   /* AppModule_GetLockPk() — owned by the Application */
#include "comm_module.h"
#include "display.h"
#include "key_store.h"

static const char *TAG = "PROV_MGR";

/* Application-side window state. */
static bool       s_active = false;
static int64_t    s_deadline_us = 0;
static uint8_t    s_secret[APP_SECRET_LEN];

/* Constant-time memory comparison. */
static int constant_time_memcmp(const void *a, const void *b, size_t len)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= pa[i] ^ pb[i];
    }
    return diff == 0 ? 0 : 1;
}

static void disarm(void)
{
    s_active = false;
    Display_Clear();
}

bool provision_mgr_arm(uint32_t timeout_ms)
{
    esp_fill_random(s_secret, sizeof(s_secret));

    if (comm_module_arm_provisioning_window(timeout_ms) != COMM_OK) {
        ESP_LOGE(TAG, "failed to arm comm provisioning window");
        return false;
    }

    s_deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    s_active = true;

    Display_RenderQR(s_secret, sizeof(s_secret));
    Display_ShowIndication(DISPLAY_IND_PROVISIONING);
    ESP_LOGI(TAG, "provisioning armed for %lu ms",
             (unsigned long)timeout_ms);
    return true;
}

bool provision_mgr_is_active(void)
{
    if (s_active && esp_timer_get_time() > s_deadline_us) {
        ESP_LOGW(TAG, "provisioning window expired");
        disarm();
    }
    return s_active;
}

void provision_mgr_abort(void)
{
    ESP_LOGW(TAG, "provisioning aborted by application");
    comm_module_force_abort();
    disarm();
}

void provision_mgr_handle_cmd(const uint8_t *cmd_bytes, size_t len,
                              uint8_t *resp_bytes, size_t *resp_len)
{
    *resp_len = 0;

    if (!provision_mgr_is_active()) {
        ESP_LOGE(TAG, "provisioning not active or expired");
        resp_bytes[0] = APP_STATUS_INVALID_CMD;
        *resp_len = 1;
        return;
    }

    if (!cmd_bytes || len != (1 + APP_SECRET_LEN + APP_KEY_LEN)) {
        ESP_LOGE(TAG, "invalid CMD_PROVISION length: %u", (unsigned)len);
        resp_bytes[0] = APP_STATUS_INVALID_CMD;
        *resp_len = 1;
        disarm(); /* single-use: window closes after one attempt */
        return;
    }

    const uint8_t *received_secret = &cmd_bytes[1];
    const uint8_t *claimed_pk      = &cmd_bytes[1 + APP_SECRET_LEN];

    /* Step 1: constant-time Provision Secret comparison. */
    if (constant_time_memcmp(received_secret, s_secret, APP_SECRET_LEN) != 0) {
        ESP_LOGE(TAG, "provisioning secret mismatch");
        resp_bytes[0] = APP_STATUS_INVALID_SECRET;
        *resp_len = 1;
        disarm(); /* single-use: window closes after one attempt */
        return;
    }

    /* Step 2: deferred identity check — verify the CACHED M3 Sig_P from the
     * provisioning-mode handshake against the claimed public key. Never
     * skipped: this is the only path that can commit a new identity. */
    if (!comm_module_provision_verify_identity(claimed_pk)) {
        ESP_LOGE(TAG, "identity verification failed for claimed key");
        resp_bytes[0] = APP_STATUS_INVALID_SECRET;
        *resp_len = 1;
        disarm(); /* single-use: window closes after one attempt */
        return;
    }

    /* Step 3: commit the new identity to the key store. */
    if (KeyStore_IsAuthorized(claimed_pk)) {
        ESP_LOGW(TAG, "key already provisioned");
        resp_bytes[0] = APP_STATUS_KEY_EXISTS;
        *resp_len = 1;
    } else if (KeyStore_AddKey(claimed_pk) != KEY_STORE_OK) {
        ESP_LOGE(TAG, "failed to store key");
        resp_bytes[0] = APP_STATUS_INTERNAL;
        *resp_len = 1;
    } else {
        ESP_LOGI(TAG, "provisioning successful — key committed");
        resp_bytes[0] = APP_STATUS_OK;
        memcpy(&resp_bytes[1], provision_mgr_get_lock_pk(), APP_KEY_LEN);
        *resp_len = 1 + APP_KEY_LEN;
    }

    /* Single-use: the window closes after one provision attempt. */
    disarm();
    Display_ShowIndication(DISPLAY_IND_SUCCESS);
}

const uint8_t *provision_mgr_get_secret(void)
{
    return s_secret;
}

const uint8_t *provision_mgr_get_lock_pk(void)
{
    return AppModule_GetLockPk();
}
