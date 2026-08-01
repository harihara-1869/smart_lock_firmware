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
 * @file session.h
 * @brief Session layer: mutual-authentication handshake and secure channel.
 *
 * Implements the lock side of the three-message mutual-authentication
 * protocol (ephemeral X25519 + Ed25519 identity + HKDF-SHA-256 key
 * derivation + AES-256-GCM payloads) and plugs into the transport layer via
 * its two callback slots — no adapter, no transport changes:
 *
 *     transport_config_t tcfg = {
 *         ...
 *         .on_apdu  = session_on_apdu,
 *         .on_erase = session_on_erase,
 *         .user_ctx = session,          // session_handle_t
 *     };
 *
 * The layer below this header is transport.h; session code never touches
 * LLI or PN532 APIs. The layer above (Application) registers one plaintext
 * command handler and one peer-key enumerator; session is a pure courier
 * between them and never interprets plaintext.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SESSION_OK = 0,
    SESSION_ERR_INVALID_ARG,    /* NULL args / bad configuration            */
    SESSION_ERR_AUTH_FAILED,    /* signature or AEAD tag mismatch           */
    SESSION_ERR_INTERNAL,       /* allocation or crypto-backend failure     */
} session_err_t;

/**
 * Application Module's plaintext command handler.
 *
 * Called once per successfully decrypted secure payload. Session does not
 * inspect the bytes in either direction. The handler must encode its own
 * application-level failure into plaintext_out (APP_STATUS_* byte per the
 * Application spec); a non-OK return is logged but does NOT tear down the
 * transport session — only crypto-level failures do.
 *
 * @param plaintext_in   Decrypted command bytes (OPCODE ‖ ARGS).
 * @param len_in         Length of plaintext_in.
 * @param plaintext_out  Destination for the reply (status ‖ ARGS).
 * @param len_out        [in] capacity (255 - 28 GCM overhead = 227);
 *                       [out] bytes written. Must stay <= capacity.
 * @param app_ctx        Opaque pointer from session_config_t.
 */
typedef session_err_t (*session_app_cmd_handler_t)(
        const uint8_t *plaintext_in, size_t len_in,
        uint8_t *plaintext_out, size_t *len_out,
        void *app_ctx);

/**
 * Enumerates candidate long-term Ed25519 public keys for M3 verification.
 *
 * M1 carries no identity hint, so the peer's identity can only be resolved
 * by trying candidates: session calls this with index = 0, 1, 2, ... and
 * verifies the M3 signature against each returned key. Return true when
 * pubkey_out was filled, false when the list is exhausted (auth failure).
 *
 * @param index       0-based candidate index.
 * @param pubkey_out  Receives one 32-byte Ed25519 public key.
 * @param provider_ctx  Opaque pointer from session_config_t.
 * @return true if pubkey_out was written; false if no more candidates.
 */
typedef bool (*session_peer_key_provider_t)(
        size_t index, uint8_t pubkey_out[32], void *provider_ctx);

/**
 * Session lifecycle event handler — called synchronously on the transport
 * task. Must be fast and non-blocking (no allocation, no blocking I/O).
 *
 * @param event_ctx  Opaque pointer from session_config_t.event_ctx.
 */
typedef void (*session_event_handler_t)(void *event_ctx);

typedef struct {
    /* Local long-term identity (this lock's Ed25519 keypair).
     * local_sk: 64 bytes (seed 32 || public 32, libsodium layout).
     * Raw bytes today; the crypto seam in src/ is the swap point for a
     * signing-callback pair when the ATECC608A migration lands. */
    uint8_t local_sk[64];
    uint8_t local_pk[32];

    session_peer_key_provider_t peer_key_provider;   /* required */
    void *peer_key_provider_ctx;

    session_app_cmd_handler_t app_handler;           /* required */
    void *app_handler_ctx;

    /* Optional lifecycle event handlers (NULL-safe). */
    session_event_handler_t on_established;  /* fires once: EPHEMERAL → ESTABLISHED */
    session_event_handler_t on_terminated;   /* fires once: only if stage was ESTABLISHED at erase */
    void *event_ctx;
} session_config_t;

typedef struct session_t *session_handle_t;

/**
 * Allocate and initialise a session instance.
 *
 * @param cfg  Configuration; peer_key_provider and app_handler must be set.
 * @param out  Receives the handle on success.
 * @return SESSION_OK, SESSION_ERR_INVALID_ARG, or SESSION_ERR_INTERNAL.
 */
session_err_t session_init(const session_config_t *cfg, session_handle_t *out);

/**
 * Destroy a session instance, zeroizing all key material first. NULL-safe.
 */
session_err_t session_deinit(session_handle_t h);

/**
 * transport_apdu_handler_t implementation — register as
 * transport_config_t.on_apdu with user_ctx = the session handle.
 *
 * Dispatches on capdu->ins: 0x10 (M1 → M2), 0x11 (M3 verify → keys),
 * 0x20 (decrypt → app_handler → encrypt). Transport has already rejected
 * INS values not valid in the current state.
 */
transport_err_t session_on_apdu(const transport_capdu_t *capdu,
                                transport_rapdu_t *rapdu, void *ctx);

/**
 * transport_erase_handler_t implementation — register as
 * transport_config_t.on_erase with user_ctx = the session handle.
 *
 * Zeroizes all ephemeral and derived key material and returns the session
 * to the empty stage. Idempotent: safe when no session ever started.
 */
void session_on_erase(void *ctx);

/**
 * Arms exactly one upcoming session to skip resolver-based M3 verification
 * and instead cache Sig_P + the M3 transcript for a later deferred check.
 *
 * @param h           The session handle.
 * @param timeout_ms  The validity window in milliseconds.
 * @return SESSION_OK or SESSION_ERR_INVALID_ARG.
 */
session_err_t session_arm_provisioning_window(session_handle_t h, uint32_t timeout_ms);

/**
 * Verifies the CACHED (unverified-at-handshake-time) Sig_P from the most
 * recently completed provisioning-mode M3 against a caller-supplied
 * candidate public key.
 *
 * @param h               The session handle.
 * @param claimed_pubkey  The 32-byte Ed25519 public key claimed by the peer.
 * @return true if the signature over the cached transcript is valid, false otherwise.
 */
bool session_provision_verify_identity(session_handle_t h, const uint8_t claimed_pubkey[32]);

#ifdef __cplusplus
}
#endif
