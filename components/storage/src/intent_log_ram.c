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
 * @file intent_log_ram.c
 * @brief Provisional RAM placeholder for the actuation-intent log.
 *
 * TODO(storage): provisional RAM backend — NO power-loss persistence. An
 * intent set just before a power loss is lost, so boot recovery has nothing
 * to read. The planned NVS / secure-element backend will make the intent
 * survive reboot behind the same intent_log.h interface.
 */

#include "intent_log.h"

#include "esp_log.h"

static const char *TAG = "INTENT_LOG";

static intent_target_t s_target = INTENT_TARGET_NONE;

intent_err_t IntentLog_Init(void)
{
    s_target = INTENT_TARGET_NONE;
    return INTENT_OK;
}

intent_err_t IntentLog_SetTarget(intent_target_t target)
{
    ESP_LOGW(TAG, "TODO(storage): RAM placeholder — intent is NOT persisted across reboot");
    s_target = target;
    return INTENT_OK;
}

intent_target_t IntentLog_GetTarget(void)
{
    return s_target;
}

intent_err_t IntentLog_ClearTarget(void)
{
    s_target = INTENT_TARGET_NONE;
    return INTENT_OK;
}
