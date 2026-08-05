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

/**
 * @file display_backend_console.c
 * @brief Console display backend — the default.
 *
 * TODO(hardware): no LEDs, no buzzer, no panel. Every call is logged with
 * what it WOULD do; the provisioning QR is rendered as a hex dump (ASCII QR
 * fallback). Success is never claimed for a peripheral that does not exist.
 */

#include "display_backend.h"

#include <stdio.h>

#include "esp_log.h"

static const char *TAG = "DISPLAY_CONSOLE";

static display_err_t console_init(void)
{
    ESP_LOGI(TAG, "console backend init "
                  "(LED green=%d, red=%d, buzzer=%d — TODO(hardware): not wired)",
             CONFIG_DISPLAY_PIN_LED_GREEN,
             CONFIG_DISPLAY_PIN_LED_RED,
             CONFIG_DISPLAY_PIN_BUZZER);
    return DISPLAY_OK;
}

static display_err_t console_show_indication(display_indication_t ind)
{
    const char *name;
    switch (ind) {
    case DISPLAY_IND_SUCCESS:
        name = "SUCCESS (green LED flash + success chime)";
        break;
    case DISPLAY_IND_ERROR:
        name = "ERROR (red LED flash + harsh beeps)";
        break;
    case DISPLAY_IND_PROVISIONING:
        name = "PROVISIONING (blue/amber LED pattern)";
        break;
    default:
        name = "IDLE (all indicators off)";
        break;
    }
    ESP_LOGI(TAG, "TODO(hardware): indication -> %s", name);
    return DISPLAY_OK;
}

static display_err_t console_render_qr(const uint8_t *payload, size_t len)
{
    if (!payload) {
        return DISPLAY_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "        PROVISIONING QR CODE (ASCII)            ");
    ESP_LOGI(TAG, "================================================");

    char hex[129];
    size_t chars = len * 2;
    if (chars >= sizeof(hex)) {
        chars = sizeof(hex) - 1;
    }
    for (size_t i = 0; i < chars / 2; i++) {
        snprintf(&hex[i * 2], 3, "%02X", payload[i]);
    }
    hex[chars] = '\0';

    ESP_LOGI(TAG, "PAYLOAD(%d): %s", (int)len, hex);
    ESP_LOGI(TAG, "(Imagine a 250x122 E-ink display showing a QR)");
    ESP_LOGI(TAG, "================================================");
    return DISPLAY_OK;
}

static display_err_t console_play_tone(uint16_t freq_hz, uint32_t duration_ms)
{
    ESP_LOGI(TAG, "TODO(hardware): tone %u Hz for %lu ms (no buzzer wired)",
             (unsigned)freq_hz, (unsigned long)duration_ms);
    return DISPLAY_OK;
}

static display_err_t console_clear(void)
{
    ESP_LOGI(TAG, "Display cleared (all indicators off).");
    return DISPLAY_OK;
}

static display_err_t console_show_text(const char *text)
{
    ESP_LOGI(TAG, "[display] %s", text ? text : "(null)");
    return DISPLAY_OK;
}

const display_backend_ops_t display_backend_console_ops = {
    .init            = console_init,
    .show_indication = console_show_indication,
    .render_qr       = console_render_qr,
    .play_tone       = console_play_tone,
    .clear           = console_clear,
    .show_text       = console_show_text,
};
