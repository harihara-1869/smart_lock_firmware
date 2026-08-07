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
 * @file key_store.c
 * @brief Authorized-key store — RAM cache with write-through persistence.
 *
 * Architecture (MODULE_RESPONSIBILITIES.md §2 boundary rule 5):
 *   - ALL reads are served from the RAM cache ONLY. No HAL call on any read
 *     path — the cache is warmed at boot and kept in sync by writes.
 *   - ALL writes go to slow storage FIRST (write-through), then mirror to RAM.
 *     The HAL result drives whether the RAM slot is updated.
 *
 * Cache warming (key_store_init):
 *   - Read keys/k00, k01, k02... sequentially from the HAL.
 *   - On STORAGE_ERR_IO: FAIL LOUDLY — a lock that cannot read its key store
 *     must not boot with an empty cache (silent owner lockout is worse than
 *     a boot failure).
 *
 * Concurrency: mutation (add/revoke) happens only on the Application task.
 * key_store_get may execute on the comm task (via the provider trampoline
 * during M3). The ordering rules below (fill slot before publishing count;
 * compact before shrinking count) make the worst concurrent race a benign
 * one-time missed candidate — never an authorization bypass. No mutex.
 */

#include "key_store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "monocypher-ed25519.h"
#include "storage_hal.h"

static const char *TAG = "KEY_STORE";

#define HAL_NS   "keys"
#define KEY_MAX  64

static uint8_t s_keys[KEY_MAX][32];
static size_t  s_count = 0;

/* Forward: key_store_add calls key_store_contains before the full definition. */
bool key_store_contains(const uint8_t pk[32]);

/* Build a zero-padded 3-digit key name, e.g. "k00" .. "k63". */
static void idx_key_name(uint8_t idx, char out[4])
{
    out[0] = 'k';
    out[1] = '0' + (idx / 10);
    out[2] = '0' + (idx % 10);
    out[3] = '\0';
}

key_store_err_t key_store_init(void)
{
    /* Ensure the HAL is ready before reading. */
    storage_err_t serr = storage_hal_init();
    if (serr != STORAGE_OK) {
        ESP_LOGE(TAG, "hal init failed: %d", serr);
        return KEY_STORE_ERR_STORAGE;
    }

    s_count = 0;
    size_t len;

    for (uint8_t i = 0; i < KEY_MAX; i++) {
        char name[4];
        idx_key_name(i, name);

        len = 32;
        storage_err_t serr = storage_hal_read_blob(HAL_NS, name,
                                                   s_keys[i], &len);
        if (serr == STORAGE_ERR_NOT_FOUND) {
            break;  /* no more keys */
        }
        if (serr != STORAGE_OK) {
            ESP_LOGE(TAG, "cache warm failed at index %u: hal error %d", i, serr);
            return KEY_STORE_ERR_STORAGE;
        }
        if (len != 32) {
            ESP_LOGE(TAG, "cache warm: corrupt blob at index %u (len %u)",
                     i, (unsigned)len);
            return KEY_STORE_ERR_STORAGE;
        }
        s_count = i + 1;
    }

    ESP_LOGI(TAG, "cache warmed: %u keys", (unsigned)s_count);
    return KEY_STORE_OK;
}

key_store_err_t key_store_add(const uint8_t pk[32])
{
    if (!pk) {
        return KEY_STORE_ERR_STORAGE;
    }
    if (s_count >= KEY_MAX) {
        return KEY_STORE_ERR_FULL;
    }
    if (key_store_contains(pk)) {
        return KEY_STORE_ERR_EXISTS;
    }

    /* Write-through: HAL FIRST. On failure, leave RAM untouched. */
    char name[4];
    idx_key_name((uint8_t)s_count, name);
    storage_err_t serr = storage_hal_write_blob(HAL_NS, name, pk, 32);
    if (serr == STORAGE_ERR_FULL) {
        return KEY_STORE_ERR_FULL;
    }
    if (serr != STORAGE_OK) {
        ESP_LOGE(TAG, "add: hal write failed: %d", serr);
        return KEY_STORE_ERR_STORAGE;
    }

    /* Fill the slot before publishing the count (concurrency rule). */
    memcpy(s_keys[s_count], pk, 32);
    s_count++;
    return KEY_STORE_OK;
}

key_store_err_t key_store_revoke(const uint8_t pk[32])
{
    if (!pk) {
        return KEY_STORE_ERR_STORAGE;
    }

    size_t idx;
    for (idx = 0; idx < s_count; idx++) {
        if (memcmp(s_keys[idx], pk, 32) == 0) {
            break;
        }
    }
    if (idx >= s_count) {
        return KEY_STORE_ERR_NOT_FOUND;
    }

    /* Erase the HAL entry for the last slot AND the removed slot if different.
     * The compacted keys are re-saved so the HAL always matches the new RAM
     * layout. */
    storage_err_t serr;

    /* Erase the key being removed. */
    char name[4];
    idx_key_name((uint8_t)idx, name);
    serr = storage_hal_erase_key(HAL_NS, name);
    if (serr != STORAGE_OK) {
        ESP_LOGE(TAG, "revoke: hal erase failed at %u: %d", (unsigned)idx, serr);
        return KEY_STORE_ERR_STORAGE;
    }

    /* Compact the array. */
    size_t tail = s_count - idx - 1;
    if (tail > 0) {
        /* Re-write the shifted keys to their new indices. */
        for (size_t i = idx; i < s_count - 1; i++) {
            char new_name[4];
            idx_key_name((uint8_t)i, new_name);
            serr = storage_hal_write_blob(HAL_NS, new_name, s_keys[i + 1], 32);
            if (serr != STORAGE_OK) {
                ESP_LOGE(TAG, "revoke: hal re-write failed at %u: %d",
                         (unsigned)i, serr);
                return KEY_STORE_ERR_STORAGE;
            }
        }
        /* Erase the old last slot (now duplicated). */
        char last_name[4];
        idx_key_name((uint8_t)(s_count - 1), last_name);
        storage_hal_erase_key(HAL_NS, last_name);

        memmove(&s_keys[idx], &s_keys[idx + 1], tail * 32);
    }

    /* Compact before shrinking count (concurrency rule). */
    s_count--;
    return KEY_STORE_OK;
}

bool key_store_get(size_t index, uint8_t out[32])
{
    if (!out || index >= s_count) {
        return false;
    }
    memcpy(out, s_keys[index], 32);
    return true;
}

bool key_store_contains(const uint8_t pk[32])
{
    if (!pk) {
        return false;
    }
    for (size_t i = 0; i < s_count; i++) {
        if (memcmp(s_keys[i], pk, 32) == 0) {
            return true;
        }
    }
    return false;
}

size_t key_store_count(void)
{
    return s_count;
}

/* ------------------------------------------------------------------ */
/* Lock identity (first-boot keygen + NVS persistence)                 */
/* ------------------------------------------------------------------ */

#define IDENTITY_NS   "identity"
#define IDENTITY_KEY  "lock"
#define IDENTITY_BLOB_LEN 96   /* 64-byte SK || 32-byte PK */

static uint8_t s_identity_sk[64];
static uint8_t s_identity_pk[32];
static bool    s_identity_loaded = false;

static void generate_fresh_keypair(uint8_t sk[64], uint8_t pk[32])
{
    uint8_t seed[32];
    esp_fill_random(seed, sizeof(seed));
    crypto_ed25519_key_pair(sk, pk, seed);
    /* seed is wiped by crypto_ed25519_key_pair internally (crypto_wipe). */
}

key_store_err_t key_store_identity_init(void)
{
    size_t len = IDENTITY_BLOB_LEN;
    uint8_t blob[IDENTITY_BLOB_LEN];

    storage_err_t serr = storage_hal_read_blob(IDENTITY_NS, IDENTITY_KEY,
                                               blob, &len);
    if (serr == STORAGE_OK && len == IDENTITY_BLOB_LEN) {
        memcpy(s_identity_sk, blob, 64);
        memcpy(s_identity_pk, blob + 64, 32);
        s_identity_loaded = true;
        ESP_LOGI(TAG, "identity loaded from NVS");
        return KEY_STORE_OK;
    }

    /* Blob found but wrong size → corrupt NVS. Treat as a fatal error —
     * generating a new keypair would orphan the old one (provisioned phones
     * would permanently lose access). */
    if (serr == STORAGE_OK) {
        ESP_LOGE(TAG, "persisted identity blob wrong size (%u != %u);"
                      " NVS may be corrupt — refusing to overwrite",
                 (unsigned)len, (unsigned)IDENTITY_BLOB_LEN);
        return KEY_STORE_ERR_STORAGE;
    }

    /* Identity not found. Check the sentinel to distinguish true first boot
     * from a prior failure that left no persisted identity. */
    if (serr == STORAGE_ERR_NOT_FOUND) {
        bool is_first_boot = false;
        size_t sb_len = 1;
        uint8_t sentinel = 0;
        storage_err_t sb_err = storage_hal_read_blob(
                IDENTITY_NS, "booted", &sentinel, &sb_len);
        if (sb_err == STORAGE_ERR_NOT_FOUND) {
            is_first_boot = true;
        }

        if (!is_first_boot) {
            ESP_LOGW(TAG, "device has booted before but identity is missing — "
                          "NVS partition may have been erased. "
                          "Proceeding as first boot.");
        } else {
            ESP_LOGI(TAG, "first boot detected — generating identity");
        }

        generate_fresh_keypair(s_identity_sk, s_identity_pk);

        memcpy(blob, s_identity_sk, 64);
        memcpy(blob + 64, s_identity_pk, 32);
        serr = storage_hal_write_blob(IDENTITY_NS, IDENTITY_KEY,
                                       blob, IDENTITY_BLOB_LEN);
        if (serr != STORAGE_OK) {
            ESP_LOGE(TAG, "failed to persist generated identity: %d", serr);
            memset(s_identity_sk, 0, sizeof(s_identity_sk));
            memset(s_identity_pk, 0, sizeof(s_identity_pk));
            return KEY_STORE_ERR_STORAGE;
        }
        s_identity_loaded = true;
        ESP_LOGI(TAG, "fresh identity generated and persisted");

        /* Write a sentinel so the next boot can tell this was NOT the first
         * boot — a missing identity on subsequent boots means corruption. */
        if (is_first_boot) {
            sentinel = 1;
            storage_hal_write_blob(IDENTITY_NS, "booted", &sentinel, 1);

            /* Fresh identity = new lock. Any stale provisioned phone keys
             * from a previous NVS life belong to a different identity and
             * are now useless — wipe them so the key store starts clean. */
            ESP_LOGI(TAG, "first boot — clearing all provisioned phone keys");
            for (uint8_t i = 0; i < KEY_MAX; i++) {
                char name[4];
                idx_key_name(i, name);
                storage_hal_erase_key(HAL_NS, name);
            }
            memset(s_keys, 0, sizeof(s_keys));
            s_count = 0;
        }
        return KEY_STORE_OK;
    }

    /* STORAGE_ERR_IO or STORAGE_ERR_FULL — NVS is unreachable or dead. */
    ESP_LOGE(TAG, "identity init: NVS backend failure (err %d) — "
                  "cannot load or generate identity", serr);
    return KEY_STORE_ERR_STORAGE;
}

const uint8_t *key_store_identity_sk(void)
{
    return s_identity_sk;
}

const uint8_t *key_store_identity_pk(void)
{
    return s_identity_pk;
}
