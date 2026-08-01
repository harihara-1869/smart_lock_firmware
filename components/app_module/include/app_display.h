#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void app_display_show_qr(const uint8_t secret[32]);
void app_display_clear(void);

#ifdef __cplusplus
}
#endif
