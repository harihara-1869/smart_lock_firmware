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
 * @file integrity_backend_tamper_gpio.c
 * @brief Real tamper-switch backend (GPIO) — honest skeleton.
 *
 * TODO(hardware): NOT implemented. Compiles when
 * INTEGRITY_BACKEND_TAMPER_GPIO is selected, logs that the sensor is not
 * wired, and reports INTEGRITY_ERR (sensor fault) rather than claiming a
 * clean check. Implementation lands with the tamper sensor (debounced GPIO
 * read on INTEGRITY_PIN_TAMPER).
 */

#include "integrity_backend.h"

#include <stddef.h>

#include "esp_log.h"

#if defined(CONFIG_INTEGRITY_BACKEND_TAMPER_GPIO)

static const char *TAG = "INTEGRITY_GPIO";

static integrity_init_err_t gpio_init(void)
{
    ESP_LOGE(TAG, "TODO(hardware): tamper GPIO backend not implemented — "
                  "sensor not wired (pin %d)",
             CONFIG_INTEGRITY_PIN_TAMPER);
    return INTEGRITY_INIT_ERR;
}

static integrity_status_t gpio_run_checks(void)
{
    ESP_LOGE(TAG, "TODO(hardware): tamper GPIO check not implemented");
    return INTEGRITY_ERR;
}

const integrity_backend_ops_t integrity_backend_tamper_gpio_ops = {
    .init        = gpio_init,
    .run_checks  = gpio_run_checks,
};

#else /* !CONFIG_INTEGRITY_BACKEND_TAMPER_GPIO */

/* Not selected: symbol must still exist so the build stays link-clean. */
const integrity_backend_ops_t integrity_backend_tamper_gpio_ops = {
    .init        = NULL,
    .run_checks  = NULL,
};

#endif /* CONFIG_INTEGRITY_BACKEND_TAMPER_GPIO */
