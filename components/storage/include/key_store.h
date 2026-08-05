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
 * @file key_store.h
 * @brief Authorized-key store — the persistence SEAM, not the implementation.
 *
 * The Application Module (and nothing below it) talks ONLY to this interface.
 * The backend behind it is selected by STORAGE_BACKEND in Kconfig and lives
 * entirely inside the storage component.
 *
 *   STORAGE_BACKEND_RAM        — provisional RAM placeholder (no persistence)
 *   STORAGE_BACKEND_NVS        — planned
 *   STORAGE_BACKEND_SECURE_ELEMENT — planned (secure element + RAM + write
 *                                    policy)
 *
 * Swapping the backend must never change a call site. KEY_STORE_ERR_STORAGE
 * exists so a future backend (e.g. a secure element that refuses a write, or
 * a write-policy denial) can express failure without an interface change.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of authorized keys (peer_key_provider is capped at 64). */
#define KEY_STORE_MAX_KEYS 64

typedef enum {
    KEY_STORE_OK = 0,
    KEY_STORE_ERR_FULL,      /* store is at capacity                       */
    KEY_STORE_ERR_EXISTS,    /* key already present                        */
    KEY_STORE_ERR_NOT_FOUND, /* key not present (remove)                   */
    KEY_STORE_ERR_STORAGE,   /* backend failure: write denied, element
                              * busy, persistence error, ...               */
} key_store_err_t;

/**
 * Initialise the key store (backend-dependent; may be a no-op for RAM).
 */
key_store_err_t KeyStore_Init(void);

/**
 * Persist a newly provisioned phone public key.
 *
 * @param pubkey  32-byte Ed25519 public key.
 * @return KEY_STORE_OK, _ERR_FULL, _ERR_EXISTS, or _ERR_STORAGE.
 */
key_store_err_t KeyStore_AddKey(const uint8_t pubkey[32]);

/**
 * Remove a phone public key (CMD_REVOKE_KEY).
 *
 * @return KEY_STORE_OK, _ERR_NOT_FOUND, or _ERR_STORAGE.
 */
key_store_err_t KeyStore_RemoveKey(const uint8_t pubkey[32]);

/**
 * @return true if @p pubkey is an authorized key.
 */
bool KeyStore_IsAuthorized(const uint8_t pubkey[32]);

/**
 * Iterator over the authorized-key list, in storage order (index 0, 1, 2, ...).
 *
 * Signature-compatible with comm_peer_key_provider_t's needs: fills
 * @p pubkey_out and returns true while candidates remain, false when
 * exhausted. The Application wires this directly into
 * comm_module_config_t.peer_key_provider.
 *
 * @param index       0-based candidate index.
 * @param pubkey_out  Receives one 32-byte Ed25519 public key.
 * @return true if @p pubkey_out was filled; false if exhausted.
 */
bool KeyStore_GetByIndex(size_t index, uint8_t pubkey_out[32]);

/**
 * @return the number of authorized keys currently stored.
 */
size_t KeyStore_Count(void);

#ifdef __cplusplus
}
#endif
