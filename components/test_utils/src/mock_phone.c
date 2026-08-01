#include "mock_phone.h"
#include "../../session/src/session_crypto.h"
#include <string.h>

#define SESSION_PROTOCOL_VERSION  0x01
#define SESSION_DOMAIN            "SLOCK-HS-v1"
#define SESSION_DOMAIN_LEN        11
#define SESSION_TRANSCRIPT_LEN    (SESSION_DOMAIN_LEN + 1 + 32 + 32 + 32 + 32)

void mock_phone_init(mock_phone_t *phone, const uint8_t phone_sk[64], const uint8_t lock_pk[32]) {
    memset(phone, 0, sizeof(mock_phone_t));
    memcpy(phone->phone_sk, phone_sk, 64);
    memcpy(phone->phone_pk, &phone_sk[32], 32); /* Libsodium layout: seed||pk */
    memcpy(phone->lock_pk, lock_pk, 32);
}

void mock_phone_get_m1(mock_phone_t *phone, uint8_t m1_out[64]) {
    session_crypto_x25519_keypair(phone->eph_pk, phone->eph_sk);
    session_crypto_random(phone->c_P, 32);
    memcpy(m1_out, phone->eph_pk, 32);
    memcpy(m1_out + 32, phone->c_P, 32);
}

bool mock_phone_process_m2_get_m3(mock_phone_t *phone, const uint8_t m2[128], uint8_t m3_out[64]) {
    const uint8_t *eph_pk_L = m2;
    const uint8_t *c_L = m2 + 32;
    const uint8_t *sig_L = m2 + 64;

    uint8_t transcript[SESSION_TRANSCRIPT_LEN];
    size_t off = 0;
    memcpy(transcript + off, SESSION_DOMAIN, SESSION_DOMAIN_LEN);
    off += SESSION_DOMAIN_LEN;
    transcript[off++] = SESSION_PROTOCOL_VERSION;
    memcpy(transcript + off, phone->eph_pk, 32); off += 32;
    memcpy(transcript + off, eph_pk_L, 32); off += 32;
    memcpy(transcript + off, phone->c_P, 32); off += 32;
    memcpy(transcript + off, c_L, 32); off += 32;

    if (session_crypto_ed25519_verify(phone->lock_pk, transcript, SESSION_TRANSCRIPT_LEN, sig_L) != 0) {
        return false;
    }

    uint8_t shared[32];
    if (session_crypto_x25519_shared(phone->eph_sk, eph_pk_L, shared) != 0) {
        return false;
    }

    uint8_t salt[64];
    memcpy(salt, phone->c_P, 32);
    memcpy(salt + 32, c_L, 32);

    static const char INFO_P2E[] = "phone->esp";
    static const char INFO_E2P[] = "esp->phone";

    session_crypto_hkdf_sha256(salt, sizeof(salt), shared, sizeof(shared), (const uint8_t*)INFO_P2E, sizeof(INFO_P2E)-1, phone->k_p2e, 32);
    session_crypto_hkdf_sha256(salt, sizeof(salt), shared, sizeof(shared), (const uint8_t*)INFO_E2P, sizeof(INFO_E2P)-1, phone->k_e2p, 32);

    session_crypto_ed25519_sign(phone->phone_sk, transcript, SESSION_TRANSCRIPT_LEN, m3_out);
    return true;
}

void mock_phone_encrypt_payload(mock_phone_t *phone, const uint8_t *pt, size_t pt_len, uint8_t *ct_out, size_t *ct_len) {
    uint8_t nonce[12];
    session_crypto_random(nonce, 12);
    
    memcpy(ct_out, nonce, 12);
    session_crypto_aes256gcm_seal(phone->k_p2e, nonce, pt, pt_len, ct_out + 12, ct_out + 12 + pt_len);
    *ct_len = 12 + pt_len + 16;
}

bool mock_phone_decrypt_payload(mock_phone_t *phone, const uint8_t *ct, size_t ct_len, uint8_t *pt_out, size_t *pt_len) {
    if (ct_len < 28) return false;
    const uint8_t *nonce = ct;
    const uint8_t *ciphertext = ct + 12;
    size_t payload_len = ct_len - 28;
    const uint8_t *tag = ct + ct_len - 16;
    
    if (session_crypto_aes256gcm_open(phone->k_e2p, nonce, ciphertext, payload_len, tag, pt_out) != 0) {
        return false;
    }
    *pt_len = payload_len;
    return true;
}
