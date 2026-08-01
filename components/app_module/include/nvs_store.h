#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool nvs_store_add_key(const uint8_t pubkey[32]);
bool nvs_store_is_key_authorized(const uint8_t pubkey[32]);

#ifdef __cplusplus
}
#endif
