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
 * @file display.h
 * @brief Display peer — all user-feedback peripherals (status LED(s),
 *        buzzer/chime, QR-capable display panel).
 *
 * The Application decides WHAT and WHEN to show (including the Provision
 * Secret QR content); the Display owns only HOW it is rendered. The backend
 * (console log now, real panel driver later) is selected by DISPLAY_BACKEND
 * in Kconfig and is swappable without changing this interface.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Semantic indications the Application can request. */
typedef enum {
    DISPLAY_IND_IDLE = 0,        /* no active indication                */
    DISPLAY_IND_SUCCESS,         /* actuation/action succeeded (green)  */
    DISPLAY_IND_ERROR,           /* actuation/action failed (red + beep)*/
    DISPLAY_IND_PROVISIONING,    /* provisioning window is armed        */
} display_indication_t;

typedef enum {
    DISPLAY_OK = 0,
    DISPLAY_ERR_NOT_IMPLEMENTED, /* backend cannot perform this yet     */
    DISPLAY_ERR_INVALID_ARG,
    DISPLAY_ERR_INTERNAL,
} display_err_t;

/**
 * Initialise the display backend (LEDs, buzzer, panel as configured).
 */
display_err_t Display_Init(void);

/**
 * Show a semantic success/error/provisioning indication (LED/buzzer pattern).
 */
display_err_t Display_ShowIndication(display_indication_t ind);

/**
 * Render a QR code for @p payload bytes (the Provision Secret, etc.).
 * The payload is opaque to the Display — it only renders it.
 */
display_err_t Display_RenderQR(const uint8_t *payload, size_t len);

/**
 * Play a tone on the buzzer/chime.
 *
 * @param freq_hz      Tone frequency; 0 = no tone.
 * @param duration_ms  Tone duration.
 */
display_err_t Display_PlayTone(uint16_t freq_hz, uint32_t duration_ms);

/**
 * Clear the panel / turn off all indicators.
 */
display_err_t Display_Clear(void);

/**
 * Show a short text line (debug/status). Optional; may be a no-op on
 * backends without text rendering.
 */
display_err_t Display_ShowText(const char *text);

#ifdef __cplusplus
}
#endif
