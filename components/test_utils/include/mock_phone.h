#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t phone_sk[64];
    uint8_t phone_pk[32];
    uint8_t lock_pk[32];
    
    uint8_t eph_sk[32];
    uint8_t eph_pk[32];
    uint8_t c_P[32];
    
    uint8_t k_p2e[32];
    uint8_t k_e2p[32];
} mock_phone_t;

void mock_phone_init(mock_phone_t *phone, const uint8_t phone_sk[64], const uint8_t lock_pk[32]);
void mock_phone_get_m1(mock_phone_t *phone, uint8_t m1_out[64]);
bool mock_phone_process_m2_get_m3(mock_phone_t *phone, const uint8_t m2[128], uint8_t m3_out[64]);
void mock_phone_encrypt_payload(mock_phone_t *phone, const uint8_t *pt, size_t pt_len, uint8_t *ct_out, size_t *ct_len);
bool mock_phone_decrypt_payload(mock_phone_t *phone, const uint8_t *ct, size_t ct_len, uint8_t *pt_out, size_t *pt_len);

#ifdef __cplusplus
}
#endif
