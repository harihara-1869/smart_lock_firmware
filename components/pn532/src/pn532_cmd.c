/**
 * @file pn532_cmd.c
 * @brief PN532 command layer: high-level wrappers for common commands.
 *
 * Each function sends one PN532 command via pn532_send_command(), then reads
 * and validates the response via pn532_receive_response(). All functions
 * return esp_err_t.
 */

#include "pn532_cmd.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "PN532_CMD";

/* PN532 response TFI byte and the expected response command code (cmd + 1). */
#define PN532_CMD_RESPONSE_OFFSET  0x01

/* Maximum payload for TgSetData / TgGetData (per datasheet). */
#define PN532_TG_MAX_DATA_LEN      262

/* Maximum general bytes / historical bytes in TgInitAsTarget. */
#define PN532_TG_MAX_GTK_LEN        47

/* ========================================================================= */
/* GetFirmwareVersion (0x02)                                                  */
/* ========================================================================= */

#define PN532_CMD_GET_FIRMWARE_VERSION  0x02
#define PN532_RSP_GET_FIRMWARE_VERSION  0x03

esp_err_t pn532_get_firmware_version(pn532_handle_t h,
                                     pn532_firmware_version_t *out)
{
    if (h == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = pn532_send_command(h, PN532_CMD_GET_FIRMWARE_VERSION,
                                       NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[5];
    size_t len = 0;
    err = pn532_receive_response(h, buf, sizeof(buf), &len, 100);
    if (err != ESP_OK) {
        return err;
    }

    /* Response: D5 03 IC VER REV SUPPORT → buf = {0x03, IC, VER, REV, SUPPORT} */
    if (len < 5 || buf[0] != PN532_RSP_GET_FIRMWARE_VERSION) {
        ESP_LOGE(TAG, "GetFirmwareVersion: unexpected response (len=%u cmd=0x%02x)",
                 (unsigned)len, len > 0 ? buf[0] : 0);
        return ESP_FAIL;
    }

    out->ic      = buf[1];
    out->ver     = buf[2];
    out->rev     = buf[3];
    out->support = buf[4];

    if (out->ic != 0x32) {
        ESP_LOGE(TAG, "GetFirmwareVersion: unexpected IC 0x%02x (expected 0x32)",
                 out->ic);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PN532 firmware v%u.%u, IC=0x%02x, capabilities=0x%02x",
             out->ver, out->rev, out->ic, out->support);
    return ESP_OK;
}

/* ========================================================================= */
/* SAMConfiguration (0x14)                                                    */
/* ========================================================================= */

#define PN532_CMD_SAM_CONFIGURATION  0x14
#define PN532_RSP_SAM_CONFIGURATION  0x15

esp_err_t pn532_sam_configuration(pn532_handle_t h,
                                  pn532_sam_mode_t mode,
                                  uint8_t timeout_50ms,
                                  bool irq_enabled)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Parameters are positional: [Mode, Timeout, IRQ]. All three are always
     * sent — the Timeout byte is only *meaningful* in Virtual Card mode but is
     * still present on the wire, so the IRQ byte keeps its slot (byte 3). */
    uint8_t params[3];
    params[0] = (uint8_t)mode;
    params[1] = timeout_50ms;
    params[2] = irq_enabled ? 0x01 : 0x00;
    const size_t params_len = 3;

    esp_err_t err = pn532_send_command(h, PN532_CMD_SAM_CONFIGURATION,
                                       params, params_len);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[1];
    size_t len = 0;
    err = pn532_receive_response(h, buf, sizeof(buf), &len, 100);
    if (err != ESP_OK) {
        return err;
    }

    /* Response: D5 15 → buf = {0x15} (no extra data). */
    if (len < 1 || buf[0] != PN532_RSP_SAM_CONFIGURATION) {
        ESP_LOGE(TAG, "SAMConfiguration: unexpected response cmd 0x%02x",
                 len > 0 ? buf[0] : 0);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SAM configured: mode=0x%02x irq=%d", mode, irq_enabled);
    return ESP_OK;
}

/* ========================================================================= */
/* SetParameters (0x12)                                                       */
/* ========================================================================= */

#define PN532_CMD_SET_PARAMETERS  0x12
#define PN532_RSP_SET_PARAMETERS  0x13

/* RFU bits that must be zero: bit 3 and bit 7. */
#define PN532_PARAM_RFU_MASK  ((1 << 3) | (1 << 7))

esp_err_t pn532_set_parameters(pn532_handle_t h, uint8_t flags)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (flags & PN532_PARAM_RFU_MASK) {
        ESP_LOGE(TAG, "SetParameters: RFU bits set (0x%02x)", flags);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = pn532_send_command(h, PN532_CMD_SET_PARAMETERS,
                                       &flags, 1);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[1];
    size_t len = 0;
    err = pn532_receive_response(h, buf, sizeof(buf), &len, 100);
    if (err != ESP_OK) {
        return err;
    }

    if (len < 1 || buf[0] != PN532_RSP_SET_PARAMETERS) {
        ESP_LOGE(TAG, "SetParameters: unexpected response cmd 0x%02x",
                 len > 0 ? buf[0] : 0);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Parameters set: 0x%02x", flags);
    return ESP_OK;
}

/* ========================================================================= */
/* TgInitAsTarget (0x8C)                                                      */
/* ========================================================================= */

#define PN532_CMD_TG_INIT_AS_TARGET  0x8C
#define PN532_RSP_TG_INIT_AS_TARGET  0x8D

esp_err_t pn532_tg_init_as_target(pn532_handle_t h,
                                  const pn532_tg_init_params_t *params,
                                  pn532_tg_init_result_t *result,
                                  uint32_t timeout_ms)
{
    if (h == NULL || params == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (params->gt_len > PN532_TG_MAX_GTK_LEN ||
        params->tk_len > PN532_TG_MAX_GTK_LEN) {
        ESP_LOGE(TAG, "TgInitAsTarget: gt/tk too long (%u/%u)",
                 params->gt_len, params->tk_len);
        return ESP_ERR_INVALID_ARG;
    }

    /* Build the parameter buffer:
     * Mode(1) + MifareParams(6) + FeliCaParams(18) + NFCID3t(10)
     * + LenGt(1) + Gt(gt_len) + LenTk(1) + Tk(tk_len)
     */
    const size_t max_params = 1 + 6 + 18 + 10 + 1 + PN532_TG_MAX_GTK_LEN + 1 + PN532_TG_MAX_GTK_LEN;
    uint8_t buf_params[max_params];
    size_t off = 0;

    buf_params[off++] = params->mode;

    /* MifareParams: sens_res(2) + nfcid1(3) + sel_res(1) = 6 bytes */
    memcpy(&buf_params[off], params->sens_res, 2);
    off += 2;
    memcpy(&buf_params[off], params->nfcid1, 3);
    off += 3;
    buf_params[off++] = params->sel_res;

    /* FeliCaParams: nfcid2(8) + pad(8) + system_code(2) = 18 bytes */
    memcpy(&buf_params[off], params->nfcid2, 8);
    off += 8;
    memcpy(&buf_params[off], params->pad, 8);
    off += 8;
    memcpy(&buf_params[off], params->system_code, 2);
    off += 2;

    /* NFCID3t: 10 bytes */
    memcpy(&buf_params[off], params->nfcid3, 10);
    off += 10;

    /* LenGt + Gt */
    buf_params[off++] = params->gt_len;
    if (params->gt_len > 0 && params->gt != NULL) {
        memcpy(&buf_params[off], params->gt, params->gt_len);
        off += params->gt_len;
    }

    /* LenTk + Tk */
    buf_params[off++] = params->tk_len;
    if (params->tk_len > 0 && params->tk != NULL) {
        memcpy(&buf_params[off], params->tk, params->tk_len);
        off += params->tk_len;
    }

    esp_err_t err = pn532_send_command(h, PN532_CMD_TG_INIT_AS_TARGET,
                                       buf_params, off);
    if (err != ESP_OK) {
        return err;
    }

    /* This command blocks until an external initiator activates us.
     * The PN532 enters power-down waiting for RF; IRQ asserts on activation.
     * Use the caller-supplied timeout.
     *
     * The response carries the initiator's first command after the Mode byte,
     * so the buffer must be large enough to hold it — a 2-byte buffer would
     * make pn532_receive_response() fail with ESP_ERR_INVALID_SIZE as soon as
     * any initiator data is appended. */
    uint8_t rsp[2 + PN532_TG_MAX_DATA_LEN];
    size_t rsp_len = 0;
    err = pn532_receive_response(h, rsp, sizeof(rsp), &rsp_len, timeout_ms);
    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "TgInitAsTarget: no initiator within %"PRIu32" ms",
                     timeout_ms);
        }
        return err;
    }

    /* Response: D5 8D Mode [InitiatorCommand...]  → buf = {0x8D, Mode, ...}.
     * NOTE: there is NO status byte here (unlike most other commands). The byte
     * right after the response code is the Mode byte, which encodes how the
     * PN532 was activated (baud rate, DEP vs ISO14443-4 PICC, active/passive).
     * A non-zero Mode is normal, not an error.
     * Minimum valid response: cmd(1) + mode(1) = 2 bytes. */
    if (rsp_len < 2 || rsp[0] != PN532_RSP_TG_INIT_AS_TARGET) {
        ESP_LOGE(TAG, "TgInitAsTarget: unexpected response cmd 0x%02x",
                 rsp_len > 0 ? rsp[0] : 0);
        return ESP_FAIL;
    }

    result->mode = rsp[1];
    ESP_LOGI(TAG, "TgInitAsTarget: activated, mode=0x%02x", result->mode);
    return ESP_OK;
}

/* ========================================================================= */
/* TgGetTargetStatus (0x8A)                                                   */
/* ========================================================================= */

#define PN532_CMD_TG_GET_TARGET_STATUS  0x8A
#define PN532_RSP_TG_GET_TARGET_STATUS  0x8B

esp_err_t pn532_tg_get_target_status(pn532_handle_t    h,
                                     pn532_tg_status_t *out,
                                     uint32_t           timeout_ms)
{
    if (h == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = pn532_send_command(h, PN532_CMD_TG_GET_TARGET_STATUS,
                                       NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[8];
    size_t len = 0;
    err = pn532_receive_response(h, buf, sizeof(buf), &len, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    /* Response: D5 8B State BRit */
    if (len < 3 || buf[0] != PN532_RSP_TG_GET_TARGET_STATUS) {
        ESP_LOGE(TAG, "TgGetTargetStatus: unexpected response "
                 "(len=%u cmd=0x%02x raw=[%02x %02x %02x])",
                 (unsigned)len,
                 len > 0 ? buf[0] : 0,
                 len > 0 ? buf[0] : 0,
                 len > 1 ? buf[1] : 0,
                 len > 2 ? buf[2] : 0);
        return ESP_FAIL;
    }

    const uint8_t state = buf[1];
    const uint8_t brit  = buf[2];

    /* Warn on unknown state values but don't fail — future firmware may
     * add new states. */
    switch (state) {
    case PN532_TG_STATE_IDLE:
    case PN532_TG_STATE_ACTIVATED:
    case PN532_TG_STATE_DESELECTED:
    case PN532_TG_STATE_PICC_RELEASED:
    case PN532_TG_STATE_PICC_ACTIVATED:
    case PN532_TG_STATE_PICC_DESELECTED:
        break;
    default:
        ESP_LOGW(TAG, "TgGetTargetStatus: unknown state 0x%02x", state);
        break;
    }

    out->state = (pn532_tg_state_t)state;
    out->br_it = brit;

    ESP_LOGD(TAG, "PN532: TgGetTargetStatus state=0x%02x brit=0x%02x",
             state, brit);
    return ESP_OK;
}

/* ========================================================================= */
/* TgGetData (0x86)                                                           */
/* ========================================================================= */

#define PN532_CMD_TG_GET_DATA  0x86
#define PN532_RSP_TG_GET_DATA  0x87

/* Status byte bit masks for TgGetData. */
#define PN532_TG_STATUS_NAD_BIT   (1 << 7)
#define PN532_TG_STATUS_MI_BIT    (1 << 6)
#define PN532_TG_STATUS_ERR_MASK  0x3F

esp_err_t pn532_tg_get_data(pn532_handle_t h,
                            uint8_t *buf,
                            size_t buf_size,
                            size_t *out_len,
                            uint32_t timeout_ms)
{
    if (h == NULL || buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_len = 0;

    /* Temporary buffer for chaining accumulation.
     * Max DataIn is 262 bytes per response; worst case with chaining
     * we accumulate up to PN532_MAX_PAYLOAD_LEN bytes. */
    uint8_t accum[PN532_MAX_PAYLOAD_LEN];
    size_t accum_len = 0;
    bool chaining = true;

    while (chaining) {
        esp_err_t err = pn532_send_command(h, PN532_CMD_TG_GET_DATA, NULL, 0);
        if (err != ESP_OK) {
            return err;
        }

        /* Response buffer: status(1) + up to 262 data bytes + possible NAD. */
        uint8_t rsp[1 + PN532_TG_MAX_DATA_LEN + 1];
        size_t rsp_len = 0;
        err = pn532_receive_response(h, rsp, sizeof(rsp), &rsp_len, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }

        /* Response: D5 87 Status [NAD] [DataIn...] */
        if (rsp_len < 2 || rsp[0] != PN532_RSP_TG_GET_DATA) {
            ESP_LOGE(TAG, "TgGetData: unexpected response cmd 0x%02x",
                     rsp_len > 0 ? rsp[0] : 0);
            return ESP_FAIL;
        }

        const uint8_t status = rsp[1];
        const uint8_t error_code = status & PN532_TG_STATUS_ERR_MASK;

        if (error_code != PN532_ERR_NONE) {
            ESP_LOGE(TAG, "TgGetData: PN532 error 0x%02x", error_code);
            return ESP_FAIL;
        }

        /* Determine data offset: skip status byte and optional NAD byte.
         *
         * Confirmed correct per UM0701-02 §7.1: bit 7 of the status byte
         * (NADPresent) is only set when NAD usage has actually been negotiated
         * via SetParameters (fNADUsed, §7.2.9). When NAD is not enabled — the
         * usual case, and the default for this driver — the bit stays 0 and the
         * skip path is never taken, so no real data byte is ever dropped. When
         * NAD *is* enabled, the extra byte is the NAD and must be skipped. */
        size_t data_offset = 2;  /* past cmd byte and status byte */
        if (status & PN532_TG_STATUS_NAD_BIT) {
            data_offset += 1;  /* skip NAD byte */
        }

        const size_t data_len = (rsp_len > data_offset) ? (rsp_len - data_offset) : 0;

        /* Accumulate data. */
        if (accum_len + data_len > sizeof(accum)) {
            ESP_LOGE(TAG, "TgGetData: accumulated data overflow");
            return ESP_ERR_INVALID_SIZE;
        }
        if (data_len > 0) {
            memcpy(&accum[accum_len], &rsp[data_offset], data_len);
            accum_len += data_len;
        }

        chaining = (status & PN532_TG_STATUS_MI_BIT) != 0;
    }

    /* Copy accumulated data to caller buffer. */
    if (accum_len > buf_size) {
        ESP_LOGE(TAG, "TgGetData: data %u > buffer %u",
                 (unsigned)accum_len, (unsigned)buf_size);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(buf, accum, accum_len);
    *out_len = accum_len;
    return ESP_OK;
}

/* ========================================================================= */
/* TgSetData (0x8E)                                                           */
/* ========================================================================= */

#define PN532_CMD_TG_SET_DATA  0x8E
#define PN532_RSP_TG_SET_DATA  0x8F

esp_err_t pn532_tg_set_data(pn532_handle_t h,
                            const uint8_t *data,
                            size_t data_len,
                            uint32_t timeout_ms)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len > 0 && data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len > PN532_TG_MAX_DATA_LEN) {
        ESP_LOGE(TAG, "TgSetData: data_len %u > max %u",
                 (unsigned)data_len, PN532_TG_MAX_DATA_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = pn532_send_command(h, PN532_CMD_TG_SET_DATA,
                                       data, data_len);
    if (err != ESP_OK) {
        return err;
    }

    /* Response: D5 8F Status — arrives after the RF exchange completes. */
    uint8_t rsp[2];
    size_t rsp_len = 0;
    err = pn532_receive_response(h, rsp, sizeof(rsp), &rsp_len, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    if (rsp_len < 2 || rsp[0] != PN532_RSP_TG_SET_DATA) {
        ESP_LOGE(TAG, "TgSetData: unexpected response cmd 0x%02x",
                 rsp_len > 0 ? rsp[0] : 0);
        return ESP_FAIL;
    }

    const uint8_t status = rsp[1];
    if (status != PN532_ERR_NONE) {
        ESP_LOGE(TAG, "TgSetData: PN532 error 0x%02x", status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ========================================================================= */
/* InListPassiveTarget (0x4A)                                                 */
/* ========================================================================= */

#define PN532_CMD_IN_LIST_PASSIVE_TARGET  0x4A
#define PN532_RSP_IN_LIST_PASSIVE_TARGET  0x4B

esp_err_t pn532_in_list_passive_target(pn532_handle_t h,
                                       uint8_t max_targets,
                                       pn532_brty_t brty,
                                       const uint8_t *initiator_data,
                                       size_t initiator_data_len,
                                       pn532_passive_target_t *targets_out,
                                       uint8_t *num_targets_out,
                                       uint32_t timeout_ms)
{
    if (h == NULL || targets_out == NULL || num_targets_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (max_targets < 1 || max_targets > 2) {
        ESP_LOGE(TAG, "InListPassiveTarget: max_targets must be 1 or 2");
        return ESP_ERR_INVALID_ARG;
    }
    if (initiator_data_len > 0 && initiator_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *num_targets_out = 0;

    /* Build parameters: MaxTg(1) + BrTy(1) + InitiatorData(initiator_data_len) */
    const size_t params_len = 2 + initiator_data_len;
    uint8_t params[2 + PN532_MAX_PAYLOAD_LEN];
    params[0] = max_targets;
    params[1] = (uint8_t)brty;
    if (initiator_data_len > 0) {
        memcpy(&params[2], initiator_data, initiator_data_len);
    }

    esp_err_t err = pn532_send_command(h, PN532_CMD_IN_LIST_PASSIVE_TARGET,
                                       params, params_len);
    if (err != ESP_OK) {
        return err;
    }

    /* Response can be large: NbTg + per-target data (ATQA+SAK+UID+ATS). */
    uint8_t rsp[PN532_MAX_PAYLOAD_LEN];
    size_t rsp_len = 0;
    err = pn532_receive_response(h, rsp, sizeof(rsp), &rsp_len, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    if (rsp_len < 2 || rsp[0] != PN532_RSP_IN_LIST_PASSIVE_TARGET) {
        ESP_LOGE(TAG, "InListPassiveTarget: unexpected response cmd 0x%02x",
                 rsp_len > 0 ? rsp[0] : 0);
        return ESP_FAIL;
    }

    const uint8_t nb_tg = rsp[1];
    if (nb_tg == 0) {
        ESP_LOGD(TAG, "InListPassiveTarget: no targets found");
        return ESP_OK;
    }

    /* Parse target data for Type A (106 kbps ISO14443-A).
     * Response format per target:
     *   Tg(1) ATQA(2) SAK(1) NFCIDLen(1) NFCID(n) [AtsLen(1) ATS(m)]
     */
    size_t off = 2;  /* past cmd byte and NbTg */
    for (uint8_t i = 0; i < nb_tg && i < max_targets; i++) {
        if (off + 5 > rsp_len) {
            ESP_LOGE(TAG, "InListPassiveTarget: truncated target data");
            return ESP_FAIL;
        }

        pn532_passive_target_t *t = &targets_out[i];
        memset(t, 0, sizeof(*t));

        t->tg = rsp[off++];
        t->atqa[0] = rsp[off++];
        t->atqa[1] = rsp[off++];
        t->sak = rsp[off++];

        const uint8_t nfcid_len = rsp[off++];
        if (nfcid_len > sizeof(t->nfcid)) {
            ESP_LOGE(TAG, "InListPassiveTarget: NFCID too long (%u)", nfcid_len);
            return ESP_FAIL;
        }
        t->nfcid_len = nfcid_len;

        if (off + nfcid_len > rsp_len) {
            ESP_LOGE(TAG, "InListPassiveTarget: truncated NFCID");
            return ESP_FAIL;
        }
        memcpy(t->nfcid, &rsp[off], nfcid_len);
        off += nfcid_len;

        /* ATS is present if SAK bit 5 is set (ISO14443-4 compliant). */
        t->ats_len = 0;
        if ((t->sak & 0x20) && off < rsp_len) {
            const uint8_t ats_len = rsp[off++];
            t->ats_len = ats_len;
            if (off + ats_len > rsp_len) {
                ESP_LOGE(TAG, "InListPassiveTarget: truncated ATS");
                return ESP_FAIL;
            }
            memcpy(t->ats, &rsp[off], ats_len);
            off += ats_len;
        }

        ESP_LOGD(TAG, "Target %u: tg=%u SAK=0x%02x NFCID len=%u ATS len=%u",
                 i, t->tg, t->sak, t->nfcid_len, t->ats_len);
    }

    *num_targets_out = (nb_tg < max_targets) ? nb_tg : max_targets;
    return ESP_OK;
}

/* ========================================================================= */
/* TgGetInitiatorCommand (0x88)                                               */
/* ========================================================================= */

#define PN532_CMD_TG_GET_INITIATOR_COMMAND  0x88
#define PN532_RSP_TG_GET_INITIATOR_COMMAND  0x89

esp_err_t pn532_tg_get_initiator_command(pn532_handle_t h,
                                         uint8_t *buf,
                                         size_t buf_size,
                                         size_t *out_len,
                                         uint32_t timeout_ms)
{
    if (h == NULL || buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = pn532_send_command(h, PN532_CMD_TG_GET_INITIATOR_COMMAND,
                                       NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t rsp[2 + PN532_TG_MAX_DATA_LEN];
    size_t rsp_len = 0;
    err = pn532_receive_response(h, rsp, sizeof(rsp), &rsp_len, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    /* Response: D5 89 Status [DataIn...] */
    if (rsp_len < 2 || rsp[0] != PN532_RSP_TG_GET_INITIATOR_COMMAND) {
        ESP_LOGE(TAG, "TgGetInitiatorCommand: unexpected response cmd 0x%02x",
                 rsp_len > 0 ? rsp[0] : 0);
        return ESP_FAIL;
    }

    const uint8_t status = rsp[1];
    if (status != PN532_ERR_NONE) {
        ESP_LOGE(TAG, "TgGetInitiatorCommand: PN532 error 0x%02x", status);
        return ESP_FAIL;
    }

    /* Data starts at offset 2 (past cmd byte and status). */
    const size_t data_len = (rsp_len > 2) ? (rsp_len - 2) : 0;
    if (data_len > buf_size) {
        ESP_LOGE(TAG, "TgGetInitiatorCommand: data %u > buffer %u",
                 (unsigned)data_len, (unsigned)buf_size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (data_len > 0) {
        memcpy(buf, &rsp[2], data_len);
    }
    *out_len = data_len;
    return ESP_OK;
}

/* ========================================================================= */
/* TgResponseToInitiator (0x90)                                               */
/* ========================================================================= */

#define PN532_CMD_TG_RESPONSE_TO_INITIATOR  0x90
#define PN532_RSP_TG_RESPONSE_TO_INITIATOR  0x91

esp_err_t pn532_tg_response_to_initiator(pn532_handle_t h,
                                         const uint8_t *data,
                                         size_t data_len,
                                         uint32_t timeout_ms)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len > 0 && data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len > PN532_TG_MAX_DATA_LEN) {
        ESP_LOGE(TAG, "TgResponseToInitiator: data_len %u > max %u",
                 (unsigned)data_len, PN532_TG_MAX_DATA_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = pn532_send_command(h, PN532_CMD_TG_RESPONSE_TO_INITIATOR,
                                       data, data_len);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t rsp[2];
    size_t rsp_len = 0;
    err = pn532_receive_response(h, rsp, sizeof(rsp), &rsp_len, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    if (rsp_len < 2 || rsp[0] != PN532_RSP_TG_RESPONSE_TO_INITIATOR) {
        ESP_LOGE(TAG, "TgResponseToInitiator: unexpected response cmd 0x%02x",
                 rsp_len > 0 ? rsp[0] : 0);
        return ESP_FAIL;
    }

    const uint8_t status = rsp[1];
    if (status != PN532_ERR_NONE) {
        ESP_LOGE(TAG, "TgResponseToInitiator: PN532 error 0x%02x", status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ========================================================================= */
/* TgSetMetaData (0x94)                                                       */
/* ========================================================================= */

#define PN532_CMD_TG_SET_META_DATA  0x94
#define PN532_RSP_TG_SET_META_DATA  0x95

esp_err_t pn532_tg_set_meta_data(pn532_handle_t h,
                                 const uint8_t *data,
                                 size_t data_len,
                                 uint32_t timeout_ms)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len > 0 && data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len > PN532_TG_MAX_DATA_LEN) {
        ESP_LOGE(TAG, "TgSetMetaData: data_len %u > max %u",
                 (unsigned)data_len, PN532_TG_MAX_DATA_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = pn532_send_command(h, PN532_CMD_TG_SET_META_DATA,
                                       data, data_len);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t rsp[2];
    size_t rsp_len = 0;
    err = pn532_receive_response(h, rsp, sizeof(rsp), &rsp_len, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    if (rsp_len < 2 || rsp[0] != PN532_RSP_TG_SET_META_DATA) {
        ESP_LOGE(TAG, "TgSetMetaData: unexpected response cmd 0x%02x",
                 rsp_len > 0 ? rsp[0] : 0);
        return ESP_FAIL;
    }

    const uint8_t status = rsp[1];
    if (status != PN532_ERR_NONE) {
        ESP_LOGE(TAG, "TgSetMetaData: PN532 error 0x%02x", status);
        return ESP_FAIL;
    }

    return ESP_OK;
}
