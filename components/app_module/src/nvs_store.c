#include "nvs_store.h"
#include <string.h>

#define MAX_KEYS 10
static uint8_t s_keys[MAX_KEYS][32];
static int s_num_keys = 0;

bool nvs_store_add_key(const uint8_t pubkey[32])
{
    if (s_num_keys >= MAX_KEYS) {
        return false;
    }
    memcpy(s_keys[s_num_keys], pubkey, 32);
    s_num_keys++;
    return true;
}

bool nvs_store_is_key_authorized(const uint8_t pubkey[32])
{
    for (int i = 0; i < s_num_keys; i++) {
        if (memcmp(s_keys[i], pubkey, 32) == 0) {
            return true;
        }
    }
    return false;
}
