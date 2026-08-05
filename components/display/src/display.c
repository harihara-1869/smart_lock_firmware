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
 * @file display.c
 * @brief Display dispatcher — forwards the public interface to the backend
 *        selected by DISPLAY_BACKEND in Kconfig.
 */

#include "display.h"

#include "display_backend.h"

static const display_backend_ops_t *backend(void)
{
#if defined(CONFIG_DISPLAY_BACKEND_PANEL)
    return &display_backend_panel_ops;
#else
    return &display_backend_console_ops;
#endif
}

display_err_t Display_Init(void)
{
    return backend()->init();
}

display_err_t Display_ShowIndication(display_indication_t ind)
{
    return backend()->show_indication(ind);
}

display_err_t Display_RenderQR(const uint8_t *payload, size_t len)
{
    return backend()->render_qr(payload, len);
}

display_err_t Display_PlayTone(uint16_t freq_hz, uint32_t duration_ms)
{
    return backend()->play_tone(freq_hz, duration_ms);
}

display_err_t Display_Clear(void)
{
    return backend()->clear();
}

display_err_t Display_ShowText(const char *text)
{
    return backend()->show_text(text);
}
