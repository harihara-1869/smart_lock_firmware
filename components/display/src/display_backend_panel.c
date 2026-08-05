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
 * @file display_backend_panel.c
 * @brief E-paper panel backend (SPI, 250x122) — honest skeleton.
 *
 * TODO(hardware): NOT implemented. Compiles when DISPLAY_BACKEND_PANEL is
 * selected, logs that the panel is not wired, and returns
 * DISPLAY_ERR_NOT_IMPLEMENTED rather than claiming it rendered anything.
 * Implementation lands with the real panel driver (SPI bus, CS/DC/RST pins
 * from Kconfig).
 */

#include "display_backend.h"

#include <stddef.h>

#include "esp_log.h"

#if defined(CONFIG_DISPLAY_BACKEND_PANEL)

static const char *TAG = "DISPLAY_PANEL";

static display_err_t panel_init(void)
{
    ESP_LOGE(TAG, "TODO(hardware): e-paper panel backend not implemented — "
                  "panel not wired (CS=%d DC=%d RST=%d)",
             CONFIG_DISPLAY_PIN_PANEL_CS,
             CONFIG_DISPLAY_PIN_PANEL_DC,
             CONFIG_DISPLAY_PIN_PANEL_RST);
    return DISPLAY_ERR_NOT_IMPLEMENTED;
}

static display_err_t panel_show_indication(display_indication_t ind)
{
    (void)ind;
    ESP_LOGE(TAG, "TODO(hardware): panel indication not implemented");
    return DISPLAY_ERR_NOT_IMPLEMENTED;
}

static display_err_t panel_render_qr(const uint8_t *payload, size_t len)
{
    (void)payload;
    (void)len;
    ESP_LOGE(TAG, "TODO(hardware): panel QR rendering not implemented");
    return DISPLAY_ERR_NOT_IMPLEMENTED;
}

static display_err_t panel_play_tone(uint16_t freq_hz, uint32_t duration_ms)
{
    (void)freq_hz;
    (void)duration_ms;
    ESP_LOGE(TAG, "TODO(hardware): panel tone not implemented");
    return DISPLAY_ERR_NOT_IMPLEMENTED;
}

static display_err_t panel_clear(void)
{
    ESP_LOGE(TAG, "TODO(hardware): panel clear not implemented");
    return DISPLAY_ERR_NOT_IMPLEMENTED;
}

static display_err_t panel_show_text(const char *text)
{
    (void)text;
    ESP_LOGE(TAG, "TODO(hardware): panel text not implemented");
    return DISPLAY_ERR_NOT_IMPLEMENTED;
}

const display_backend_ops_t display_backend_panel_ops = {
    .init            = panel_init,
    .show_indication = panel_show_indication,
    .render_qr       = panel_render_qr,
    .play_tone       = panel_play_tone,
    .clear           = panel_clear,
    .show_text       = panel_show_text,
};

#else /* !CONFIG_DISPLAY_BACKEND_PANEL */

/* Not selected: the dispatcher never reaches this table, but the symbol must
 * exist so the build stays link-clean. */
const display_backend_ops_t display_backend_panel_ops = {
    .init            = NULL,
    .show_indication = NULL,
    .render_qr       = NULL,
    .play_tone       = NULL,
    .clear           = NULL,
    .show_text       = NULL,
};

#endif /* CONFIG_DISPLAY_BACKEND_PANEL */
