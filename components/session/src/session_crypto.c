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

#include "session_crypto.h"

#include <string.h>

#include "esp_random.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

#include "monocypher.h"
#include "monocypher-ed25519.h"

void session_crypto_random(uint8_t *buf, size_t len)
{
    esp_fill_random(buf, len);
}

void session_crypto_zeroize(void *buf, size_t len)
{
    mbedtls_platform_zeroize(buf, len);
}

bool session_crypto_is_zero(const uint8_t *buf, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; i++) {
        acc |= buf[i];
    }
    return acc == 0;
}

int session_crypto_x25519_keypair(uint8_t pk_out[SESSION_CRYPTO_X25519_LEN],
                                  uint8_t sk_out[SESSION_CRYPTO_X25519_LEN])
{
    if (!pk_out || !sk_out) {
        return -1;
    }
    session_crypto_random(sk_out, SESSION_CRYPTO_X25519_LEN);
    crypto_x25519_public_key(pk_out, sk_out);
    return 0;
}

int session_crypto_x25519_shared(
        const uint8_t sk[SESSION_CRYPTO_X25519_LEN],
        const uint8_t peer_pk[SESSION_CRYPTO_X25519_LEN],
        uint8_t out[SESSION_CRYPTO_X25519_LEN])
{
    if (!sk || !peer_pk || !out) {
        return -1;
    }
    crypto_x25519(out, sk, peer_pk);
    /* RFC 7748 section 6.1: an all-zero shared secret means the peer sent a
     * low-order point. Authentication MUST fail; never use this value. */
    if (session_crypto_is_zero(out, SESSION_CRYPTO_X25519_LEN)) {
        session_crypto_zeroize(out, SESSION_CRYPTO_X25519_LEN);
        return -1;
    }
    return 0;
}

int session_crypto_ed25519_sign(
        const uint8_t sk[SESSION_CRYPTO_ED25519_SK_LEN],
        const uint8_t *message, size_t message_len,
        uint8_t sig_out[SESSION_CRYPTO_ED25519_SIG_LEN])
{
    if (!sk || (!message && message_len > 0) || !sig_out) {
        return -1;
    }
    crypto_ed25519_sign(sig_out, sk, message, message_len);
    return 0;
}

int session_crypto_ed25519_verify(
        const uint8_t pk[SESSION_CRYPTO_ED25519_PK_LEN],
        const uint8_t *message, size_t message_len,
        const uint8_t sig[SESSION_CRYPTO_ED25519_SIG_LEN])
{
    if (!pk || (!message && message_len > 0) || !sig) {
        return -1;
    }
    return crypto_ed25519_check(sig, pk, message, message_len) == 0 ? 0 : -1;
}

/* HMAC-SHA-256 helper: out is 32 bytes. */
static int hmac_sha256(const uint8_t *key, size_t key_len,
                       const uint8_t *data, size_t data_len,
                       uint8_t out[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) {
        return -1;
    }
    return mbedtls_md_hmac(info, key, key_len, data, data_len, out) == 0 ? 0 : -1;
}

int session_crypto_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                               const uint8_t *ikm, size_t ikm_len,
                               const uint8_t *info, size_t info_len,
                               uint8_t *out, size_t out_len)
{
    if ((!salt && salt_len > 0) || (!ikm && ikm_len > 0) ||
        (!info && info_len > 0) || !out || out_len == 0 || out_len > 255 * 32) {
        return -1;
    }

    /* RFC 5869: absent salt = HashLen zeros. */
    static const uint8_t zero_salt[32] = {0};
    if (!salt) {
        salt = zero_salt;
        salt_len = sizeof(zero_salt);
    }

    /* Extract: PRK = HMAC-Hash(salt, IKM) */
    uint8_t prk[32];
    if (hmac_sha256(salt, salt_len, ikm, ikm_len, prk) != 0) {
        session_crypto_zeroize(prk, sizeof(prk));
        return -1;
    }

    /* Expand: T(0) empty; T(i) = HMAC(PRK, T(i-1) | info | i); OKM = T(1).. */
    uint8_t t[32];
    size_t t_len = 0;                    /* T(0) is the empty string */
    size_t produced = 0;
    uint8_t counter = 0;

    while (produced < out_len) {
        counter++;
        /* HMAC input = T(i-1) | info | counter(i), built contiguously. */
        uint8_t buf[32 + 256 + 1];       /* max T + max info + counter */
        if (info_len > 256) {
            session_crypto_zeroize(prk, sizeof(prk));
            return -1;
        }
        memcpy(buf, t, t_len);
        memcpy(buf + t_len, info, info_len);
        buf[t_len + info_len] = counter;

        if (hmac_sha256(prk, sizeof(prk), buf, t_len + info_len + 1, t) != 0) {
            session_crypto_zeroize(prk, sizeof(prk));
            session_crypto_zeroize(t, sizeof(t));
            return -1;
        }
        t_len = sizeof(t);

        const size_t take = (out_len - produced < t_len) ? out_len - produced
                                                         : t_len;
        memcpy(out + produced, t, take);
        produced += take;
    }

    session_crypto_zeroize(prk, sizeof(prk));
    session_crypto_zeroize(t, sizeof(t));
    return 0;
}

int session_crypto_aes256gcm_seal(
        const uint8_t key[SESSION_CRYPTO_GCM_KEY_LEN],
        const uint8_t nonce[SESSION_CRYPTO_GCM_NONCE_LEN],
        const uint8_t *plaintext, size_t plaintext_len,
        uint8_t *ct_out,
        uint8_t tag_out[SESSION_CRYPTO_GCM_TAG_LEN])
{
    if (!key || !nonce || (!plaintext && plaintext_len > 0) ||
        !ct_out || !tag_out) {
        return -1;
    }

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT,
                                       plaintext_len,
                                       nonce, SESSION_CRYPTO_GCM_NONCE_LEN,
                                       NULL, 0,
                                       plaintext, ct_out,
                                       SESSION_CRYPTO_GCM_TAG_LEN, tag_out);
    }
    mbedtls_gcm_free(&ctx);
    return rc == 0 ? 0 : -1;
}

int session_crypto_aes256gcm_open(
        const uint8_t key[SESSION_CRYPTO_GCM_KEY_LEN],
        const uint8_t nonce[SESSION_CRYPTO_GCM_NONCE_LEN],
        const uint8_t *ct, size_t ct_len,
        const uint8_t tag[SESSION_CRYPTO_GCM_TAG_LEN],
        uint8_t *pt_out)
{
    if (!key || !nonce || (!ct && ct_len > 0) || !tag || !pt_out) {
        return -1;
    }

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(&ctx, ct_len,
                                      nonce, SESSION_CRYPTO_GCM_NONCE_LEN,
                                      NULL, 0,
                                      tag, SESSION_CRYPTO_GCM_TAG_LEN,
                                      ct, pt_out);
    }
    mbedtls_gcm_free(&ctx);
    /* Auth failure (MBEDTLS_ERR_GCM_AUTH_FAILED) and any other error are
     * indistinguishable to the caller: -1, no plaintext written. */
    return rc == 0 ? 0 : -1;
}
