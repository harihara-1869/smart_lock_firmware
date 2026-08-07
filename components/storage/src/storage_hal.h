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
 * @file storage_hal.h
 * @brief PRIVATE slow-storage backend interface.
 *
 * This is an internal seam — never exposed to any component outside storage.
 * key_store.c and intent_log.c call these functions; the backend (NVS today,
 * Secure Element later) is selected by Kconfig.
 *
 * No nvs.h or other HAL-specific types appear in the public headers.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STORAGE_OK = 0,
    STORAGE_ERR_IO,
    STORAGE_ERR_FULL,
    STORAGE_ERR_NOT_FOUND,
} storage_err_t;

/**
 * One-time backend initialisation (NVS: nvs_flash_init + open namespaces;
 * SE: no-op). Must succeed before any read or write.
 */
storage_err_t storage_hal_init(void);

/**
 * Read a blob by namespace + key. On success @p *len is updated to the blob
 * size; on NOT_FOUND @p *len is unchanged and the output is unmodified.
 */
storage_err_t storage_hal_read_blob(const char *ns, const char *key,
                                    void *out, size_t *len);

/**
 * Write a blob (create or overwrite). On FULL the backend refuses; the
 * caller must not write the corresponding RAM slot.
 */
storage_err_t storage_hal_write_blob(const char *ns, const char *key,
                                     const void *in, size_t len);

/**
 * Erase a key (idempotent — OK if it doesn't exist).
 */
storage_err_t storage_hal_erase_key(const char *ns, const char *key);

#ifdef __cplusplus
}
#endif
