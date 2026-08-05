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
 * @file aai_backend_stub.c
 * @brief Honest simulated actuator backend (default).
 *
 * TODO(hardware): simulated motion only — no motor, no limit switches, no
 * stall pin. Every command is logged; the state machine transitions
 * MOVING -> LOCKED/UNLOCKED after ACTUATOR_MOVE_TIMEOUT_MS, or -> JAMMED if
 * ACTUATOR_SIM_JAM is enabled. It NEVER reports success for an action it did
 * not perform: AAI_Open/AAI_Close report AAI_ERR_BUSY while MOVING, and the
 * reported state always reflects what this backend actually did.
 */

#include "aai_backend.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "AAI_STUB";

static aai_state_t   s_state = AAI_STATE_UNKNOWN;
static aai_state_t   s_target = AAI_STATE_UNKNOWN;
static int64_t       s_move_deadline_us = 0;

/* Bool Kconfig symbols are only defined when set — never use them in runtime
 * C expressions. Resolve once at compile time instead. */
#if CONFIG_ACTUATOR_SIM_JAM
static const bool s_sim_jam = true;
#else
static const bool s_sim_jam = false;
#endif

static void log_intent(const char *action)
{
    ESP_LOGI(TAG, "TODO(hardware): [stub] would %s — simulated, no motor driven", action);
}

static aai_err_t stub_init(void)
{
    ESP_LOGI(TAG, "stub backend init (move timeout %d ms, sim-jam %s)",
             CONFIG_ACTUATOR_MOVE_TIMEOUT_MS,
             s_sim_jam ? "on" : "off");
    s_state = AAI_STATE_LOCKED; /* a freshly booted lock is assumed locked */
    return AAI_OK;
}

static aai_err_t stub_start_motion(aai_state_t target)
{
    if (s_state == AAI_STATE_MOVING) {
        ESP_LOGW(TAG, "busy — actuation already in progress");
        return AAI_ERR_BUSY;
    }
    if (s_state == AAI_STATE_JAMMED || s_state == AAI_STATE_FAULT) {
        ESP_LOGW(TAG, "cannot actuate from state %d (must recover first)", s_state);
        return AAI_ERR_INVALID_STATE;
    }
    if (s_state == target) {
        ESP_LOGI(TAG, "already at target state %d — no motion needed", target);
        return AAI_OK;
    }

    log_intent(target == AAI_STATE_UNLOCKED ? "open (drive to UNLOCKED)"
                                            : "close (drive to LOCKED)");
    s_target = target;
    s_move_deadline_us = esp_timer_get_time()
                         + (int64_t)CONFIG_ACTUATOR_MOVE_TIMEOUT_MS * 1000;
    s_state = AAI_STATE_MOVING;
    return AAI_OK;
}

static aai_err_t stub_open(void)
{
    return stub_start_motion(AAI_STATE_UNLOCKED);
}

static aai_err_t stub_close(void)
{
    return stub_start_motion(AAI_STATE_LOCKED);
}

static aai_err_t stub_stop(void)
{
    log_intent("stop (cease driving)");
    if (s_state == AAI_STATE_MOVING) {
        /* Honest: we stopped mid-motion, position is unknown. */
        s_state = AAI_STATE_UNKNOWN;
        s_target = AAI_STATE_UNKNOWN;
    }
    return AAI_OK;
}

static aai_state_t stub_get_status(void)
{
    if (s_state == AAI_STATE_MOVING) {
        int64_t now = esp_timer_get_time();
        if (now < s_move_deadline_us) {
            return AAI_STATE_MOVING;
        }
        if (s_sim_jam) {
            ESP_LOGW(TAG, "simulated jam at end of travel");
            s_state = AAI_STATE_JAMMED;
            s_target = AAI_STATE_UNKNOWN;
        } else {
            /* Deadline fired without a jam: the simulated bolt arrived. */
            ESP_LOGI(TAG, "simulated motion complete -> %s",
                     s_target == AAI_STATE_UNLOCKED ? "UNLOCKED" : "LOCKED");
            s_state = s_target;
            s_target = AAI_STATE_UNKNOWN;
        }
    }
    return s_state;
}

const aai_backend_ops_t aai_backend_stub_ops = {
    .init       = stub_init,
    .open       = stub_open,
    .close      = stub_close,
    .stop       = stub_stop,
    .get_status = stub_get_status,
};
