/**
 * @file pn532_cmd.h
 * @brief PN532 command layer: high-level wrappers for common commands.
 *
 * Built on top of the core frame send/receive API (pn532_send_command /
 * pn532_receive_response). Each function sends one PN532 command and parses
 * its response, returning esp_err_t.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pn532.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Error codes returned by PN532 in status bytes                              */
/* ========================================================================= */

typedef enum {
    PN532_ERR_NONE            = 0x00,
    PN532_ERR_TIMEOUT         = 0x01,
    PN532_ERR_CRC             = 0x02,
    PN532_ERR_PARITY          = 0x03,
    PN532_ERR_BITCOUNT        = 0x04,
    PN532_ERR_FRAMING         = 0x05,
    PN532_ERR_COLLISION       = 0x06,
    PN532_ERR_BUF_OVERFLOW    = 0x07,
    PN532_ERR_RF_OVERFLOW     = 0x09,
    PN532_ERR_RF_TIMING       = 0x0A,
    PN532_ERR_RF_PROTOCOL     = 0x0B,
    PN532_ERR_TEMPERATURE     = 0x0D,
    PN532_ERR_BUFFER_INSUFFI  = 0x0E,
    PN532_ERR_NACK            = 0x0F,
    PN532_ERR_DEP_INVALID     = 0x10,
    PN532_ERR_DEP_MISMATCH    = 0x11,
    PN532_ERR_OVER_CURRENT    = 0x13,
    PN532_ERR_NAD_MISSING     = 0x14,
    PN532_ERR_TARGET_RELEASED = 0x29,
} pn532_error_code_t;

/* ========================================================================= */
/* GetFirmwareVersion (0x02)                                                  */
/* ========================================================================= */

/** @brief Firmware version information returned by the PN532. */
typedef struct {
    uint8_t ic;      /**< IC code — 0x32 for PN532. */
    uint8_t ver;     /**< Firmware major version. */
    uint8_t rev;     /**< Firmware minor revision. */
    uint8_t support; /**< Capability bitmask:
                          bit 0 = ISO/IEC14443 Type A
                          bit 1 = ISO/IEC14443 Type B
                          bit 2 = ISO18092 */
} pn532_firmware_version_t;

/**
 * @brief Query the PN532 firmware version (connectivity check).
 *
 * Sends GetFirmwareVersion (0x02) and validates IC == 0x32.
 *
 * @param[in]  h   Driver handle.
 * @param[out] out Receives firmware info.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or a transport/frame error.
 */
esp_err_t pn532_get_firmware_version(pn532_handle_t h,
                                     pn532_firmware_version_t *out);

/* ========================================================================= */
/* SAMConfiguration (0x14)                                                    */
/* ========================================================================= */

typedef enum {
    PN532_SAM_NORMAL       = 0x01, /**< SAM not used (default). */
    PN532_SAM_VIRTUAL_CARD = 0x02,
    PN532_SAM_WIRED_CARD   = 0x03,
    PN532_SAM_DUAL_CARD    = 0x04,
} pn532_sam_mode_t;

/**
 * @brief Configure the PN532's SAM (Security Access Module) mode.
 *
 * For our project use: pn532_sam_configuration(h, PN532_SAM_NORMAL, 0, true)
 *
 * @param[in] h           Driver handle.
 * @param[in] mode        SAM operating mode.
 * @param[in] timeout_50ms Timeout in 50 ms units (only used in VIRTUAL_CARD mode).
 * @param[in] irq_enabled true = PN532 drives IRQ pin; false = IRQ stays HIGH.
 * @return ESP_OK or error.
 */
esp_err_t pn532_sam_configuration(pn532_handle_t h,
                                  pn532_sam_mode_t mode,
                                  uint8_t timeout_50ms,
                                  bool irq_enabled);

/* ========================================================================= */
/* SetParameters (0x12)                                                       */
/* ========================================================================= */

/** @name SetParameters flag bits
 * @{ */
#define PN532_PARAM_NAD_USED         (1 << 0)
#define PN532_PARAM_DID_USED         (1 << 1)
#define PN532_PARAM_AUTO_ATR_RES     (1 << 2)
#define PN532_PARAM_AUTO_RATS        (1 << 4)
#define PN532_PARAM_ISO14443_4_PICC  (1 << 5)
#define PN532_PARAM_REMOVE_PREAMBLE  (1 << 6)
/** @} */

/**
 * @brief Set PN532 communication parameters.
 *
 * Bits 3 and 7 are RFU and must be 0; the function returns
 * ESP_ERR_INVALID_ARG if they are set.
 *
 * @param[in] h      Driver handle.
 * @param[in] flags  Combination of PN532_PARAM_* flags.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or transport/frame error.
 */
esp_err_t pn532_set_parameters(pn532_handle_t h, uint8_t flags);

/* ========================================================================= */
/* TgInitAsTarget (0x8C)                                                      */
/* ========================================================================= */

/** @name TgInitAsTarget mode byte flags
 * @{ */
#define PN532_TG_MODE_PASSIVE_ONLY  (1 << 0)
#define PN532_TG_MODE_DEP_ONLY      (1 << 1)
#define PN532_TG_MODE_PICC_ONLY     (1 << 2)
/** @} */

/** @brief Parameters for TgInitAsTarget. */
typedef struct {
    /* MifareParams (6 bytes) — 106 kbps passive activation */
    uint8_t sens_res[2];   /**< ATQA, LSB first. e.g. {0x04, 0x00} */
    uint8_t nfcid1[3];     /**< 3-byte NFCID1 (single size). */
    uint8_t sel_res;       /**< SAK: 0x20=ISO14443-4, 0x40=DEP, 0x60=both */

    /* FeliCaParams (18 bytes) — 212/424 kbps passive activation */
    uint8_t nfcid2[8];
    uint8_t pad[8];
    uint8_t system_code[2];

    /* NFCID3t (10 bytes) — used in ATR_RES */
    uint8_t nfcid3[10];

    /* General bytes (Gt), optional */
    const uint8_t *gt;
    uint8_t gt_len;        /**< 0 = no general bytes (max 47). */

    /* Historical bytes (Tk), optional */
    const uint8_t *tk;
    uint8_t tk_len;        /**< 0 = no historical bytes (max 47). */

    uint8_t mode;          /**< Combination of PN532_TG_MODE_* flags. */
} pn532_tg_init_params_t;

/** @brief Result of TgInitAsTarget. */
typedef struct {
    uint8_t mode;          /**< Actual mode PN532 was activated in. */
} pn532_tg_init_result_t;

/**
 * @brief Initialise PN532 as an NFC target (card emulation).
 *
 * Blocks until an external RF initiator activates the PN532, or timeout_ms
 * elapses. Use a large timeout (seconds to minutes) for real use.
 *
 * @param[in]  h          Driver handle.
 * @param[in]  params     Target configuration.
 * @param[out] result     Receives activation result.
 * @param[in]  timeout_ms Maximum time to wait for activation.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_TIMEOUT, or ESP_FAIL on
 *         PN532 error status.
 */
esp_err_t pn532_tg_init_as_target(pn532_handle_t h,
                                  const pn532_tg_init_params_t *params,
                                  pn532_tg_init_result_t *result,
                                  uint32_t timeout_ms);

/* ========================================================================= */
/* TgGetTargetStatus (0x8A)                                                   */
/* ========================================================================= */

typedef enum {
    PN532_TG_STATE_IDLE           = 0x00, /**< TG_IDLE / TG_RELEASED */
    PN532_TG_STATE_ACTIVATED      = 0x01, /**< TG_ACTIVATED (DEP) */
    PN532_TG_STATE_DESELECTED     = 0x02, /**< TG_DESELECTED (DEP) */
    PN532_TG_STATE_PICC_RELEASED  = 0x80, /**< PICC_RELEASED (ISO14443-4) */
    PN532_TG_STATE_PICC_ACTIVATED = 0x81, /**< PICC_ACTIVATED (ISO14443-4) */
    PN532_TG_STATE_PICC_DESELECTED = 0x82, /**< PICC_DESELECTED (ISO14443-4) */
} pn532_tg_state_t;

typedef struct {
    pn532_tg_state_t state;    /**< Current target state. */
    uint8_t          br_it;    /**< Raw BRit byte (0x00 if not activated). */
} pn532_tg_status_t;

/**
 * @brief Query the current state of the PN532 target session.
 *
 * Sends TgGetTargetStatus (0x8A) and returns the state and baud rate
 * information. Valid to call at any point after TgInitAsTarget has
 * been sent — useful for detecting reader departure (RELEASED/DESELECTED)
 * without waiting for a TgGetData timeout.
 *
 * @param h       Driver handle.
 * @param out     Receives state and BRit. Must not be NULL.
 * @return ESP_OK on success.
 *         ESP_ERR_INVALID_ARG if h or out is NULL.
 *         ESP_ERR_TIMEOUT if no response within timeout_ms.
 *         ESP_FAIL if response is malformed or unexpected command code.
 */
esp_err_t pn532_tg_get_target_status(pn532_handle_t    h,
                                     pn532_tg_status_t *out,
                                     uint32_t           timeout_ms);

/* ========================================================================= */
/* TgGetData (0x86)                                                           */
/* ========================================================================= */

/**
 * @brief Retrieve data received from the NFC initiator.
 *
 * Handles internal chaining (MI bit) transparently.
 *
 * @param[in]  h          Driver handle.
 * @param[out] buf        Destination buffer.
 * @param[in]  buf_size   Capacity of @p buf.
 * @param[out] out_len    Receives the number of payload bytes written.
 * @param[in]  timeout_ms Maximum time to wait for the response frame.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE, or ESP_FAIL.
 */
esp_err_t pn532_tg_get_data(pn532_handle_t h,
                            uint8_t *buf,
                            size_t buf_size,
                            size_t *out_len,
                            uint32_t timeout_ms);

/* ========================================================================= */
/* TgSetData (0x8E)                                                           */
/* ========================================================================= */

/**
 * @brief Send data from the PN532 to the NFC initiator.
 *
 * @param[in] h          Driver handle.
 * @param[in] data       Payload to send (max 262 bytes).
 * @param[in] data_len   Number of bytes in @p data.
 * @param[in] timeout_ms Maximum time to wait for the response frame.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE, or ESP_FAIL.
 */
esp_err_t pn532_tg_set_data(pn532_handle_t h,
                            const uint8_t *data,
                            size_t data_len,
                            uint32_t timeout_ms);

/* ========================================================================= */
/* InListPassiveTarget (0x4A)                                                 */
/* ========================================================================= */

/** @brief Bit rate / modulation type for passive target enumeration. */
typedef enum {
    PN532_BRTY_106A  = 0x00, /**< ISO/IEC 14443-A @ 106 kbps. */
    PN532_BRTY_212F  = 0x01, /**< FeliCa @ 212 kbps. */
    PN532_BRTY_424F  = 0x02, /**< FeliCa @ 424 kbps. */
    PN532_BRTY_106B  = 0x03, /**< ISO/IEC 14443-B @ 106 kbps. */
    PN532_BRTY_JEWEL = 0x04, /**< Jewel @ 106 kbps. */
} pn532_brty_t;

/** @brief Information about one discovered passive target. */
typedef struct {
    uint8_t tg;          /**< Target number assigned by PN532 (1-based). */
    uint8_t atqa[2];     /**< ATQA (Type A only, little-endian). */
    uint8_t sak;         /**< SAK (Type A only). */
    uint8_t nfcid_len;   /**< Length of NFCID / UID. */
    uint8_t nfcid[10];   /**< NFCID / UID bytes (up to 10 for triple-size). */
    uint8_t ats_len;     /**< ATS length (0 if target does not support ISO14443-4). */
    uint8_t ats[256];    /**< ATS response bytes. */
} pn532_passive_target_t;

/**
 * @brief Discover passive NFC targets in the RF field.
 *
 * Sends InListPassiveTarget (0x4A). NbTg=0 in the response means no targets
 * were found — this is returned as ESP_OK with *num_targets_out == 0.
 *
 * @param[in]  h                 Driver handle.
 * @param[in]  max_targets       Maximum number of targets to detect (1 or 2).
 * @param[in]  brty              Bit rate / modulation type.
 * @param[in]  initiator_data    Optional extra initiator data (may be NULL).
 * @param[in]  initiator_data_len Length of @p initiator_data (0 if NULL).
 * @param[out] targets_out       Array of target info (caller allocates).
 * @param[out] num_targets_out   Receives the number of targets found.
 * @param[in]  timeout_ms        Maximum time to wait for the response.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or transport/frame error.
 */
esp_err_t pn532_in_list_passive_target(pn532_handle_t h,
                                       uint8_t max_targets,
                                       pn532_brty_t brty,
                                       const uint8_t *initiator_data,
                                       size_t initiator_data_len,
                                       pn532_passive_target_t *targets_out,
                                       uint8_t *num_targets_out,
                                       uint32_t timeout_ms);

/* ========================================================================= */
/* TgGetInitiatorCommand (0x88)                                               */
/* ========================================================================= */

/**
 * @brief Retrieve the raw command from the NFC initiator.
 *
 * Used in ISO14443-4 PICC emulation after TgInitAsTarget returns with an
 * indication that the initiator has sent a command.
 *
 * @param[in]  h          Driver handle.
 * @param[out] buf        Destination buffer for the initiator data.
 * @param[in]  buf_size   Capacity of @p buf.
 * @param[out] out_len    Receives the number of bytes written.
 * @param[in]  timeout_ms Maximum time to wait for the response.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE, or ESP_FAIL.
 */
esp_err_t pn532_tg_get_initiator_command(pn532_handle_t h,
                                         uint8_t *buf,
                                         size_t buf_size,
                                         size_t *out_len,
                                         uint32_t timeout_ms);

/* ========================================================================= */
/* TgResponseToInitiator (0x90)                                               */
/* ========================================================================= */

/**
 * @brief Send a response to the initiator in ISO14443-4 PICC emulation.
 *
 * Alternative to TgSetData for the initial response after RATS.
 * Maximum 262 bytes. PN532 handles protocol chaining internally.
 *
 * @param[in] h          Driver handle.
 * @param[in] data       Response payload (max 262 bytes).
 * @param[in] data_len   Number of bytes in @p data.
 * @param[in] timeout_ms Maximum time to wait for the RF exchange to complete.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE, or ESP_FAIL.
 */
esp_err_t pn532_tg_response_to_initiator(pn532_handle_t h,
                                         const uint8_t *data,
                                         size_t data_len,
                                         uint32_t timeout_ms);

/* ========================================================================= */
/* TgSetMetaData (0x94)                                                       */
/* ========================================================================= */

/**
 * @brief Set meta-data to be sent to the initiator.
 *
 * Appends meta-data to the next TgResponseToInitiator / TgSetData payload.
 * Maximum 262 bytes.
 *
 * @param[in] h          Driver handle.
 * @param[in] data       Meta-data bytes (max 262 bytes).
 * @param[in] data_len   Number of bytes in @p data.
 * @param[in] timeout_ms Maximum time to wait for the response.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE, or ESP_FAIL.
 */
esp_err_t pn532_tg_set_meta_data(pn532_handle_t h,
                                 const uint8_t *data,
                                 size_t data_len,
                                 uint32_t timeout_ms);

/* TODO: ISO-DEP application layer — separate component */

#ifdef __cplusplus
}
#endif
