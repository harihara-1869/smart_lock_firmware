#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void lli_mock_trigger_activation(void);
void lli_mock_push_rx(const uint8_t *data, size_t len);
bool lli_mock_pull_tx(uint8_t *out_data, size_t *out_len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
