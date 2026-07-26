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

#include "transport.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TRANSPORT";

#define INS_HANDSHAKE_INIT    0x10
#define INS_HANDSHAKE_FINISH  0x11
#define INS_SECURE_PAYLOAD    0x20
#define INS_SESSION_ABORT     0x30

#define SW1_OK              0x90
#define SW2_OK              0x00
#define SW1_SECURITY        0x69
#define SW2_AUTH_FAILED     0x82
#define SW2_WRONG_STATE     0x85
#define SW1_WRONG_PARAM     0x6A
#define SW2_INVALID_DATA    0x80
#define SW2_NOT_SUPPORTED   0x81

#define PLAINTEXT_BUDGET    227  /* 255 - 12 (GCM nonce) - 16 (MAC tag) */

struct transport_t {
    transport_config_t cfg;
    transport_state_t  state;
    lli_handle_t       lli;
};

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static void invoke_erase(transport_handle_t h)
{
    if (h->cfg.on_erase) {
        h->cfg.on_erase(h->cfg.user_ctx);
    }
}

/**
 * Parse a raw byte buffer into a transport_capdu_t.
 *
 * Returns TRANSPORT_OK on success. On failure, *sw1 / *sw2 are set to the
 * status words the transport layer should send back to the reader.
 */
static transport_err_t parse_capdu(const uint8_t *raw, size_t len,
                                   transport_capdu_t *out,
                                   uint8_t *sw1, uint8_t *sw2)
{
    if (len < 5) {
        *sw1 = SW1_WRONG_PARAM;
        *sw2 = SW2_INVALID_DATA;
        return TRANSPORT_ERR_INVALID_APDU;
    }

    out->cla = raw[0];
    out->ins = raw[1];
    out->p1  = raw[2];
    out->p2  = raw[3];
    out->lc  = raw[4];

    if (out->cla != 0x80) {
        *sw1 = SW1_WRONG_PARAM;
        *sw2 = SW2_NOT_SUPPORTED;
        return TRANSPORT_ERR_INVALID_APDU;
    }

    if (out->lc != len - 5) {
        *sw1 = SW1_WRONG_PARAM;
        *sw2 = SW2_INVALID_DATA;
        return TRANSPORT_ERR_INVALID_APDU;
    }

    if (out->lc > 0) {
        memcpy(out->data, &raw[5], out->lc);
    }

    return TRANSPORT_OK;
}

static transport_err_t send_status(transport_handle_t h,
                                   uint8_t sw1, uint8_t sw2,
                                   uint32_t timeout_ms)
{
    uint8_t buf[2] = {sw1, sw2};
    lli_err_t err = lli_send_apdu(h->lli, buf, sizeof(buf), timeout_ms);
    if (err != LLI_OK) {
        ESP_LOGE(TAG, "send_status failed: %d", err);
        return TRANSPORT_ERR_INTERNAL;
    }
    return TRANSPORT_OK;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

transport_err_t transport_init(const transport_config_t *cfg,
                               transport_handle_t *handle_out)
{
    if (!cfg || !handle_out) {
        return TRANSPORT_ERR_INVALID_APDU;
    }

    struct transport_t *h = calloc(1, sizeof(*h));
    if (!h) {
        return TRANSPORT_ERR_INTERNAL;
    }

    lli_err_t err = lli_init(&cfg->lli_cfg, &h->lli);
    if (err != LLI_OK) {
        ESP_LOGE(TAG, "lli_init failed: %d", err);
        free(h);
        return TRANSPORT_ERR_INTERNAL;
    }

    h->cfg   = *cfg;
    h->state = TRANSPORT_STATE_IDLE;
    *handle_out = h;
    return TRANSPORT_OK;
}

transport_err_t transport_deinit(transport_handle_t handle)
{
    if (!handle) {
        return TRANSPORT_OK;
    }
    lli_deinit(handle->lli);
    free(handle);
    return TRANSPORT_OK;
}

transport_state_t transport_get_state(transport_handle_t handle)
{
    if (!handle) {
        return TRANSPORT_STATE_IDLE;
    }
    return handle->state;
}

/* ------------------------------------------------------------------ */
/* State handlers                                                      */
/* ------------------------------------------------------------------ */

static transport_err_t run_idle(transport_handle_t h)
{
    lli_err_t err = lli_activate(h->lli, h->cfg.activate_timeout_ms);
    if (err == LLI_ERR_TIMEOUT) {
        ESP_LOGD(TAG, "activate timeout");
        return TRANSPORT_ERR_TIMEOUT;
    }
    if (err != LLI_OK) {
        ESP_LOGE(TAG, "activate failed: %d", err);
        return TRANSPORT_ERR_INTERNAL;
    }

    h->state = TRANSPORT_STATE_ACTIVATED;
    return TRANSPORT_OK;
}

static transport_err_t run_activated(transport_handle_t h)
{
    uint8_t raw[264];
    size_t  raw_len = 0;

    lli_err_t err = lli_receive_apdu(h->lli, raw, sizeof(raw),
                                     &raw_len, h->cfg.apdu_timeout_ms);
    if (err != LLI_OK) {
        ESP_LOGW(TAG, "activated receive failed: %d", err);
        invoke_erase(h);
        h->state = TRANSPORT_STATE_RELEASED;
        return TRANSPORT_OK;
    }

    transport_capdu_t capdu;
    uint8_t sw1, sw2;
    if (parse_capdu(raw, raw_len, &capdu, &sw1, &sw2) != TRANSPORT_OK) {
        send_status(h, sw1, sw2, h->cfg.apdu_timeout_ms);
        return TRANSPORT_OK;
    }

    if (capdu.ins == INS_SESSION_ABORT) {
        invoke_erase(h);
        send_status(h, SW1_OK, SW2_OK, h->cfg.apdu_timeout_ms);
        h->state = TRANSPORT_STATE_RELEASED;
        return TRANSPORT_OK;
    }

    if (capdu.ins != INS_HANDSHAKE_INIT) {
        ESP_LOGW(TAG, "unexpected INS 0x%02X in ACTIVATED", capdu.ins);
        send_status(h, SW1_SECURITY, SW2_WRONG_STATE, h->cfg.apdu_timeout_ms);
        return TRANSPORT_OK;
    }

    transport_rapdu_t rapdu;
    transport_err_t app_err = h->cfg.on_apdu(&capdu, &rapdu, h->cfg.user_ctx);
    if (app_err != TRANSPORT_OK) {
        ESP_LOGE(TAG, "on_apdu failed for M1: %d", app_err);
        send_status(h, SW1_SECURITY, SW2_AUTH_FAILED, h->cfg.apdu_timeout_ms);
        invoke_erase(h);
        h->state = TRANSPORT_STATE_RELEASED;
        return TRANSPORT_OK;
    }

    err = lli_send_apdu(h->lli, rapdu.data, rapdu.len, h->cfg.apdu_timeout_ms);
    if (err != LLI_OK) {
        ESP_LOGE(TAG, "send M2 failed: %d", err);
        invoke_erase(h);
        h->state = TRANSPORT_STATE_RELEASED;
        return TRANSPORT_OK;
    }

    h->state = TRANSPORT_STATE_HANDSHAKE;
    return TRANSPORT_OK;
}

static transport_err_t run_handshake(transport_handle_t h)
{
    TickType_t deadline = xTaskGetTickCount() +
                          pdMS_TO_TICKS(h->cfg.handshake_timeout_ms);

    while (1) {
        if (lli_get_link_status(h->lli) == LLI_STATUS_RELEASED) {
            ESP_LOGW(TAG, "link released during handshake");
            invoke_erase(h);
            h->state = TRANSPORT_STATE_RELEASED;
            return TRANSPORT_OK;
        }

        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            ESP_LOGW(TAG, "handshake timer expired");
            invoke_erase(h);
            h->state = TRANSPORT_STATE_RELEASED;
            return TRANSPORT_OK;
        }
        uint32_t remaining_ms =
            (uint32_t)((deadline - now) * portTICK_PERIOD_MS);
        if (remaining_ms == 0) {
            invoke_erase(h);
            h->state = TRANSPORT_STATE_RELEASED;
            return TRANSPORT_OK;
        }

        uint8_t raw[264];
        size_t  raw_len = 0;
        lli_err_t err = lli_receive_apdu(h->lli, raw, sizeof(raw),
                                         &raw_len, remaining_ms);
        if (err == LLI_ERR_LINK_RELEASED || err == LLI_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "handshake receive lost: %d", err);
            invoke_erase(h);
            h->state = TRANSPORT_STATE_RELEASED;
            return TRANSPORT_OK;
        }
        if (err != LLI_OK) {
            ESP_LOGW(TAG, "handshake receive frame error, retrying");
            continue;
        }

        transport_capdu_t capdu;
        uint8_t sw1, sw2;
        if (parse_capdu(raw, raw_len, &capdu, &sw1, &sw2) != TRANSPORT_OK) {
            send_status(h, sw1, sw2, h->cfg.apdu_timeout_ms);
            continue;
        }

        if (capdu.ins == INS_SESSION_ABORT) {
            invoke_erase(h);
            send_status(h, SW1_OK, SW2_OK, h->cfg.apdu_timeout_ms);
            h->state = TRANSPORT_STATE_RELEASED;
            return TRANSPORT_OK;
        }

        if (capdu.ins != INS_HANDSHAKE_FINISH) {
            ESP_LOGW(TAG, "unexpected INS 0x%02X in HANDSHAKE", capdu.ins);
            send_status(h, SW1_SECURITY, SW2_WRONG_STATE,
                        h->cfg.apdu_timeout_ms);
            continue;
        }

        transport_rapdu_t rapdu;
        transport_err_t app_err =
            h->cfg.on_apdu(&capdu, &rapdu, h->cfg.user_ctx);
        if (app_err != TRANSPORT_OK) {
            ESP_LOGW(TAG, "M3 verification failed");
            send_status(h, SW1_SECURITY, SW2_AUTH_FAILED,
                        h->cfg.apdu_timeout_ms);
            invoke_erase(h);
            h->state = TRANSPORT_STATE_RELEASED;
            return TRANSPORT_OK;
        }

        send_status(h, SW1_OK, SW2_OK, h->cfg.apdu_timeout_ms);
        h->state = TRANSPORT_STATE_SECURE_SESSION;
        return TRANSPORT_OK;
    }
}

static transport_err_t run_secure_session(transport_handle_t h)
{
    while (1) {
        if (lli_get_link_status(h->lli) == LLI_STATUS_RELEASED) {
            ESP_LOGW(TAG, "link released in secure session");
            invoke_erase(h);
            h->state = TRANSPORT_STATE_RELEASED;
            return TRANSPORT_OK;
        }

        uint8_t raw[264];
        size_t  raw_len = 0;
        lli_err_t err = lli_receive_apdu(h->lli, raw, sizeof(raw),
                                         &raw_len, h->cfg.apdu_timeout_ms);
        if (err != LLI_OK) {
            ESP_LOGW(TAG, "secure receive failed: %d", err);
            invoke_erase(h);
            h->state = TRANSPORT_STATE_RELEASED;
            return TRANSPORT_OK;
        }

        transport_capdu_t capdu;
        uint8_t sw1, sw2;
        if (parse_capdu(raw, raw_len, &capdu, &sw1, &sw2) != TRANSPORT_OK) {
            send_status(h, sw1, sw2, h->cfg.apdu_timeout_ms);
            continue;
        }

        if (capdu.ins == INS_SESSION_ABORT) {
            invoke_erase(h);
            send_status(h, SW1_OK, SW2_OK, h->cfg.apdu_timeout_ms);
            h->state = TRANSPORT_STATE_RELEASED;
            return TRANSPORT_OK;
        }

        if (capdu.ins != INS_SECURE_PAYLOAD) {
            ESP_LOGW(TAG, "unexpected INS 0x%02X in SECURE_SESSION", capdu.ins);
            send_status(h, SW1_SECURITY, SW2_WRONG_STATE,
                        h->cfg.apdu_timeout_ms);
            continue;
        }

        if (capdu.lc > PLAINTEXT_BUDGET) {
            ESP_LOGW(TAG, "payload too large: %u > %d",
                     (unsigned)capdu.lc, PLAINTEXT_BUDGET);
            send_status(h, SW1_WRONG_PARAM, SW2_INVALID_DATA,
                        h->cfg.apdu_timeout_ms);
            continue;
        }

        transport_rapdu_t rapdu;
        transport_err_t app_err =
            h->cfg.on_apdu(&capdu, &rapdu, h->cfg.user_ctx);
        if (app_err != TRANSPORT_OK) {
            ESP_LOGW(TAG, "secure payload rejected (GCM tag mismatch?)");
            send_status(h, SW1_SECURITY, SW2_AUTH_FAILED,
                        h->cfg.apdu_timeout_ms);
            invoke_erase(h);
            h->state = TRANSPORT_STATE_RELEASED;
            return TRANSPORT_OK;
        }

        err = lli_send_apdu(h->lli, rapdu.data, rapdu.len,
                            h->cfg.apdu_timeout_ms);
        if (err != LLI_OK) {
            ESP_LOGE(TAG, "secure send failed: %d", err);
            invoke_erase(h);
            h->state = TRANSPORT_STATE_RELEASED;
            return TRANSPORT_OK;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

transport_err_t transport_run_session(transport_handle_t handle)
{
    if (!handle) {
        return TRANSPORT_ERR_INVALID_APDU;
    }

    handle->state = TRANSPORT_STATE_IDLE;

    while (1) {
        switch (handle->state) {
        case TRANSPORT_STATE_IDLE: {
            transport_err_t err = run_idle(handle);
            if (err != TRANSPORT_OK) {
                return err;
            }
            break;
        }

        case TRANSPORT_STATE_ACTIVATED: {
            transport_err_t err = run_activated(handle);
            if (err != TRANSPORT_OK) {
                return err;
            }
            break;
        }

        case TRANSPORT_STATE_HANDSHAKE: {
            transport_err_t err = run_handshake(handle);
            if (err != TRANSPORT_OK) {
                return err;
            }
            break;
        }

        case TRANSPORT_STATE_SECURE_SESSION: {
            transport_err_t err = run_secure_session(handle);
            if (err != TRANSPORT_OK) {
                return err;
            }
            break;
        }

        case TRANSPORT_STATE_RELEASED:
            lli_abort(handle->lli);
            handle->state = TRANSPORT_STATE_IDLE;
            return TRANSPORT_OK;
        }
    }
}
