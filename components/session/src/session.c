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

#include "session.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "session_crypto.h"

static const char *TAG = "SESSION";

/* INS codes the transport layer forwards to us (transport.c owns the
 * state-machine validation; these mirror its internal definitions). */
#define SESSION_INS_HANDSHAKE_INIT    0x10
#define SESSION_INS_HANDSHAKE_FINISH  0x11
#define SESSION_INS_SECURE_PAYLOAD    0x20

/* Status words written into the R-APDU. */
#define SESSION_SW1_OK            0x90
#define SESSION_SW2_OK            0x00
#define SESSION_SW1_AUTH_FAILED   0x69
#define SESSION_SW2_AUTH_FAILED   0x82

/* Protocol constants. */
#define SESSION_PROTOCOL_VERSION  0x01
#define SESSION_DOMAIN            "SLOCK-HS-v1"
#define SESSION_DOMAIN_LEN        11
#define SESSION_TRANSCRIPT_LEN    (SESSION_DOMAIN_LEN + 1 + 32 + 32 + 32 + 32)

#define SESSION_M1_LEN   64    /* pk_eph_P(32) ‖ c_P(32)                 */
#define SESSION_M2_LEN   128   /* pk_eph_L(32) ‖ c_L(32) ‖ Sig_L(64)     */
#define SESSION_M3_LEN   64    /* Sig_P(64)                              */

#define SESSION_GCM_OVERHEAD  (SESSION_CRYPTO_GCM_NONCE_LEN + \
                               SESSION_CRYPTO_GCM_TAG_LEN)      /* 28     */
#define SESSION_PLAINTEXT_MAX (255 - SESSION_GCM_OVERHEAD)      /* 227    */

#define SESSION_HKDF_SALT_LEN  64   /* c_P(32) ‖ c_L(32) */

typedef enum {
    SESSION_STAGE_EMPTY = 0,   /* no key material (post-init / post-erase) */
    SESSION_STAGE_EPHEMERAL,   /* M1 answered, awaiting M3                 */
    SESSION_STAGE_ESTABLISHED, /* M3 verified, directional keys live       */
} session_stage_t;

struct session_t {
    session_config_t cfg;
    session_stage_t  stage;

    /* Handshake cache (present in EPHEMERAL and ESTABLISHED stages). */
    uint8_t pk_eph_P[SESSION_CRYPTO_X25519_LEN];
    uint8_t c_P[32];
    uint8_t pk_eph_L[SESSION_CRYPTO_X25519_LEN];
    uint8_t sk_eph_L[SESSION_CRYPTO_X25519_LEN];
    uint8_t c_L[32];

    /* Derived session keys (present in ESTABLISHED stage only). */
    uint8_t k_p2e[SESSION_CRYPTO_GCM_KEY_LEN];
    uint8_t k_e2p[SESSION_CRYPTO_GCM_KEY_LEN];
};

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/**
 * Build the domain-separated handshake transcript:
 *   "SLOCK-HS-v1" ‖ version(0x01) ‖ pk_eph_P ‖ pk_eph_L ‖ c_P ‖ c_L
 * Returns SESSION_TRANSCRIPT_LEN.
 */
static size_t build_transcript(const session_handle_t h,
                               uint8_t out[SESSION_TRANSCRIPT_LEN])
{
    size_t off = 0;
    memcpy(out + off, SESSION_DOMAIN, SESSION_DOMAIN_LEN);
    off += SESSION_DOMAIN_LEN;
    out[off++] = SESSION_PROTOCOL_VERSION;
    memcpy(out + off, h->pk_eph_P, sizeof(h->pk_eph_P));
    off += sizeof(h->pk_eph_P);
    memcpy(out + off, h->pk_eph_L, sizeof(h->pk_eph_L));
    off += sizeof(h->pk_eph_L);
    memcpy(out + off, h->c_P, sizeof(h->c_P));
    off += sizeof(h->c_P);
    memcpy(out + off, h->c_L, sizeof(h->c_L));
    off += sizeof(h->c_L);
    return off;
}

/* ------------------------------------------------------------------ */
/* Message handlers                                                    */
/* ------------------------------------------------------------------ */

static transport_err_t session_handle_m1(session_handle_t h,
                                         const transport_capdu_t *capdu,
                                         transport_rapdu_t *rapdu)
{
    if (capdu->lc != SESSION_M1_LEN) {
        return TRANSPORT_ERR_INVALID_APDU;
    }

    const uint8_t *pk_eph_P = capdu->data;
    const uint8_t *c_P      = capdu->data + 32;

    /* X25519 contributory-behavior check (session spec §4.4 rule 3):
     * reject the all-zero public point before deriving anything. */
    if (session_crypto_is_zero(pk_eph_P, SESSION_CRYPTO_X25519_LEN)) {
        ESP_LOGW(TAG, "M1: all-zero peer ephemeral key rejected");
        return TRANSPORT_ERR_INTERNAL;
    }

    /* A fresh M1 always starts from a clean slate: wipe any material a
     * previous (aborted) session may have left behind. */
    session_on_erase(h);

    if (session_crypto_x25519_keypair(h->pk_eph_L, h->sk_eph_L) != 0) {
        return TRANSPORT_ERR_INTERNAL;
    }
    session_crypto_random(h->c_L, sizeof(h->c_L));
    memcpy(h->pk_eph_P, pk_eph_P, sizeof(h->pk_eph_P));
    memcpy(h->c_P, c_P, sizeof(h->c_P));

    uint8_t transcript[SESSION_TRANSCRIPT_LEN];
    const size_t tlen = build_transcript(h, transcript);

    uint8_t sig_L[SESSION_CRYPTO_ED25519_SIG_LEN];
    const int sign_rc = session_crypto_ed25519_sign(h->cfg.local_sk,
                                                    transcript, tlen, sig_L);
    session_crypto_zeroize(transcript, sizeof(transcript));
    if (sign_rc != 0) {
        session_on_erase(h);
        return TRANSPORT_ERR_INTERNAL;
    }

    /* M2 = pk_eph_L ‖ c_L ‖ Sig_L. */
    memcpy(rapdu->data, h->pk_eph_L, sizeof(h->pk_eph_L));
    memcpy(rapdu->data + 32, h->c_L, sizeof(h->c_L));
    memcpy(rapdu->data + 64, sig_L, sizeof(sig_L));
    rapdu->len = SESSION_M2_LEN;
    rapdu->sw1 = SESSION_SW1_OK;
    rapdu->sw2 = SESSION_SW2_OK;

    h->stage = SESSION_STAGE_EPHEMERAL;
    return TRANSPORT_OK;
}

static transport_err_t session_handle_m3(session_handle_t h,
                                         const transport_capdu_t *capdu,
                                         transport_rapdu_t *rapdu)
{
    if (h->stage != SESSION_STAGE_EPHEMERAL) {
        /* Defensive: transport's state machine already prevents this. */
        return TRANSPORT_ERR_INVALID_STATE;
    }
    if (capdu->lc != SESSION_M3_LEN) {
        return TRANSPORT_ERR_INVALID_APDU;
    }

    const uint8_t *sig_P = capdu->data;

    uint8_t transcript[SESSION_TRANSCRIPT_LEN];
    const size_t tlen = build_transcript(h, transcript);

    /* M1 carries no identity hint: enumerate candidate long-term keys and
     * accept the first one that verifies Sig_P against the transcript. */
    uint8_t candidate[SESSION_CRYPTO_ED25519_PK_LEN];
    bool authenticated = false;
    for (size_t i = 0; ; i++) {
        if (!h->cfg.peer_key_provider(i, candidate,
                                      h->cfg.peer_key_provider_ctx)) {
            break;
        }
        if (session_crypto_ed25519_verify(candidate, transcript, tlen,
                                          sig_P) == 0) {
            authenticated = true;
            break;
        }
    }
    session_crypto_zeroize(candidate, sizeof(candidate));
    session_crypto_zeroize(transcript, sizeof(transcript));

    if (!authenticated) {
        ESP_LOGW(TAG, "M3: no candidate key verified the signature");
        rapdu->len = 0;
        rapdu->sw1 = SESSION_SW1_AUTH_FAILED;
        rapdu->sw2 = SESSION_SW2_AUTH_FAILED;
        return TRANSPORT_ERR_INTERNAL;
    }

    uint8_t shared[SESSION_CRYPTO_X25519_LEN];
    if (session_crypto_x25519_shared(h->sk_eph_L, h->pk_eph_P, shared) != 0) {
        ESP_LOGW(TAG, "M3: low-order shared secret rejected");
        rapdu->len = 0;
        rapdu->sw1 = SESSION_SW1_AUTH_FAILED;
        rapdu->sw2 = SESSION_SW2_AUTH_FAILED;
        return TRANSPORT_ERR_INTERNAL;
    }

    /* PRK = HKDF-Extract(salt = c_P ‖ c_L, ikm = SharedSecret);
     * K_p2e = Expand(PRK, "phone->esp"); K_e2p = Expand(PRK, "esp->phone"). */
    uint8_t salt[SESSION_HKDF_SALT_LEN];
    memcpy(salt, h->c_P, sizeof(h->c_P));
    memcpy(salt + 32, h->c_L, sizeof(h->c_L));

    static const char INFO_P2E[] = "phone->esp";
    static const char INFO_E2P[] = "esp->phone";
    const int kdf_rc =
        session_crypto_hkdf_sha256(salt, sizeof(salt),
                                   shared, sizeof(shared),
                                   (const uint8_t *)INFO_P2E,
                                   sizeof(INFO_P2E) - 1,
                                   h->k_p2e, sizeof(h->k_p2e)) == 0 &&
        session_crypto_hkdf_sha256(salt, sizeof(salt),
                                   shared, sizeof(shared),
                                   (const uint8_t *)INFO_E2P,
                                   sizeof(INFO_E2P) - 1,
                                   h->k_e2p, sizeof(h->k_e2p)) == 0
        ? 0 : -1;

    session_crypto_zeroize(shared, sizeof(shared));
    session_crypto_zeroize(salt, sizeof(salt));

    if (kdf_rc != 0) {
        session_on_erase(h);
        return TRANSPORT_ERR_INTERNAL;
    }

    rapdu->len = 0;
    rapdu->sw1 = SESSION_SW1_OK;
    rapdu->sw2 = SESSION_SW2_OK;

    h->stage = SESSION_STAGE_ESTABLISHED;
    return TRANSPORT_OK;
}

static transport_err_t session_handle_secure_payload(
        session_handle_t h,
        const transport_capdu_t *capdu,
        transport_rapdu_t *rapdu)
{
    if (h->stage != SESSION_STAGE_ESTABLISHED) {
        /* Defensive: transport only forwards INS=0x20 in SECURE_SESSION. */
        return TRANSPORT_ERR_INVALID_STATE;
    }
    if (capdu->lc < SESSION_GCM_OVERHEAD) {
        return TRANSPORT_ERR_INVALID_APDU;
    }

    /* Wire format: nonce(12) ‖ ciphertext ‖ tag(16). */
    const uint8_t *nonce = capdu->data;
    const uint8_t *ct    = capdu->data + SESSION_CRYPTO_GCM_NONCE_LEN;
    const size_t   ct_len = capdu->lc - SESSION_GCM_OVERHEAD;
    const uint8_t *tag   = capdu->data + capdu->lc - SESSION_CRYPTO_GCM_TAG_LEN;

    uint8_t plaintext[SESSION_PLAINTEXT_MAX];
    if (session_crypto_aes256gcm_open(h->k_p2e, nonce, ct, ct_len,
                                      tag, plaintext) != 0) {
        /* Tag mismatch MUST be treated identically to a signature failure:
         * transport maps this to 0x69 0x82, erases, and releases. */
        ESP_LOGW(TAG, "secure payload: GCM tag mismatch");
        rapdu->len = 0;
        rapdu->sw1 = SESSION_SW1_AUTH_FAILED;
        rapdu->sw2 = SESSION_SW2_AUTH_FAILED;
        return TRANSPORT_ERR_INTERNAL;
    }

    /* Session is a pure courier: plaintext goes to the Application handler
     * uninterpreted. A handler error does NOT tear down the session — the
     * Application encodes its own failure status into the reply; we answer
     * with an (empty) encrypted reply and 0x90 0x00. */
    uint8_t reply[SESSION_PLAINTEXT_MAX];
    size_t reply_len = sizeof(reply);
    const session_err_t app_err =
        h->cfg.app_handler(plaintext, ct_len, reply, &reply_len,
                           h->cfg.app_handler_ctx);
    session_crypto_zeroize(plaintext, sizeof(plaintext));
    if (app_err != SESSION_OK) {
        ESP_LOGW(TAG, "app_handler returned %d — replying empty, session kept",
                 app_err);
        reply_len = 0;
    }
    if (reply_len > sizeof(reply)) {
        ESP_LOGE(TAG, "app_handler overran reply buffer — clamping");
        reply_len = sizeof(reply);
    }

    uint8_t out_nonce[SESSION_CRYPTO_GCM_NONCE_LEN];
    session_crypto_random(out_nonce, sizeof(out_nonce));

    /* Reply wire format: nonce(12) ‖ ciphertext ‖ tag(16). */
    memcpy(rapdu->data, out_nonce, sizeof(out_nonce));
    const int seal_rc = session_crypto_aes256gcm_seal(
            h->k_e2p, out_nonce, reply, reply_len,
            rapdu->data + SESSION_CRYPTO_GCM_NONCE_LEN,
            rapdu->data + SESSION_CRYPTO_GCM_NONCE_LEN + reply_len);
    session_crypto_zeroize(reply, sizeof(reply));
    if (seal_rc != 0) {
        return TRANSPORT_ERR_INTERNAL;
    }

    rapdu->len = (uint8_t)(SESSION_GCM_OVERHEAD + reply_len);
    rapdu->sw1 = SESSION_SW1_OK;
    rapdu->sw2 = SESSION_SW2_OK;
    return TRANSPORT_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

session_err_t session_init(const session_config_t *cfg, session_handle_t *out)
{
    if (!cfg || !out) {
        return SESSION_ERR_INVALID_ARG;
    }
    if (!cfg->peer_key_provider || !cfg->app_handler) {
        return SESSION_ERR_INVALID_ARG;
    }

    struct session_t *h = calloc(1, sizeof(*h));
    if (!h) {
        return SESSION_ERR_INTERNAL;
    }

    h->cfg   = *cfg;
    h->stage = SESSION_STAGE_EMPTY;
    *out = h;
    return SESSION_OK;
}

session_err_t session_deinit(session_handle_t h)
{
    if (!h) {
        return SESSION_OK;
    }
    /* Zeroizes the whole struct, including the cfg copy (it holds local_sk)
     * and all ephemeral/derived material. */
    session_crypto_zeroize(h, sizeof(*h));
    free(h);
    return SESSION_OK;
}

transport_err_t session_on_apdu(const transport_capdu_t *capdu,
                                transport_rapdu_t *rapdu, void *ctx)
{
    session_handle_t h = (session_handle_t)ctx;
    if (!h || !capdu || !rapdu) {
        return TRANSPORT_ERR_INVALID_APDU;
    }

    /* Transport has already rejected INS values not valid in the current
     * state; the default branch is defensive and should never fire. */
    switch (capdu->ins) {
    case SESSION_INS_HANDSHAKE_INIT:
        return session_handle_m1(h, capdu, rapdu);
    case SESSION_INS_HANDSHAKE_FINISH:
        return session_handle_m3(h, capdu, rapdu);
    case SESSION_INS_SECURE_PAYLOAD:
        return session_handle_secure_payload(h, capdu, rapdu);
    default:
        ESP_LOGW(TAG, "unexpected INS 0x%02X reached session", capdu->ins);
        return TRANSPORT_ERR_INVALID_APDU;
    }
}

void session_on_erase(void *ctx)
{
    session_handle_t h = (session_handle_t)ctx;
    if (!h) {
        return;
    }

    /* Idempotent: wiping already-zeroed memory is harmless, and always
     * wiping (rather than gating on stage) guarantees no key material can
     * leak through a half-completed handshake path.
     *
     * The long-term identity in cfg survives; everything session-scoped
     * (ephemeral keys, challenges, transcript cache, derived keys) dies. */
    session_crypto_zeroize(h->pk_eph_P, sizeof(h->pk_eph_P));
    session_crypto_zeroize(h->c_P,      sizeof(h->c_P));
    session_crypto_zeroize(h->pk_eph_L, sizeof(h->pk_eph_L));
    session_crypto_zeroize(h->sk_eph_L, sizeof(h->sk_eph_L));
    session_crypto_zeroize(h->c_L,      sizeof(h->c_L));
    session_crypto_zeroize(h->k_p2e,    sizeof(h->k_p2e));
    session_crypto_zeroize(h->k_e2p,    sizeof(h->k_e2p));

    h->stage = SESSION_STAGE_EMPTY;
}
