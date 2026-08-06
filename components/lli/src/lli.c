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

#ifndef MOCK_LLI_FOR_TESTING

#include "lli.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "pn532.h"
#include "pn532_cmd.h"
#include "pn532_i2c.h"

static const char *TAG = "LLI";

struct lli_t {
    pn532_handle_t pn532;
    void           *i2c_ctx;
    pn532_transport_ops_t ops;

    /* Card identity copied from config for lli_activate. */
    uint8_t sens_res[2];
    uint8_t nfcid1[3];
    uint8_t sel_res;
    uint8_t nfcid2[8];
    uint8_t pad[8];
    uint8_t system_code[2];
    uint8_t nfcid3t[10];
    uint8_t gt[47];
    uint8_t gt_len;
    uint8_t tk[47];
    uint8_t tk_len;
};

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Map an esp_err_t from a pn532 transport/core call to an lli_err_t.
 *
 * The driver surfaces a distinct fatal ESP_ERR_INVALID_STATE when the I2C
 * controller is wedged beyond recovery (see pn532.h write op doc). That must
 * propagate as LLI_ERR_BUS_FATAL so upper layers stop retrying; every other
 * failure falls back to @p fallback (the caller keeps its existing
 * TIMEOUT/etc. special-cases).
 */
static lli_err_t lli_err_from_esp(esp_err_t err, lli_err_t fallback)
{
    if (err == ESP_ERR_INVALID_STATE) {
        return LLI_ERR_BUS_FATAL;
    }
    return fallback;
}

/**
 * @brief Apply the PN532 configuration required for ISO-DEP card emulation.
 *
 * Safe to call repeatedly (idempotent). Re-applies configuration to recover
 * from silent PN532 hardware resets that the LLI layer cannot detect.
 */
static lli_err_t configure_pn532(struct lli_t *h)
{
    /* SAM configuration — Normal mode, IRQ enabled. */
    esp_err_t err = pn532_sam_configuration(h->pn532, PN532_SAM_NORMAL, 0, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pn532_sam_configuration failed: %s", esp_err_to_name(err));
        return lli_err_from_esp(err, LLI_ERR_INTERNAL);
    }

    /* Communication parameters — auto ATR_RES + ISO14443-4 PICC mode. */
    err = pn532_set_parameters(h->pn532,
                               PN532_PARAM_AUTO_ATR_RES |
                               PN532_PARAM_ISO14443_4_PICC);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pn532_set_parameters failed: %s", esp_err_to_name(err));
        return lli_err_from_esp(err, LLI_ERR_INTERNAL);
    }

    return LLI_OK;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

lli_err_t lli_init(const lli_config_t *cfg, lli_handle_t *handle_out)
{
    if (!cfg || !handle_out) {
        return LLI_ERR_INVALID_ARG;
    }

    if (cfg->gt_len > 47 || cfg->tk_len > 47) {
        return LLI_ERR_INVALID_ARG;
    }

    struct lli_t *h = calloc(1, sizeof(*h));
    if (!h) {
        return LLI_ERR_INTERNAL;
    }

    /* Configure-PN532 result, used by the cleanup path below (initialised so
     * a goto that jumps over its assignment still reads a valid value). */
    lli_err_t cfg_err = LLI_OK;

    /* Step 1 — create I2C transport. */
    pn532_i2c_config_t i2c_cfg = {
        .sda_gpio  = cfg->sda_gpio,
        .scl_gpio  = cfg->scl_gpio,
        .irq_gpio  = cfg->irq_gpio,
        .rst_gpio  = cfg->rst_gpio,
        .port      = cfg->i2c_port,
        .clk_speed = cfg->i2c_clk_hz,
    };

    esp_err_t err = pn532_i2c_create(&i2c_cfg, &h->ops, &h->i2c_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pn532_i2c_create failed: %s", esp_err_to_name(err));
        free(h);
        return lli_err_from_esp(err, LLI_ERR_INTERNAL);
    }

    /* Step 2 — create core driver handle. */
    pn532_config_t drv_cfg = {
        .ops           = &h->ops,
        .transport_ctx = h->i2c_ctx,
    };

    err = pn532_init(&drv_cfg, &h->pn532);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pn532_init failed: %s", esp_err_to_name(err));
        pn532_i2c_destroy(h->i2c_ctx);
        free(h);
        return lli_err_from_esp(err, LLI_ERR_INTERNAL);
    }

    /* Step 3 — wake the chip. */
    err = pn532_wakeup(h->pn532);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pn532_wakeup failed: %s", esp_err_to_name(err));
        goto cleanup_deinit;
    }

    /* Step 4 — firmware version sanity check (before any configuration). */
    pn532_firmware_version_t fw;
    err = pn532_get_firmware_version(h->pn532, &fw);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pn532_get_firmware_version failed: %s",
                 esp_err_to_name(err));
        goto cleanup_deinit;
    }
    if (fw.ic != 0x32) {
        ESP_LOGE(TAG, "unexpected IC code 0x%02X (expected 0x32)", fw.ic);
        err = ESP_FAIL;
        goto cleanup_deinit;
    }
    ESP_LOGI(TAG, "PN532 v%d.%d (IC=0x%02X, caps=0x%02X)",
             fw.ver, fw.rev, fw.ic, fw.support);

    /* Step 5 + 6 — configure PN532 for ISO-DEP card emulation. */
    cfg_err = configure_pn532(h);
    if (cfg_err != LLI_OK) {
        goto cleanup_deinit;
    }

    /* Step 7 — cache card identity. */
    memcpy(h->sens_res,    cfg->sens_res,    sizeof(h->sens_res));
    memcpy(h->nfcid1,      cfg->nfcid1,      sizeof(h->nfcid1));
    h->sel_res = cfg->sel_res;
    memcpy(h->nfcid2,      cfg->nfcid2,      sizeof(h->nfcid2));
    memcpy(h->pad,         cfg->pad,         sizeof(h->pad));
    memcpy(h->system_code, cfg->system_code, sizeof(h->system_code));
    memcpy(h->nfcid3t,     cfg->nfcid3t,     sizeof(h->nfcid3t));
    h->gt_len = cfg->gt_len;
    if (cfg->gt_len > 0) {
        memcpy(h->gt, cfg->gt, cfg->gt_len);
    }
    h->tk_len = cfg->tk_len;
    if (cfg->tk_len > 0) {
        memcpy(h->tk, cfg->tk, cfg->tk_len);
    }

    *handle_out = h;
    return LLI_OK;

cleanup_deinit:
    pn532_deinit(h->pn532);
    pn532_i2c_destroy(h->i2c_ctx);
    free(h);
    if (cfg_err != LLI_OK) {
        /* configure_pn532 already produced an lli_err_t (may be BUS_FATAL). */
        return cfg_err;
    }
    return lli_err_from_esp(err, LLI_ERR_INTERNAL);
}

lli_err_t lli_deinit(lli_handle_t handle)
{
    if (!handle) {
        return LLI_OK;
    }
    pn532_deinit(handle->pn532);
    pn532_i2c_destroy(handle->i2c_ctx);
    free(handle);
    return LLI_OK;
}

lli_err_t lli_activate(lli_handle_t handle, uint32_t timeout_ms)
{
    if (!handle) {
        return LLI_ERR_INVALID_ARG;
    }

    /* Re-apply PN532 configuration in case it was silently reset. */
    lli_err_t err = configure_pn532(handle);
    if (err != LLI_OK) {
        return err;
    }

    pn532_tg_init_params_t params = {
        .sens_res    = {handle->sens_res[0], handle->sens_res[1]},
        .nfcid1      = {handle->nfcid1[0], handle->nfcid1[1], handle->nfcid1[2]},
        .sel_res     = handle->sel_res,
        .nfcid2      = {0}, /* filled below */
        .pad         = {0},
        .system_code = {0},
        .nfcid3      = {0},
        .gt          = (handle->gt_len > 0) ? handle->gt : NULL,
        .gt_len      = handle->gt_len,
        .tk          = (handle->tk_len > 0) ? handle->tk : NULL,
        .tk_len      = handle->tk_len,
        /* Passive PICC only — reject active-mode DEP at the PN532 level. */
        .mode        = PN532_TG_MODE_PICC_ONLY |
                       PN532_TG_MODE_PASSIVE_ONLY,
    };
    memcpy(params.nfcid2,      handle->nfcid2,      sizeof(handle->nfcid2));
    memcpy(params.pad,         handle->pad,         sizeof(handle->pad));
    memcpy(params.system_code, handle->system_code, sizeof(handle->system_code));
    memcpy(params.nfcid3,      handle->nfcid3t,     sizeof(handle->nfcid3t));

    pn532_tg_init_result_t result;
    esp_err_t pn532_err = pn532_tg_init_as_target(handle->pn532,
                                                   &params, &result, timeout_ms);
    if (pn532_err == ESP_ERR_TIMEOUT) {
        return LLI_ERR_TIMEOUT;
    }
    if (pn532_err != ESP_OK) {
        ESP_LOGE(TAG, "pn532_tg_init_as_target failed: %s",
                 esp_err_to_name(pn532_err));
        return lli_err_from_esp(pn532_err, LLI_ERR_INTERNAL);
    }

    return LLI_OK;
}

lli_err_t lli_receive_apdu(lli_handle_t handle, uint8_t *buf, size_t buf_len,
                           size_t *len_out, uint32_t timeout_ms)
{
    if (!handle || !buf || !len_out) {
        return LLI_ERR_INVALID_ARG;
    }

    /*
     * Use the raw command layer so we can inspect the PN532 status byte
     * directly. pn532_tg_get_data() abstracts it away.
     *
     * Command 0x86 (TgGetData) → response 0x87 + status + data.
     *
     * No MI-bit (More Information) fragment reassembly: the current protocol
     * guarantees every Short APDU (max 261 bytes) fits in one PN532 frame
     * (262-byte payload). Revisit if the protocol adds application-layer
     * chaining or increases the maximum APDU size.
     */
    esp_err_t err = pn532_send_command(handle->pn532, 0x86, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TgGetData send failed: %s", esp_err_to_name(err));
        *len_out = 0;
        return (err == ESP_ERR_TIMEOUT) ? LLI_ERR_TIMEOUT
                                        : lli_err_from_esp(err, LLI_ERR_INTERNAL);
    }

    uint8_t resp[PN532_MAX_PAYLOAD_LEN];
    size_t  resp_len = 0;
    err = pn532_receive_response(handle->pn532,
                                 resp, sizeof(resp), &resp_len, timeout_ms);
    if (err == ESP_ERR_TIMEOUT) {
        *len_out = 0;
        return LLI_ERR_TIMEOUT;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TgGetData receive failed: %s", esp_err_to_name(err));
        *len_out = 0;
        return lli_err_from_esp(err, LLI_ERR_FRAME_INTEGRITY);
    }

    /* Response layout: resp[0] = 0x87 (cmd), resp[1] = status, rest = data. */
    if (resp_len < 2 || resp[0] != 0x87) {
        ESP_LOGE(TAG, "TgGetData unexpected response code 0x%02X len=%u",
                 resp[0], (unsigned)resp_len);
        *len_out = 0;
        return LLI_ERR_FRAME_INTEGRITY;
    }

    uint8_t status = resp[1];
    if (status != 0x00) {
        *len_out = 0;
        if (status == 0x01) {
            ESP_LOGW(TAG, "TgGetData status=0x01 (timeout)");
            return LLI_ERR_TIMEOUT;
        }
        if (status == 0x29) {
            ESP_LOGW(TAG, "TgGetData status=0x29 (target released)");
            return LLI_ERR_LINK_RELEASED;
        }
        ESP_LOGW(TAG, "TgGetData status=0x%02X (frame integrity)", status);
        return LLI_ERR_FRAME_INTEGRITY;
    }

    /* Success — copy payload (bytes after status). */
    size_t data_len = resp_len - 2;
    if (data_len > buf_len) {
        ESP_LOGE(TAG, "TgGetData payload %u > buf_len %u",
                 (unsigned)data_len, (unsigned)buf_len);
        return LLI_ERR_FRAME_INTEGRITY;
    }

    if (data_len > 0) {
        memcpy(buf, &resp[2], data_len);
    }
    *len_out = data_len;
    return LLI_OK;
}

lli_err_t lli_send_apdu(lli_handle_t handle, const uint8_t *data,
                        size_t len, uint32_t timeout_ms)
{
    if (!handle) {
        return LLI_ERR_INVALID_ARG;
    }

    esp_err_t err = pn532_tg_set_data(handle->pn532, data, len, timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pn532_tg_set_data failed: %s", esp_err_to_name(err));
        return lli_err_from_esp(err, LLI_ERR_SEND_FAILED);
    }

    return LLI_OK;
}

lli_link_status_t lli_get_link_status(lli_handle_t handle)
{
    if (!handle) {
        return LLI_STATUS_ERROR;
    }

    pn532_tg_status_t status;
    esp_err_t err = pn532_tg_get_target_status(handle->pn532, &status, 100);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "pn532_tg_get_target_status failed: %s",
                 esp_err_to_name(err));
        return LLI_STATUS_ERROR;
    }

    switch (status.state) {
    case PN532_TG_STATE_PICC_ACTIVATED:
    case PN532_TG_STATE_PICC_DESELECTED:
        return LLI_STATUS_ACTIVE;
    case PN532_TG_STATE_PICC_RELEASED:
    case PN532_TG_STATE_IDLE:
        return LLI_STATUS_RELEASED;
    default:
        ESP_LOGW(TAG, "unknown target state 0x%02X", status.state);
        return LLI_STATUS_ERROR;
    }
}

lli_err_t lli_abort(lli_handle_t handle)
{
    if (!handle) {
        return LLI_ERR_INVALID_ARG;
    }

    /* Step 1 — best-effort: send ACK to abort any in-progress command. */
    esp_err_t err = pn532_send_ack(handle->pn532);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "pn532_send_ack (best-effort) failed: %s",
                 esp_err_to_name(err));
    }

    /* Step 2 — best-effort: release all targets (InRelease, Tg=0x00). */
    uint8_t rel_param = 0x00;
    err = pn532_send_command(handle->pn532, 0x52, &rel_param, 1);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "InRelease send (best-effort) failed: %s",
                 esp_err_to_name(err));
    } else {
        uint8_t resp[16];
        size_t  resp_len = 0;
        err = pn532_receive_response(handle->pn532,
                                     resp, sizeof(resp), &resp_len, 1000);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "InRelease response (best-effort) failed: %s",
                     esp_err_to_name(err));
        }
    }

    /* Step 3 — chip may have entered Power Down after release; guarantee
     * it is awake for the next lli_activate call. */
    err = pn532_wakeup(handle->pn532);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "pn532_wakeup (best-effort) failed: %s",
                 esp_err_to_name(err));
    }

    return LLI_OK;
}

#endif /* MOCK_LLI_FOR_TESTING */
