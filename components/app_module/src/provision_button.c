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

#include "provision_button.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "PROV_BTN";

/* Active-low: pressed = LOW, released = HIGH. */
#define PROV_BTN_PRESSED_LEVEL   0
#define PROV_BTN_RELEASED_LEVEL  1

/* The button GPIO is disabled (Kconfig -1) → all calls are no-ops. */
#define PROV_BTN_DISABLED()  (CONFIG_APP_PROVISION_BUTTON_GPIO < 0)

typedef struct {
    bool          inited;
    bool          isr_added;
    TaskHandle_t  app_task;         /* task notified on every edge          */
    int           gpio;
    int64_t       edge_ts_us;       /* timestamp of the last ISR edge       */
    bool          pressed;          /* debounced press state                */
    int64_t       press_start_us;   /* timestamp when the press was confirmed */
    int           last_level;       /* last observed GPIO level             */
} prov_button_ctx_t;

static prov_button_ctx_t s_ctx;

/**
 * ISR doorbell — runs in ISR context. ONLY records the edge and notifies the
 * Application task; never blocks, never touches I2C, never runs app logic.
 * Mirrors the PN532 IRQ ISR (pn532_i2c.c pn532_irq_isr).
 */
static void IRAM_ATTR prov_button_isr(void *arg)
{
    prov_button_ctx_t *c = (prov_button_ctx_t *)arg;
    BaseType_t hp_task_woken = pdFALSE;

    c->edge_ts_us = esp_timer_get_time();
    vTaskNotifyGiveFromISR(c->app_task, &hp_task_woken);
    if (hp_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

prov_button_err_t ProvisionButton_Init(void *task)
{
    if (PROV_BTN_DISABLED()) {
        ESP_LOGI(TAG, "provision button disabled (GPIO -1)");
        return PROV_BUTTON_ERR_DISABLED;
    }
    if (!task) {
        return PROV_BUTTON_ERR_INVALID_ARG;
    }

    s_ctx.app_task      = (TaskHandle_t)task;
    s_ctx.gpio          = CONFIG_APP_PROVISION_BUTTON_GPIO;
    s_ctx.edge_ts_us    = 0;
    s_ctx.pressed       = false;
    s_ctx.press_start_us = 0;

    /* Active-low with internal pull-up (matches the PN532 IRQ wiring). */
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << s_ctx.gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,   /* both press and release edges */
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return PROV_BUTTON_ERR_INIT;
    }

    /* The ISR service is process-global; tolerate it already being installed
     * (the PN532 IRQ path may have installed it first). */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
        return PROV_BUTTON_ERR_INIT;
    }

    err = gpio_isr_handler_add(s_ctx.gpio, prov_button_isr, &s_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(err));
        return PROV_BUTTON_ERR_INIT;
    }
    s_ctx.isr_added = true;
    s_ctx.inited    = true;
    /* Baseline: read the current level so the first real edge is detected
     * against a known state (pull-up ⇒ released by default). */
    s_ctx.last_level = gpio_get_level(s_ctx.gpio);

    ESP_LOGI(TAG, "provision button ready (GPIO %d, active-low, hold %d ms)",
             s_ctx.gpio, CONFIG_APP_PROVISION_BUTTON_HOLD_MS);
    return PROV_BUTTON_OK;
}

bool ProvisionButton_OnTaskNotified(void)
{
    if (!s_ctx.inited) {
        return false;
    }

    const int level = gpio_get_level(s_ctx.gpio);
    const int64_t now_us = esp_timer_get_time();

    /* ---- Debounce (non-blocking) ---- */
    /* Track level changes only if they have persisted for the debounce
     * window. If the level differs from the last confirmed one, treat it as a
     * candidate and wait; only commit once stable. */
    if (level != s_ctx.last_level) {
        if (s_ctx.edge_ts_us == 0) {
            s_ctx.edge_ts_us = now_us;
        }
        if (now_us - s_ctx.edge_ts_us >=
                (int64_t)CONFIG_APP_PROVISION_BUTTON_DEBOUNCE_MS * 1000) {
            /* Stable change — commit. */
            s_ctx.last_level = level;
            s_ctx.edge_ts_us = 0;

            const bool now_pressed = (level == PROV_BTN_PRESSED_LEVEL);
            if (now_pressed) {
                s_ctx.pressed = true;
                s_ctx.press_start_us = now_us;
                ESP_LOGI(TAG, "provision button pressed — hold %d ms to arm",
                         CONFIG_APP_PROVISION_BUTTON_HOLD_MS);
            } else {
                s_ctx.pressed = false;
                s_ctx.press_start_us = 0;
                ESP_LOGI(TAG, "provision button released");
            }
        }
        return false;
    }

    /* ---- Hold check (runs every call, incl. the bounded-wait cadence) ---- */
    if (s_ctx.pressed && now_us - s_ctx.press_start_us >=
            (int64_t)CONFIG_APP_PROVISION_BUTTON_HOLD_MS * 1000) {
        s_ctx.pressed = false;
        s_ctx.press_start_us = 0;
        ESP_LOGI(TAG, "provision button hold complete — arming window");
        return true;
    }

    return false;
}
