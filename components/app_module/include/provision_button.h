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
 * @file provision_button.h
 * @brief Event-driven provision button (ISR + task-notification doorbell).
 *
 * The button is the physical-presence gate for provisioning (master doc
 * §11.4: "the one physical action this entire feature's security rests on").
 * It is deliberately event-driven, matching the project philosophy and the
 * PN532 IRQ pattern:
 *
 *   - A GPIO ISR (ANYEDGE) is a pure doorbell — it only records the edge and
 *     notifies the Application task via xTaskNotifyGive(). It never touches
 *     I2C, never blocks, and never runs application logic.
 *   - The Application task, already woken by the notification (which coalesces
 *     with comm mailbox notifications), performs the debounce and
 *     hold-to-arm logic in task context — where vTaskDelay and provisioning
 *     calls are legal.
 *
 * The CPU never polls the pin.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROV_BUTTON_OK = 0,
    PROV_BUTTON_ERR_INVALID_ARG,
    PROV_BUTTON_ERR_INIT,     /* GPIO/ISR setup failed                       */
    PROV_BUTTON_ERR_DISABLED, /* button GPIO is -1 (Kconfig)                */
} prov_button_err_t;

/**
 * Initialise the button: configure the GPIO (input, pull-up, ANYEDGE ISR) and
 * install the ISR doorbell that notifies the Application task.
 *
 * Must be called once, before the Application task starts (or from it),
 * after the ISR service is available. The button notifies @p task on every
 * edge; the task then calls ProvisionButton_OnTaskNotified() to debounce and
 * evaluate the hold.
 *
 * @param task  The Application task handle to notify.
 * @return PROV_BUTTON_OK, _ERR_DISABLED (GPIO -1), or _ERR_INIT.
 */
prov_button_err_t ProvisionButton_Init(void *task);

/**
 * Handle a button-edge notification from the Application task context.
 *
 * Debounces the level, tracks press duration, and arms provisioning when the
 * button has been held for APP_PROVISION_BUTTON_HOLD_MS. Non-blocking beyond
 * a bounded debounce read.
 *
 * @return true if a hold-to-arm fired (provisioning window armed).
 */
bool ProvisionButton_OnTaskNotified(void);

#ifdef __cplusplus
}
#endif
