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
 * @file display_backend.h
 * @brief Internal backend ops table for the Display dispatcher.
 *
 * NOT a public header — lives in the display component only. Each backend
 * (console, panel) implements this table; display.c forwards the public
 * interface to whichever backend DISPLAY_BACKEND selected.
 */

#pragma once

#include "display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    display_err_t (*init)(void);
    display_err_t (*show_indication)(display_indication_t ind);
    display_err_t (*render_qr)(const uint8_t *payload, size_t len);
    display_err_t (*play_tone)(uint16_t freq_hz, uint32_t duration_ms);
    display_err_t (*clear)(void);
    display_err_t (*show_text)(const char *text);
} display_backend_ops_t;

extern const display_backend_ops_t display_backend_console_ops;
extern const display_backend_ops_t display_backend_panel_ops;

#ifdef __cplusplus
}
#endif
