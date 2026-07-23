/**
 * @file pn532.c
 * @brief PN532 core: frame builder, frame parser, and command/response logic.
 *
 * Transport-agnostic. All bus access goes through the @ref pn532_transport_ops_t
 * vtable supplied at init time. This layer knows the PN532 frame format and the
 * command -> ACK -> response handshake, nothing about I2C/SPI/UART specifics.
 *
 * Frame reference (host -> PN532, "normal" frame):
 *   PREAMBLE(0x00) START(0x00 0xFF) LEN LCS TFI(0xD4) DATA... DCS POSTAMBLE(0x00)
 * where LEN = 1 + len(DATA), LCS = (~LEN + 1), DCS = (~(TFI + sum(DATA)) + 1).
 */

#include "pn532.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

static const char *TAG = "PN532";

/* ---- Frame constants ---------------------------------------------------- */

#define PN532_PREAMBLE          0x00
#define PN532_STARTCODE1        0x00
#define PN532_STARTCODE2        0xFF
#define PN532_POSTAMBLE         0x00

#define PN532_TFI_HOST_TO_PN532 0xD4  /* frames we send */
#define PN532_TFI_PN532_TO_HOST 0xD5  /* frames we receive */

/* LEN value 0xFF marks an extended information frame (LEN follows as 2 bytes). */
#define PN532_EXTENDED_MARKER   0xFF

/* Largest "normal" frame LEN (TFI + DATA). Above this an extended frame is used. */
#define PN532_NORMAL_LEN_MAX    0xFE

/*
 * Worst-case outgoing buffer:
 *   preamble(1) + startcode(2) + extended-marker(2) + len_hi/len_lo(2) + lcs(1)
 *   + tfi(1) + data(PN532_MAX_PAYLOAD_LEN-1) + dcs(1) + postamble(1)
 * PN532_MAX_PAYLOAD_LEN already counts the TFI, hence (LEN-1) DATA bytes here.
 */
#define PN532_TX_BUF_SIZE       (11 + (PN532_MAX_PAYLOAD_LEN - 1))

/*
 * Worst-case incoming read (single transaction), all as an over-read that
 * starts with the I2C status byte:
 *   status(1) + preamble(1) + startcode(2) + extended-marker(2) + len(2)
 *   + lcs(1) + tfi(1) + data(PN532_MAX_PAYLOAD_LEN-1) + dcs(1) + postamble(1)
 */
#define PN532_RX_BUF_SIZE       (12 + (PN532_MAX_PAYLOAD_LEN - 1))

/* 6-byte ACK / NACK handshake frames (fixed patterns). */
static const uint8_t PN532_ACK_FRAME[6]  = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
static const uint8_t PN532_NACK_FRAME[6] = {0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00};

/* Default time budget for the ACK handshake after a command is written. */
#define PN532_ACK_TIMEOUT_MS    100U

/* ---- Handle ------------------------------------------------------------- */

struct pn532_t {
    const pn532_transport_ops_t *ops;
    void *ctx;
};

/* Convenience wrappers around the transport vtable. */
static inline esp_err_t tp_write(pn532_handle_t h, const uint8_t *b, size_t n)
{
    return h->ops->write(h->ctx, b, n);
}
static inline esp_err_t tp_read_frame(pn532_handle_t h, uint8_t *b, size_t n)
{
    return h->ops->read_frame(h->ctx, b, n);
}
static inline esp_err_t tp_wait_ready(pn532_handle_t h, uint32_t timeout_ms)
{
    return h->ops->wait_ready(h->ctx, timeout_ms);
}

/* ------------------------------------------------------------------------- */
/* Frame builder                                                              */
/* ------------------------------------------------------------------------- */

/**
 * @brief Serialise a command into a normal or extended information frame.
 *
 * @param cmd_code    Command byte (first DATA byte).
 * @param params      Parameter bytes (may be NULL when @p params_len == 0).
 * @param params_len  Number of parameter bytes.
 * @param out         Destination buffer (>= PN532_TX_BUF_SIZE).
 * @param out_len     Receives the number of bytes written.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if the payload exceeds the max.
 */
static esp_err_t pn532_build_frame(uint8_t cmd_code,
                                   const uint8_t *params, size_t params_len,
                                   uint8_t *out, size_t *out_len)
{
    /* DATA = cmd_code + params; TFI is counted separately in LEN. */
    const size_t data_len = 1U + params_len;      /* cmd + params            */
    const size_t info_len = 1U + data_len;        /* TFI + DATA (the "LEN")  */

    if (info_len > PN532_MAX_PAYLOAD_LEN) {
        ESP_LOGE(TAG, "payload too large: %u > %u",
                 (unsigned)info_len, (unsigned)PN532_MAX_PAYLOAD_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    size_t i = 0;
    out[i++] = PN532_PREAMBLE;
    out[i++] = PN532_STARTCODE1;
    out[i++] = PN532_STARTCODE2;

    if (info_len > PN532_NORMAL_LEN_MAX) {
        /* Extended frame: 0xFF 0xFF marker, then 2-byte big-endian length. */
        const uint8_t len_hi = (uint8_t)((info_len >> 8) & 0xFF);
        const uint8_t len_lo = (uint8_t)(info_len & 0xFF);
        out[i++] = PN532_EXTENDED_MARKER;
        out[i++] = PN532_EXTENDED_MARKER;
        out[i++] = len_hi;
        out[i++] = len_lo;
        out[i++] = (uint8_t)((~(len_hi + len_lo) + 1) & 0xFF);  /* LCS */
    } else {
        /* Normal frame: single-byte LEN + its checksum. */
        const uint8_t len = (uint8_t)info_len;
        out[i++] = len;
        out[i++] = (uint8_t)((~len + 1) & 0xFF);                /* LCS */
    }

    /* TFI + DATA, accumulating the data checksum as we go. */
    uint8_t sum = PN532_TFI_HOST_TO_PN532;
    out[i++] = PN532_TFI_HOST_TO_PN532;
    out[i++] = cmd_code;
    sum += cmd_code;
    for (size_t p = 0; p < params_len; p++) {
        out[i++] = params[p];
        sum += params[p];
    }

    out[i++] = (uint8_t)((~sum + 1) & 0xFF);  /* DCS */
    out[i++] = PN532_POSTAMBLE;

    *out_len = i;
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Frame parser                                                               */
/* ------------------------------------------------------------------------- */

/**
 * @brief Locate the start-of-frame (0x00 0xFF) after the status byte.
 *
 * The chip may emit leading 0x00 preamble padding; scan for the 0x00 0xFF
 * start code. @p buf points at the first byte AFTER the I2C status byte.
 *
 * @param buf   Frame bytes (status byte already stripped).
 * @param len   Number of valid bytes in @p buf.
 * @param off   Receives the index just past the 0x00 0xFF start code.
 * @return ESP_OK if found, ESP_ERR_INVALID_RESPONSE otherwise.
 */
static esp_err_t pn532_find_start(const uint8_t *buf, size_t len, size_t *off)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == PN532_STARTCODE1 && buf[i + 1] == PN532_STARTCODE2) {
            *off = i + 2;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_RESPONSE;
}

/**
 * @brief Check whether a received buffer matches a fixed 6-byte handshake frame.
 *
 * Handles optional leading preamble by locating the 0x00 0xFF start code and
 * comparing the 3 bytes that follow it against @p pattern[2..4].
 */
static bool pn532_match_fixed(const uint8_t *buf, size_t len, const uint8_t *pattern)
{
    size_t off = 0;
    if (pn532_find_start(buf, len, &off) != ESP_OK) {
        return false;
    }
    /* off points just past 0x00 0xFF; pattern[3], [4], [5] follow. */
    if (off + 3 > len) {
        return false;
    }
    return buf[off] == pattern[3] &&
           buf[off + 1] == pattern[4] &&
           buf[off + 2] == pattern[5];
}

/* Error frame from PN532: 0x00 0x00 0xFF 0x01 0xFF 0x7F 0x81 0x00. */
static bool pn532_is_error_frame(const uint8_t *buf, size_t len)
{
    size_t off = 0;
    if (pn532_find_start(buf, len, &off) != ESP_OK) {
        return false;
    }
    /* After 0x00 0xFF: LEN=0x01, LCS=0xFF, then error TFI 0x7F. */
    return (off + 3 <= len) &&
           buf[off] == 0x01 && buf[off + 1] == 0xFF && buf[off + 2] == 0x7F;
}

/**
 * @brief Parse a received information frame and extract its DATA payload.
 *
 * @param raw       Bytes read from the transport INCLUDING the leading status
 *                  byte at raw[0].
 * @param raw_len   Number of valid bytes in @p raw.
 * @param out       Destination for the DATA payload (response cmd + params).
 * @param out_size  Capacity of @p out.
 * @param out_len   Receives the number of payload bytes written.
 * @return ESP_OK / ESP_ERR_INVALID_CRC / ESP_ERR_INVALID_SIZE /
 *         ESP_ERR_INVALID_RESPONSE / ESP_FAIL (error frame).
 */
static esp_err_t pn532_parse_frame(const uint8_t *raw, size_t raw_len,
                                   uint8_t *out, size_t out_size, size_t *out_len)
{
    /* Skip the status byte; everything below is relative to the frame body. */
    const uint8_t *buf = raw + 1;
    const size_t len = (raw_len > 0) ? raw_len - 1 : 0;

    if (pn532_is_error_frame(buf, len)) {
        ESP_LOGE(TAG, "PN532 application error frame (0x7F)");
        return ESP_FAIL;
    }

    size_t off = 0;
    esp_err_t err = pn532_find_start(buf, len, &off);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no start code in response");
        return err;
    }

    /* Need at least LEN + LCS after the start code. */
    if (off + 2 > len) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t info_len;   /* LEN: number of TFI+DATA bytes */
    uint8_t lcs;
    if (buf[off] == PN532_EXTENDED_MARKER && (off + 1 < len) &&
        buf[off + 1] == PN532_EXTENDED_MARKER) {
        /* Extended frame: 0xFF 0xFF, LEN_H, LEN_L, LCS. */
        off += 2;
        if (off + 3 > len) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint8_t len_hi = buf[off];
        const uint8_t len_lo = buf[off + 1];
        lcs = buf[off + 2];
        off += 3;
        info_len = ((size_t)len_hi << 8) | len_lo;
        if (((len_hi + len_lo + lcs) & 0xFF) != 0) {
            ESP_LOGE(TAG, "extended LCS mismatch");
            return ESP_ERR_INVALID_CRC;
        }
    } else {
        /* Normal frame: LEN, LCS. */
        info_len = buf[off];
        lcs = buf[off + 1];
        off += 2;
        if (((info_len + lcs) & 0xFF) != 0) {
            ESP_LOGE(TAG, "LCS mismatch (len=0x%02x lcs=0x%02x)",
                     (unsigned)info_len, lcs);
            return ESP_ERR_INVALID_CRC;
        }
    }

    if (info_len < 1) {
        ESP_LOGE(TAG, "info length zero");
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Body is TFI + DATA (info_len bytes) followed by DCS. */
    if (off + info_len + 1 > len) {
        ESP_LOGE(TAG, "truncated frame: need %u have %u",
                 (unsigned)(off + info_len + 1), (unsigned)len);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t tfi = buf[off];
    if (tfi != PN532_TFI_PN532_TO_HOST) {
        ESP_LOGE(TAG, "unexpected TFI 0x%02x (want 0x%02x)",
                 tfi, PN532_TFI_PN532_TO_HOST);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Validate DCS over TFI + DATA. */
    uint8_t sum = 0;
    for (size_t k = 0; k < info_len; k++) {
        sum += buf[off + k];
    }
    const uint8_t dcs = buf[off + info_len];
    if (((sum + dcs) & 0xFF) != 0) {
        ESP_LOGE(TAG, "DCS mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    /* DATA payload = info_len - 1 bytes (drop the TFI). */
    const size_t data_len = info_len - 1;
    if (data_len > out_size) {
        ESP_LOGE(TAG, "response %u > buffer %u",
                 (unsigned)data_len, (unsigned)out_size);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, &buf[off + 1], data_len);
    *out_len = data_len;
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* ACK handshake                                                              */
/* ------------------------------------------------------------------------- */

/**
 * @brief Wait for readiness and read/validate the 6-byte ACK frame.
 */
static esp_err_t pn532_read_ack(pn532_handle_t h, uint32_t timeout_ms)
{
    esp_err_t err = tp_wait_ready(h, timeout_ms);
    if (err != ESP_OK) {
        return err;  /* ESP_ERR_TIMEOUT propagates as-is */
    }

    /* status byte + 6 ACK bytes read in one transaction. */
    uint8_t raw[1 + sizeof(PN532_ACK_FRAME)];
    err = tp_read_frame(h, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    if (!pn532_match_fixed(raw + 1, sizeof(raw) - 1, PN532_ACK_FRAME)) {
        ESP_LOGE(TAG, "expected ACK, got malformed frame");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Frame resync                                                               */
/* ------------------------------------------------------------------------- */

/** Maximum bytes to consume during resync before giving up. */
#define PN532_RESYNC_MAX_BYTES  512

/**
 * @brief Drain incoming bytes until a valid preamble sequence is found.
 *
 * Reads one byte at a time using tp_read_frame (single-byte over-reads),
 * maintaining a 3-byte sliding window looking for {0x00, 0x00, 0xFF}.
 *
 * @param h           Driver handle.
 * @param timeout_ms  Maximum time to wait.
 * @return ESP_OK if preamble found; ESP_FAIL if 512 bytes exhausted;
 *         ESP_ERR_TIMEOUT if timeout elapsed.
 */
static esp_err_t resync_frame(pn532_handle_t h, uint32_t timeout_ms)
{
    uint8_t window[3] = {0};
    size_t consumed = 0;

    ESP_LOGW(TAG, "attempting frame resync");

    while (consumed < PN532_RESYNC_MAX_BYTES) {
        esp_err_t err = tp_wait_ready(h, timeout_ms);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "frame resync timed out after %"PRIu32" ms", timeout_ms);
            return err;
        }

        /* Read status byte + 1 frame byte (2 bytes total). */
        uint8_t raw[2];
        err = tp_read_frame(h, raw, sizeof(raw));
        if (err != ESP_OK) {
            return err;
        }

        /* Shift window and add new byte. */
        window[0] = window[1];
        window[1] = window[2];
        window[2] = raw[1];  /* raw[0] is status, raw[1] is data */
        consumed++;

        /* Check for preamble: 0x00 0x00 0xFF */
        if (window[0] == 0x00 && window[1] == 0x00 && window[2] == 0xFF) {
            ESP_LOGI(TAG, "frame resync succeeded after %u bytes", (unsigned)consumed);
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "resync exhausted %d bytes without finding preamble",
             PN532_RESYNC_MAX_BYTES);
    return ESP_FAIL;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

esp_err_t pn532_init(const pn532_config_t *cfg, pn532_handle_t *out_handle)
{
    if (cfg == NULL || cfg->ops == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* A usable transport must provide every operation the core relies on. */
    if (cfg->ops->write == NULL || cfg->ops->read_frame == NULL ||
        cfg->ops->wait_ready == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct pn532_t *h = calloc(1, sizeof(*h));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }
    h->ops = cfg->ops;
    h->ctx = cfg->transport_ctx;

    *out_handle = h;
    ESP_LOGI(TAG, "driver initialised");
    return ESP_OK;
}

void pn532_deinit(pn532_handle_t handle)
{
    if (handle == NULL) {
        return;
    }
    if (handle->ops != NULL && handle->ops->destroy != NULL) {
        handle->ops->destroy(handle->ctx);
    }
    free(handle);
}

esp_err_t pn532_send_command(pn532_handle_t h,
                             uint8_t cmd_code,
                             const uint8_t *params, size_t params_len)
{
    if (h == NULL || (params == NULL && params_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t frame[PN532_TX_BUF_SIZE];
    size_t frame_len = 0;
    esp_err_t err = pn532_build_frame(cmd_code, params, params_len,
                                      frame, &frame_len);
    if (err != ESP_OK) {
        return err;
    }

    err = tp_write(h, frame, frame_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "command write failed: %s", esp_err_to_name(err));
        return err;
    }

    /* The chip acknowledges receipt with an ACK frame before it computes the
     * response; wait for and validate it here. */
    err = pn532_read_ack(h, PN532_ACK_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no ACK for cmd 0x%02x: %s", cmd_code, esp_err_to_name(err));
    }
    return err;
}

esp_err_t pn532_receive_response(pn532_handle_t h,
                                 uint8_t *buf, size_t buf_size,
                                 size_t *out_len,
                                 uint32_t timeout_ms)
{
    if (h == NULL || buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = tp_wait_ready(h, timeout_ms);
    if (err != ESP_OK) {
        return err;  /* ESP_ERR_TIMEOUT */
    }

    /* Over-read status + worst-case frame in one transaction. The transport
     * reads exactly this many bytes; trailing bytes beyond the real frame are
     * ignored by the parser (it stops at DCS). */
    uint8_t raw[PN532_RX_BUF_SIZE];
    err = tp_read_frame(h, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    err = pn532_parse_frame(raw, sizeof(raw), buf, buf_size, out_len);
    if (err == ESP_ERR_INVALID_CRC) {
        /* Checksum mismatch — attempt frame resync before giving up. */
        ESP_LOGW(TAG, "checksum mismatch — attempting frame resync");
        esp_err_t resync_err = resync_frame(h, timeout_ms);
        if (resync_err == ESP_OK) {
            /* Resync succeeded — retry the receive exactly once. */
            ESP_LOGI(TAG, "resync succeeded, retrying receive");
            err = tp_wait_ready(h, timeout_ms);
            if (err == ESP_OK) {
                err = tp_read_frame(h, raw, sizeof(raw));
                if (err == ESP_OK) {
                    err = pn532_parse_frame(raw, sizeof(raw), buf, buf_size, out_len);
                }
            }
        }
        /* If resync failed or retry still fails, return original CRC error. */
        if (err == ESP_ERR_INVALID_CRC) {
            ESP_LOGW(TAG, "checksum error, sending NACK to request retransmit");
            (void)tp_write(h, PN532_NACK_FRAME, sizeof(PN532_NACK_FRAME));
        }
    }
    return err;
}

esp_err_t pn532_wakeup(pn532_handle_t h)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* A short burst of 0x55 bytes wakes the chip from Power-Down / LowVbat.
     * The write may NACK while the chip is still asleep; the transport already
     * retries the address, and we then wait out T_osc_start regardless. */
    static const uint8_t wake[] = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
    esp_err_t err = tp_write(h, wake, sizeof(wake));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wakeup write returned %s (chip may still wake)",
                 esp_err_to_name(err));
    }

    /* T_osc_start: typically a few hundred us, up to ~2 ms depending on the
     * quartz and board. Wait the worst case before the first real command. */
    vTaskDelay(pdMS_TO_TICKS(2));
    return err;
}

esp_err_t pn532_send_ack(pn532_handle_t h)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = tp_write(h, PN532_ACK_FRAME, sizeof(PN532_ACK_FRAME));
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "PN532: ACK sent");
    }
    return err;
}

esp_err_t pn532_reset(pn532_handle_t h, uint32_t pulse_ms, uint32_t settle_ms)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Check if transport supports reset. */
    if (h->ops->reset_device == NULL) {
        ESP_LOGW(TAG, "transport has no reset capability");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Acquire bus mutex to prevent I2C transactions during reset. */
    if (h->ops->bus_lock != NULL) {
        esp_err_t err = h->ops->bus_lock(h->ctx, 5000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to acquire bus mutex for reset: %s",
                     esp_err_to_name(err));
            return err;
        }
    }

    /* Assert reset. */
    esp_err_t err = h->ops->reset_device(h->ctx, pulse_ms, settle_ms);

    /* Release bus mutex. */
    if (h->ops->bus_unlock != NULL) {
        h->ops->bus_unlock(h->ctx);
    }

    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "transport has no reset pin configured");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "hardware reset failed: %s", esp_err_to_name(err));
    }

    return err;
}
