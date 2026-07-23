/**
 * @file pn532.h
 * @brief Public API for the PN532 NFC controller driver (physical/link layer).
 *
 * This component implements only the PN532 physical and data-link layer:
 * transport abstraction, frame construction/parsing, and the command/response
 * handshake (command -> ACK -> response). Crypto, ISO-DEP application logic and
 * higher-level command wrappers (GetFirmwareVersion, SAMConfiguration, ...)
 * live in a separate ESP layer and are intentionally NOT part of this component.
 *
 * The driver is transport-agnostic: it talks to the chip exclusively through a
 * @ref pn532_transport_ops_t vtable. An I2C backend is provided in
 * `pn532_i2c.h`; SPI/UART backends can be added later without touching the core.
 *
 * @note ESP-IDF v5.x component. All public functions return `esp_err_t`.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum PN532 payload size (TFI + DATA) carried by an extended frame.
 *
 * The PN532 supports "normal" frames (LEN up to 255) and "extended" frames.
 * The largest information payload the chip accepts/returns is 264 data bytes,
 * i.e. 265 bytes including the TFI. Buffers sized with this constant can hold
 * any valid frame payload.
 */
#define PN532_MAX_PAYLOAD_LEN   265U

/**
 * @brief Opaque driver handle.
 *
 * Allocated by @ref pn532_init and released by @ref pn532_deinit. The internal
 * layout is private to the implementation; callers must treat it as a token.
 */
typedef struct pn532_t *pn532_handle_t;

/**
 * @brief Transport operations vtable.
 *
 * Abstracts the physical link (I2C now, SPI/UART later). Each callback receives
 * the transport-private @p ctx returned by the backend factory (e.g.
 * @ref pn532_i2c_create).
 *
 * Threading: the PN532 core serialises calls into this vtable per handle, but
 * the backend is still responsible for protecting the shared bus against other
 * devices/tasks (the I2C backend uses an internal mutex for this).
 */
typedef struct {
    /**
     * @brief Write a fully-formed frame to the chip.
     * @param ctx  Transport-private context.
     * @param buf  Bytes to transmit (complete PN532 frame).
     * @param len  Number of bytes in @p buf.
     * @return ESP_OK on success; ESP_ERR_TIMEOUT if the chip never ACKs its
     *         address; otherwise an esp_err_t from the underlying bus.
     */
    esp_err_t (*write)(void *ctx, const uint8_t *buf, size_t len);

    /**
     * @brief Read the 1-byte PN532 status byte (RDY in bit 0).
     * @param ctx         Transport-private context.
     * @param status_out  Receives the raw status byte.
     * @return ESP_OK on success; otherwise an esp_err_t from the bus.
     * @note This performs a self-contained START/STOP transaction. It is used
     *       for polling; the ready frame read itself re-reads the status byte
     *       (see @ref read_frame).
     */
    esp_err_t (*read_status)(void *ctx, uint8_t *status_out);

    /**
     * @brief Read a frame, status byte first, in a single bus transaction.
     * @param ctx  Transport-private context.
     * @param buf  Destination buffer.
     * @param len  Number of bytes to read, INCLUDING the leading status byte;
     *             i.e. @p buf[0] receives the status byte and @p buf[1..len-1]
     *             receive frame bytes.
     * @return ESP_OK on success; ESP_ERR_INVALID_RESPONSE if the chip reports
     *         not-ready; otherwise an esp_err_t from the bus.
     * @note The status byte and frame bytes MUST be read in one transaction:
     *       issuing an I2C STOP before the whole frame is clocked out loses the
     *       remaining bytes. Callers therefore over-read (status + frame) at
     *       once rather than reading the status separately.
     */
    esp_err_t (*read_frame)(void *ctx, uint8_t *buf, size_t len);

    /**
     * @brief Block until the chip signals a frame is ready (IRQ or polling).
     * @param ctx         Transport-private context.
     * @param timeout_ms  Maximum time to wait.
     * @return ESP_OK when ready; ESP_ERR_TIMEOUT if not ready in time.
     */
    esp_err_t (*wait_ready)(void *ctx, uint32_t timeout_ms);

    /**
     * @brief Release all resources owned by the transport context.
     * @param ctx  Transport-private context (may be NULL).
     */
    void (*destroy)(void *ctx);

    /**
     * @brief Assert hardware reset on the PN532 (optional, NULL if not supported).
     * @param ctx        Transport-private context.
     * @param pulse_ms   Duration to hold reset LOW (milliseconds).
     * @param settle_ms  Wait after reset releases before returning (ms).
     * @return ESP_OK, or ESP_ERR_NOT_SUPPORTED if transport has no reset pin.
     */
    esp_err_t (*reset_device)(void *ctx, uint32_t pulse_ms, uint32_t settle_ms);

    /**
     * @brief Acquire the transport bus mutex (optional, NULL if not applicable).
     * @param ctx         Transport-private context.
     * @param timeout_ms  Maximum time to wait for the mutex.
     * @return ESP_OK on success; ESP_ERR_TIMEOUT if not acquired in time.
     */
    esp_err_t (*bus_lock)(void *ctx, uint32_t timeout_ms);

    /**
     * @brief Release the transport bus mutex (optional, NULL if not applicable).
     * @param ctx  Transport-private context.
     */
    void (*bus_unlock)(void *ctx);
} pn532_transport_ops_t;

/**
 * @brief Core driver configuration.
 */
typedef struct {
    const pn532_transport_ops_t *ops;  /**< Transport vtable (must outlive the handle). */
    void *transport_ctx;               /**< Transport-private context passed to @p ops. */
} pn532_config_t;

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief Allocate and initialise a PN532 driver instance.
 *
 * Does not perform any bus traffic; it only binds the supplied transport to a
 * newly allocated handle. The transport itself must already be created (see
 * @ref pn532_i2c_create).
 *
 * @param[in]  cfg        Configuration; @p cfg->ops must be non-NULL.
 * @param[out] out_handle Receives the created handle on success.
 * @return
 *   - ESP_OK on success
 *   - ESP_ERR_INVALID_ARG if @p cfg, @p cfg->ops or @p out_handle is NULL
 *   - ESP_ERR_NO_MEM if allocation fails
 */
esp_err_t pn532_init(const pn532_config_t *cfg, pn532_handle_t *out_handle);

/**
 * @brief Destroy a driver instance and its transport.
 *
 * Invokes the transport's @ref pn532_transport_ops_t::destroy callback and frees
 * the handle. Safe to call with NULL.
 *
 * @param[in] handle Handle from @ref pn532_init (may be NULL).
 */
void pn532_deinit(pn532_handle_t handle);

/* ------------------------------------------------------------------------- */
/* Core I/O — used internally by the command layer added in a future session  */
/* ------------------------------------------------------------------------- */

/**
 * @brief Build a command frame, send it, and wait for the chip's ACK.
 *
 * Constructs a normal or extended information frame (TFI = 0xD4) carrying
 * @p cmd_code followed by @p params, transmits it, then waits for and validates
 * the 6-byte ACK frame. It does NOT read the response; call
 * @ref pn532_receive_response for that.
 *
 * @param[in] h           Driver handle.
 * @param[in] cmd_code    PN532 command byte (first DATA byte).
 * @param[in] params      Command parameters (may be NULL when @p params_len is 0).
 * @param[in] params_len  Number of parameter bytes.
 * @return
 *   - ESP_OK if the command was sent and ACKed
 *   - ESP_ERR_INVALID_ARG on a NULL handle or oversized payload
 *   - ESP_ERR_TIMEOUT if the ACK does not arrive in time
 *   - ESP_FAIL if the received frame is not a valid ACK
 */
esp_err_t pn532_send_command(pn532_handle_t h,
                             uint8_t cmd_code,
                             const uint8_t *params, size_t params_len);

/**
 * @brief Wait for and parse a response frame from the chip.
 *
 * Waits for readiness, reads the frame, validates the length and data
 * checksums (LCS/DCS) and the TFI (0xD5), and copies the DATA payload
 * (response command byte + parameters) into @p buf.
 *
 * @param[in]  h          Driver handle.
 * @param[out] buf        Destination for the response DATA payload.
 * @param[in]  buf_size   Capacity of @p buf in bytes.
 * @param[out] out_len    Receives the number of payload bytes written.
 * @param[in]  timeout_ms Maximum time to wait for the frame.
 * @return
 *   - ESP_OK on a valid response
 *   - ESP_ERR_INVALID_ARG on NULL arguments
 *   - ESP_ERR_TIMEOUT if no frame arrives in time
 *   - ESP_ERR_INVALID_CRC on an LCS or DCS mismatch
 *   - ESP_ERR_INVALID_SIZE if the payload does not fit in @p buf
 *   - ESP_FAIL if the chip returned an application error frame
 */
esp_err_t pn532_receive_response(pn532_handle_t h,
                                 uint8_t *buf, size_t buf_size,
                                 size_t *out_len,
                                 uint32_t timeout_ms);

/**
 * @brief Wake the PN532 from Power-Down / LowVbat mode.
 *
 * After reset or power-up the chip is asleep. This sends a short burst of dummy
 * 0x55 bytes to wake it, then waits ~2 ms (T_osc_start) for its oscillator to
 * stabilise before the first real command is issued.
 *
 * @note T_osc_start is typically a few hundred microseconds but can reach ~2 ms
 *       depending on the quartz crystal and board layout; the conservative 2 ms
 *       delay covers the worst case.
 *
 * @param[in] h Driver handle.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG on a NULL handle; otherwise a
 *         bus error from the transport.
 */
esp_err_t pn532_wakeup(pn532_handle_t h);

/**
 * @brief Send an ACK frame to the PN532.
 *
 * Used to abort a command that is currently being processed by the
 * PN532. After sending ACK, the PN532 discontinues the current
 * operation and returns to waiting for a new command. No response
 * is sent by the PN532 after an abort ACK.
 *
 * This is also used after receiving a response to confirm receipt,
 * though that step is optional per the PN532 protocol.
 *
 * @param h  Driver handle.
 * @return ESP_OK on success.
 *         ESP_ERR_INVALID_ARG if h is NULL.
 *         ESP_ERR_TIMEOUT if the write fails (bus error).
 */
esp_err_t pn532_send_ack(pn532_handle_t h);

/**
 * @brief Assert hardware reset on the PN532.
 *
 * Acquires the bus mutex before asserting reset and releases it after
 * settle_ms completes, so no I2C transaction can race with the reset.
 * Delegates to the transport's reset_device internally.
 *
 * Returns ESP_ERR_NOT_SUPPORTED if the transport has no reset pin.
 *
 * @param[in] h          Driver handle.
 * @param[in] pulse_ms   Duration to hold reset LOW (milliseconds).
 * @param[in] settle_ms  Wait after reset releases before returning (ms).
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NOT_SUPPORTED, or transport error.
 */
esp_err_t pn532_reset(pn532_handle_t h, uint32_t pulse_ms, uint32_t settle_ms);

/* TODO: command layer — implemented in next session.
 *
 * The following high-level command wrappers will be added on top of
 * pn532_send_command()/pn532_receive_response() in a later layer and are
 * intentionally NOT declared yet:
 *   - pn532_get_firmware_version()
 *   - pn532_sam_configuration()
 *   - pn532_in_list_passive_target()
 *   - pn532_tg_init_as_target()
 *   - ... and other InXxx/TgXxx commands.
 * Crypto and ISO-DEP application logic live in a separate ESP layer, not here.
 */

#ifdef __cplusplus
}
#endif
