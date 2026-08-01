#include "app_display.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "APP_DISPLAY";

void app_display_show_qr(const uint8_t secret[32])
{
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "        PROVISIONING QR CODE (ASCII)            ");
    ESP_LOGI(TAG, "================================================");
    
    char hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(&hex[i*2], "%02X", secret[i]);
    }
    hex[64] = '\0';
    
    ESP_LOGI(TAG, "SECRET: %s", hex);
    ESP_LOGI(TAG, "(Imagine a 250x122 E-ink display showing a QR)");
    ESP_LOGI(TAG, "================================================");
}

void app_display_clear(void)
{
    ESP_LOGI(TAG, "Display cleared.");
}
