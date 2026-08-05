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
 * @file integrity_backend.h
 * @brief Internal backend ops table for the Integrity dispatcher.
 *
 * NOT a public header — lives in the integrity component only.
 */

#pragma once

#include "integrity.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    integrity_init_err_t (*init)(void);
    integrity_status_t   (*run_checks)(void);
} integrity_backend_ops_t;

extern const integrity_backend_ops_t integrity_backend_stub_ops;
extern const integrity_backend_ops_t integrity_backend_tamper_gpio_ops;

#ifdef __cplusplus
}
#endif
