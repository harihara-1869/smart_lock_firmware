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
 *   STORAGE_BACKEND_NVS          — NVS flash (default, with power-loss persistence)
 *   STORAGE_BACKEND_SE           — Secure Element (planned, honest stub today)
 *
 * Architecture:
 *   - ALL reads are served from a RAM cache warmed at boot. No HAL/backend
 *     call on any read path.
 *   - ALL writes go to slow storage FIRST (write-through), then mirror to RAM.
 *     A failed HAL write leaves the RAM cache untouched.
 *   - Concurrency: mutation (add/revoke) happens only on the Application task.
 *     key_store_get() may execute on the comm task (via the provider trampoline
 *     during M3). Fill-slot-before-publish-count and compact-before-shrink-count
 *     make the worst race a benign one-time missed candidate — never an
 *     authorization bypass. No mutex, by design.
 *
 * Swapping the backend must never change a call site.
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
    KEY_STORE_ERR_NOT_FOUND, /* key not present (revoke)                   */
    KEY_STORE_ERR_STORAGE,   /* backend failure: write denied, element
                              * busy, persistence error, ...               */
} key_store_err_t;

/**
 * Initialise the key store: warm the RAM cache from the persistent backend.
 *
 * Reads keys sequentially from the HAL. On STORAGE_ERR_IO, fails loudly —
 * a lock that cannot read its key store must not boot with an empty cache
 * (silent owner lockout is worse than a boot failure).
 */
key_store_err_t key_store_init(void);

/**
 * Persist a newly provisioned phone public key (write-through).
 *
 * @param pk  32-byte Ed25519 public key.
 * @return KEY_STORE_OK, _ERR_FULL, _ERR_EXISTS, or _ERR_STORAGE.
 */
key_store_err_t key_store_add(const uint8_t pk[32]);

/**
 * Remove a phone public key — CMD_REVOKE_KEY (write-through).
 *
 * Erases from HAL first, then compacts the RAM array.
 *
 * @return KEY_STORE_OK, _ERR_NOT_FOUND, or _ERR_STORAGE.
 */
key_store_err_t key_store_revoke(const uint8_t pk[32]);

/**
 * Iterator over the authorized-key list, in storage order (index 0, 1, 2, ...).
 *
 * Signature-compatible with comm_peer_key_provider_t: fills
 * @p out and returns true while candidates remain, false when exhausted.
 * The Application wires this directly into
 * comm_module_config_t.peer_key_provider.
 *
 * RAM-only — no HAL call. May execute on the comm task during M3.
 *
 * @param index  0-based candidate index.
 * @param out    Receives one 32-byte Ed25519 public key.
 * @return true if @p out was filled; false if exhausted.
 */
bool key_store_get(size_t index, uint8_t out[32]);

/**
 * @return true if @p pk is an authorized key. RAM-only.
 */
bool key_store_contains(const uint8_t pk[32]);

/**
 * @return the number of authorized keys currently stored. RAM-only.
 */
size_t key_store_count(void);

/* --- Lock identity ----------------------------------------------------- */

/**
 * Load or generate the lock's long-term Ed25519 identity.
 *
 * On first boot (no persisted key): generates a fresh keypair from CSPRNG
 * and persists it to NVS. On subsequent boots: reads the persisted keypair
 * from NVS.
 *
 * Must be called once, after storage_hal_init (i.e. after key_store_init or
 * intent_log_init) and before any caller needs the identity bytes.
 * The returned pointers are valid for the firmware lifetime.
 *
 * @return KEY_STORE_OK or KEY_STORE_ERR_STORAGE.
 */
key_store_err_t key_store_identity_init(void);

/** @return pointer to the lock's 64-byte Ed25519 secret key. */
const uint8_t *key_store_identity_sk(void);

/** @return pointer to the lock's 32-byte Ed25519 public key. */
const uint8_t *key_store_identity_pk(void);

#ifdef __cplusplus
}
#endif
