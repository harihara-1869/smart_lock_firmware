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
 * @file key_store_ram.c
 * @brief Provisional RAM placeholder for the authorized-key store.
 *
 * TODO(storage): provisional RAM backend — NO power-loss persistence. Keys
 * are lost on reset, exactly like the original in-RAM nvs_store.c array this
 * supersedes. The planned NVS / secure-element + RAM + write-policy backends
 * will replace this file behind the same key_store.h interface; no call site
 * changes.
 *
 * Every mutating call logs that persistence is not real.
 */

#include "key_store.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "KEY_STORE";

/* Reuse the pre-existing in-RAM array semantics (was nvs_store.c). */
static uint8_t s_keys[KEY_STORE_MAX_KEYS][32];
static size_t  s_num_keys = 0;

key_store_err_t KeyStore_Init(void)
{
    /* RAM placeholder: nothing to initialise; keys start empty on boot. */
    s_num_keys = 0;
    return KEY_STORE_OK;
}

key_store_err_t KeyStore_AddKey(const uint8_t pubkey[32])
{
    if (!pubkey) {
        return KEY_STORE_ERR_STORAGE;
    }
    ESP_LOGW(TAG, "TODO(storage): RAM placeholder — key is NOT persisted across reboot");

    if (s_num_keys >= KEY_STORE_MAX_KEYS) {
        ESP_LOGW(TAG, "key store full (%d); refusing new key", KEY_STORE_MAX_KEYS);
        return KEY_STORE_ERR_FULL;
    }
    if (KeyStore_IsAuthorized(pubkey)) {
        return KEY_STORE_ERR_EXISTS;
    }
    memcpy(s_keys[s_num_keys], pubkey, 32);
    s_num_keys++;
    return KEY_STORE_OK;
}

key_store_err_t KeyStore_RemoveKey(const uint8_t pubkey[32])
{
    if (!pubkey) {
        return KEY_STORE_ERR_STORAGE;
    }
    ESP_LOGW(TAG, "TODO(storage): RAM placeholder — removal is NOT persisted across reboot");

    for (size_t i = 0; i < s_num_keys; i++) {
        if (memcmp(s_keys[i], pubkey, 32) == 0) {
            /* Shift the tail down to keep the list dense. */
            size_t tail = s_num_keys - i - 1;
            if (tail > 0) {
                memmove(&s_keys[i], &s_keys[i + 1], tail * 32);
            }
            s_num_keys--;
            return KEY_STORE_OK;
        }
    }
    return KEY_STORE_ERR_NOT_FOUND;
}

bool KeyStore_IsAuthorized(const uint8_t pubkey[32])
{
    if (!pubkey) {
        return false;
    }
    for (size_t i = 0; i < s_num_keys; i++) {
        if (memcmp(s_keys[i], pubkey, 32) == 0) {
            return true;
        }
    }
    return false;
}

bool KeyStore_GetByIndex(size_t index, uint8_t pubkey_out[32])
{
    if (!pubkey_out || index >= s_num_keys) {
        return false;
    }
    memcpy(pubkey_out, s_keys[index], 32);
    return true;
}

size_t KeyStore_Count(void)
{
    return s_num_keys;
}
