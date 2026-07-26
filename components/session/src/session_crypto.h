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
 * @file session_crypto.h
 * @brief Private crypto seam for the Session layer.
 *
 * Every cryptographic primitive the session layer needs, behind one small
 * interface. session.c calls only these functions; it never touches
 * Monocypher, mbedTLS, or esp_random directly. This is also the swap point
 * for a future ATECC608A secure-element backend (signing moves off-chip).
 *
 * All buffers are fixed-size at the call site; lengths are explicit.
 * Return convention: 0 on success, -1 on failure (never returns partial
 * output; output buffers are wiped on failure where they held secrets).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SESSION_CRYPTO_ED25519_SK_LEN   64
#define SESSION_CRYPTO_ED25519_PK_LEN   32
#define SESSION_CRYPTO_ED25519_SIG_LEN  64
#define SESSION_CRYPTO_X25519_LEN       32
#define SESSION_CRYPTO_GCM_KEY_LEN      32
#define SESSION_CRYPTO_GCM_NONCE_LEN    12
#define SESSION_CRYPTO_GCM_TAG_LEN      16

/** Fill buf with cryptographically secure random bytes. */
void session_crypto_random(uint8_t *buf, size_t len);

/** Zeroize len bytes at buf in a way the compiler cannot elide. */
void session_crypto_zeroize(void *buf, size_t len);

/** True if all len bytes at buf are zero (constant-time). */
bool session_crypto_is_zero(const uint8_t *buf, size_t len);

/**
 * Generate an ephemeral X25519 keypair.
 * pk_out[32] receives the public key, sk_out[32] the private key.
 */
int session_crypto_x25519_keypair(uint8_t pk_out[SESSION_CRYPTO_X25519_LEN],
                                  uint8_t sk_out[SESSION_CRYPTO_X25519_LEN]);

/**
 * X25519 shared secret: out = scalarmult(sk, peer_pk).
 * Fails (-1) if the resulting secret is the all-zero (low-order) value;
 * callers must treat that as an authentication failure, not retry.
 */
int session_crypto_x25519_shared(
        const uint8_t sk[SESSION_CRYPTO_X25519_LEN],
        const uint8_t peer_pk[SESSION_CRYPTO_X25519_LEN],
        uint8_t out[SESSION_CRYPTO_X25519_LEN]);

/**
 * RFC 8032 Ed25519 detached signature over message with a 64-byte secret key
 * (seed 32 || public 32, libsodium layout).
 */
int session_crypto_ed25519_sign(
        const uint8_t sk[SESSION_CRYPTO_ED25519_SK_LEN],
        const uint8_t *message, size_t message_len,
        uint8_t sig_out[SESSION_CRYPTO_ED25519_SIG_LEN]);

/**
 * RFC 8032 Ed25519 detached verification. Returns 0 on valid signature,
 * -1 otherwise (bad signature, bad key, or bad encoding — indistinguishable
 * by design).
 */
int session_crypto_ed25519_verify(
        const uint8_t pk[SESSION_CRYPTO_ED25519_PK_LEN],
        const uint8_t *message, size_t message_len,
        const uint8_t sig[SESSION_CRYPTO_ED25519_SIG_LEN]);

/**
 * RFC 5869 HKDF-SHA-256, extract + expand in one call.
 * out_len must be <= 255 * 32 (8160); the session layer only ever asks for 32.
 */
int session_crypto_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                               const uint8_t *ikm, size_t ikm_len,
                               const uint8_t *info, size_t info_len,
                               uint8_t *out, size_t out_len);

/**
 * AES-256-GCM seal: ct_out receives plaintext_len ciphertext bytes,
 * tag_out receives the 16-byte tag. No AAD. ct_out may not alias plaintext.
 */
int session_crypto_aes256gcm_seal(
        const uint8_t key[SESSION_CRYPTO_GCM_KEY_LEN],
        const uint8_t nonce[SESSION_CRYPTO_GCM_NONCE_LEN],
        const uint8_t *plaintext, size_t plaintext_len,
        uint8_t *ct_out,
        uint8_t tag_out[SESSION_CRYPTO_GCM_TAG_LEN]);

/**
 * AES-256-GCM open: verifies tag, decrypts ct_len bytes into pt_out.
 * Returns -1 on tag mismatch (authentication failure) without writing
 * plaintext.
 */
int session_crypto_aes256gcm_open(
        const uint8_t key[SESSION_CRYPTO_GCM_KEY_LEN],
        const uint8_t nonce[SESSION_CRYPTO_GCM_NONCE_LEN],
        const uint8_t *ct, size_t ct_len,
        const uint8_t tag[SESSION_CRYPTO_GCM_TAG_LEN],
        uint8_t *pt_out);

#ifdef __cplusplus
}
#endif
