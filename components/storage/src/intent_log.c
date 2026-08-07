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
 * @file intent_log.c
 * @brief Actuation-intent log — RAM cache + write-through persistence.
 */

#include "intent_log.h"

#include "esp_log.h"
#include "storage_hal.h"

static const char *TAG = "INTENT_LOG";

#define HAL_NS  "intent"
#define HAL_KEY "target"

static intent_target_t s_target = INTENT_TARGET_NONE;

intent_err_t intent_log_init(void)
{
    /* Ensure the HAL is ready before reading. */
    storage_err_t serr = storage_hal_init();
    if (serr != STORAGE_OK) {
        ESP_LOGE(TAG, "hal init failed: %d", serr);
        return INTENT_ERR_STORAGE;
    }

    size_t len = sizeof(s_target);
    serr = storage_hal_read_blob(HAL_NS, HAL_KEY,
                                  &s_target, &len);
    if (serr == STORAGE_ERR_NOT_FOUND) {
        s_target = INTENT_TARGET_NONE;
        ESP_LOGI(TAG, "no persisted intent — starting clean");
        return INTENT_OK;
    }
    if (serr != STORAGE_OK) {
        ESP_LOGE(TAG, "cache warm failed: hal error %d", serr);
        return INTENT_ERR_STORAGE;
    }
    if (len != sizeof(s_target)) {
        ESP_LOGE(TAG, "corrupt intent blob (len %u)", (unsigned)len);
        return INTENT_ERR_STORAGE;
    }
    ESP_LOGI(TAG, "cache warmed: target=%d", (int)s_target);
    return INTENT_OK;
}

intent_err_t intent_log_write(intent_target_t target)
{
    /* Write-through: HAL first. On failure, leave RAM untouched. */
    storage_err_t serr;
    if (target == INTENT_TARGET_NONE) {
        serr = storage_hal_erase_key(HAL_NS, HAL_KEY);
    } else {
        serr = storage_hal_write_blob(HAL_NS, HAL_KEY, &target, sizeof(target));
    }
    if (serr != STORAGE_OK) {
        ESP_LOGE(TAG, "hal write failed: %d", serr);
        return INTENT_ERR_STORAGE;
    }

    s_target = target;
    return INTENT_OK;
}

intent_target_t intent_log_get_cached(void)
{
    return s_target;
}
