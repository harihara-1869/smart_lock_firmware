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
 * @file storage_hal.c
 * @brief Slow-storage backend dispatcher — NVS (default) or SE (stub).
 *
 * Selected by CONFIG_STORAGE_BACKEND_NVS / CONFIG_STORAGE_BACKEND_SE at
 * build time. Only one backend is compiled into the binary.
 */

#include "storage_hal.h"
#include "sdkconfig.h"

#if defined(CONFIG_STORAGE_BACKEND_NVS)

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "STORAGE_NVS";

#define NVS_NS_KEYS      "keys"
#define NVS_NS_INTENT    "intent"

static nvs_handle_t s_keys_h;
static nvs_handle_t s_intent_h;
static bool s_inited = false;

storage_err_t storage_hal_init(void)
{
    if (s_inited) {
        return STORAGE_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return STORAGE_ERR_IO;
    }

    err = nvs_open(NVS_NS_KEYS, NVS_READWRITE, &s_keys_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(keys) failed: %s", esp_err_to_name(err));
        return STORAGE_ERR_IO;
    }

    err = nvs_open(NVS_NS_INTENT, NVS_READWRITE, &s_intent_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(intent) failed: %s", esp_err_to_name(err));
        nvs_close(s_keys_h);
        return STORAGE_ERR_IO;
    }

    s_inited = true;
    ESP_LOGI(TAG, "NVS storage backend ready");
    return STORAGE_OK;
}

static nvs_handle_t ns_to_handle(const char *ns)
{
    if (strcmp(ns, NVS_NS_KEYS) == 0)   return s_keys_h;
    if (strcmp(ns, NVS_NS_INTENT) == 0) return s_intent_h;
    return 0;
}

storage_err_t storage_hal_read_blob(const char *ns, const char *key,
                                    void *out, size_t *len)
{
    if (!s_inited || !out || !len || *len == 0) {
        return STORAGE_ERR_IO;
    }
    nvs_handle_t h = ns_to_handle(ns);
    if (h == 0) return STORAGE_ERR_IO;

    esp_err_t err = nvs_get_blob(h, key, out, len);
    if (err == ESP_OK) return STORAGE_OK;
    if (err == ESP_ERR_NVS_NOT_FOUND) return STORAGE_ERR_NOT_FOUND;

    ESP_LOGE(TAG, "nvs_get_blob(%s/%s) failed: %s", ns, key, esp_err_to_name(err));
    return STORAGE_ERR_IO;
}

storage_err_t storage_hal_write_blob(const char *ns, const char *key,
                                     const void *in, size_t len)
{
    if (!s_inited || !in || len == 0) {
        return STORAGE_ERR_IO;
    }
    nvs_handle_t h = ns_to_handle(ns);
    if (h == 0) return STORAGE_ERR_IO;

    esp_err_t err = nvs_set_blob(h, key, in, len);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
            ESP_LOGE(TAG, "NVS full: %s/%s", ns, key);
            return STORAGE_ERR_FULL;
        }
        ESP_LOGE(TAG, "nvs_set_blob(%s/%s) failed: %s", ns, key, esp_err_to_name(err));
        return STORAGE_ERR_IO;
    }

    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(%s) failed: %s", ns, esp_err_to_name(err));
        return STORAGE_ERR_IO;
    }
    return STORAGE_OK;
}

storage_err_t storage_hal_erase_key(const char *ns, const char *key)
{
    if (!s_inited) {
        return STORAGE_ERR_IO;
    }
    nvs_handle_t h = ns_to_handle(ns);
    if (h == 0) return STORAGE_ERR_IO;

    esp_err_t err = nvs_erase_key(h, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(h);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_commit after erase failed: %s", esp_err_to_name(err));
            return STORAGE_ERR_IO;
        }
        return STORAGE_OK;
    }
    ESP_LOGE(TAG, "nvs_erase_key(%s/%s) failed: %s", ns, key, esp_err_to_name(err));
    return STORAGE_ERR_IO;
}

#elif defined(CONFIG_STORAGE_BACKEND_SE)

#include "esp_log.h"

static const char *TAG = "STORAGE_SE";

storage_err_t storage_hal_init(void)
{
    ESP_LOGW(TAG, "TODO(hardware): SE backend not implemented — no persistence");
    return STORAGE_OK;
}

storage_err_t storage_hal_read_blob(const char *ns, const char *key,
                                    void *out, size_t *len)
{
    (void)ns; (void)key; (void)out; (void)len;
    ESP_LOGW(TAG, "TODO(hardware): SE read not implemented (%s/%s)", ns, key);
    return STORAGE_ERR_IO;
}

storage_err_t storage_hal_write_blob(const char *ns, const char *key,
                                     const void *in, size_t len)
{
    (void)ns; (void)key; (void)in; (void)len;
    ESP_LOGW(TAG, "TODO(hardware): SE write not implemented (%s/%s)", ns, key);
    return STORAGE_ERR_IO;
}

storage_err_t storage_hal_erase_key(const char *ns, const char *key)
{
    (void)ns; (void)key;
    ESP_LOGW(TAG, "TODO(hardware): SE erase not implemented (%s/%s)", ns, key);
    return STORAGE_ERR_IO;
}

#else
#error "No STORAGE_BACKEND selected — check Kconfig"
#endif
