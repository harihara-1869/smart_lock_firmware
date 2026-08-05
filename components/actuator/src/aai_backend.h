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
 * @file aai_backend.h
 * @brief Internal backend ops table for the AAI dispatcher.
 *
 * NOT a public header — lives in the actuator component only. Each backend
 * (stub, RMT, MCPWM) implements this table; aai.c forwards the public
 * interface to whichever backend ACTUATOR_BACKEND selected. Exactly one
 * backend is compiled in.
 */

#pragma once

#include "aai.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    aai_err_t   (*init)(void);
    aai_err_t   (*open)(void);
    aai_err_t   (*close)(void);
    aai_err_t   (*stop)(void);
    aai_state_t (*get_status)(void);
} aai_backend_ops_t;

/* One of these is provided per ACTUATOR_BACKEND choice. */
extern const aai_backend_ops_t aai_backend_stub_ops;
extern const aai_backend_ops_t aai_backend_rmt_ops;
extern const aai_backend_ops_t aai_backend_mcpwm_ops;

#ifdef __cplusplus
}
#endif
