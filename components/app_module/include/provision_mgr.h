#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void provision_mgr_arm(uint32_t timeout_ms);
bool provision_mgr_is_active(void);
void provision_mgr_handle_cmd(const uint8_t *cmd_bytes, size_t len,
                              uint8_t *resp_bytes, size_t *resp_len);

#ifdef __cplusplus
}
#endif
