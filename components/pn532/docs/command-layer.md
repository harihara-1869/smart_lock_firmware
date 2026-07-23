# Command Layer (`pn532_cmd.c`)

## Overview

High-level wrappers for the PN532 command set. Each function sends one PN532 command via `pn532_send_command()`, reads and validates the response via `pn532_receive_response()`, and returns `esp_err_t`.

The command layer knows the PN532 command/response formats but is **transport-agnostic** — it uses only the core driver API (`pn532_send_command` / `pn532_receive_response`).

**Source**: `components/pn532/src/pn532_cmd.c`
**Header**: `components/pn532/include/pn532_cmd.h`

## PN532 Error Codes

The PN532 reports application-level errors in status bytes. The command layer validates these and logs them.

```c
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
```

## Commands

### GetFirmwareVersion (0x02)

Queries the PN532 IC code, firmware version/revision, and capability bitmask.

**Response format**: `{0x03, IC, VER, REV, SUPPORT}` (5 bytes)

| Field | Offset | Meaning |
|-------|--------|---------|
| IC | buf[1] | 0x32 for PN532 |
| VER | buf[2] | Major version (upper nibble) + minor (lower nibble) |
| REV | buf[3] | Firmware revision |
| SUPPORT | buf[4] | Capabilities: bit 0 = ISO14443A, bit 1 = ISO14443B, bit 2 = ISO18092 |

**Validation**: IC must equal 0x32, otherwise returns `ESP_FAIL`.

### SAMConfiguration (0x14)

Configures the Security Access Module (SAM) operating mode.

| Mode | Value | Description |
|------|-------|-------------|
| `PN532_SAM_NORMAL` | 0x01 | SAM not used (default for card emulation) |
| `PN532_SAM_VIRTUAL_CARD` | 0x02 | Virtual card mode (SAM acts as card) |
| `PN532_SAM_WIRED_CARD` | 0x03 | Wired card mode |
| `PN532_SAM_DUAL_CARD` | 0x04 | Dual card mode |

**Parameter bytes**: varies by mode.
- NORMAL / WIRED_CARD / DUAL_CARD: `{mode, irq_flag}` (2 bytes)
- VIRTUAL_CARD: `{mode, timeout_50ms, irq_flag}` (3 bytes)

**Response**: `{0x15}` — single command byte, no data.

### SetParameters (0x12)

Sets PN532 communication flags. One parameter byte.

| Flag | Bit | Description |
|------|-----|-------------|
| `PN532_PARAM_NAD_USED` | 0 | Use Node Address byte |
| `PN532_PARAM_DID_USED` | 1 | Use Device ID byte |
| `PN532_PARAM_AUTO_ATR_RES` | 2 | Auto-send ATR_RES on activation |
| `PN532_PARAM_AUTO_RATS` | 4 | Auto-send RATS on ISO14443-4 activation |
| `PN532_PARAM_ISO14443_4_PICC` | 5 | Enable ISO14443-4 PICC emulation |
| `PN532_PARAM_REMOVE_PREAMBLE` | 6 | Remove preamble/postamble from responses |

**RFU validation**: bits 3 and 7 are reserved. The function returns `ESP_ERR_INVALID_ARG` if either is set.

**Response**: `{0x13}` — single command byte, no data.

### TgInitAsTarget (0x8C)

Puts the PN532 into NFC target (card emulation) mode. **Blocks** until an external RF reader activates the chip, or the timeout elapses.

**Parameter structure** (up to 100+ bytes):

```
Mode(1) + MifareParams(6) + FeliCaParams(18) + NFCID3t(10)
+ LenGt(1) + Gt(gt_len) + LenTk(1) + Tk(tk_len)
```

| Field | Size | Description |
|-------|------|-------------|
| Mode | 1 | `PN532_TG_MODE_PASSIVE_ONLY`, `_DEP_ONLY`, `_PICC_ONLY` |
| MifareParams | 6 | sens_res(2) + nfcid1(3) + sel_res(1) — 106 kbps passive activation |
| FeliCaParams | 18 | nfcid2(8) + pad(8) + system_code(2) — 212/424 kbps passive |
| NFCID3t | 10 | Used in ATR_RES for DEP mode |
| LenGt + Gt | 1+N | General bytes (max 47) |
| LenTk + Tk | 1+N | Historical bytes (max 47) |

**Response format**: `{0x8D, status, mode, [InitiatorCommand...]}`

- `status`: 0x00 = success
- `mode`: actual activation mode

**Timeout**: use a large value (seconds to minutes) for real card emulation. The PN532 enters power-down internally while waiting for RF — low power consumption.

### TgGetData (0x86)

Retrieves data received from the NFC initiator. Handles MI-bit chaining transparently.

**Response format**: `{0x87, status, [NAD], data...}`

- `status` bit 6 (MI): more fragments follow — driver chains automatically
- `status` bit 7 (NAD): NAD byte present before data
- `status` bits 0-5: PN532 error code

**Chaining**: if MI bit is set, the driver sends repeated TgGetData commands and accumulates fragments up to `PN532_MAX_PAYLOAD_LEN` (265 bytes) before returning the complete payload to the caller.

### TgSetData (0x8E)

Sends data from the PN532 to the NFC initiator.

- Max 262 bytes per exchange (`PN532_TG_MAX_DATA_LEN`)
- Response: `{0x8F, status}` — status byte only

### TgGetTargetStatus (0x8A)

Queries the current state of the PN532 target session. No parameters. Valid to call at any point after TgInitAsTarget has been sent — useful for detecting reader departure (RELEASED/DESELECTED) without waiting for a TgGetData timeout.

**Response format**: `{0x8B, State, BRit}` (3 bytes)

| Field | Offset | Description |
|-------|--------|-------------|
| State | buf[1] | Current target state (see below) |
| BRit | buf[2] | Baud rate in both directions (only meaningful when ACTIVATED) |

**State values** (`pn532_tg_state_t`):

| Value | Constant | Description |
|-------|----------|-------------|
| 0x00 | `PN532_TG_STATE_IDLE` | TG_IDLE / TG_RELEASED |
| 0x01 | `PN532_TG_STATE_ACTIVATED` | TG_ACTIVATED (DEP) |
| 0x02 | `PN532_TG_STATE_DESELECTED` | TG_DESELECTED (DEP) |
| 0x80 | `PN532_TG_STATE_PICC_RELEASED` | PICC_RELEASED (ISO14443-4) |
| 0x81 | `PN532_TG_STATE_PICC_ACTIVATED` | PICC_ACTIVATED (ISO14443-4) |
| 0x82 | `PN532_TG_STATE_PICC_DESELECTED` | PICC_DESELECTED (ISO14443-4) |

Unknown state values are logged at WARN but do not cause a failure — future firmware may add new states.

**BRit byte encoding** (only meaningful when `State == PN532_TG_STATE_ACTIVATED`):

```
bits 7:4 = Speed_Initiator → Target  (000=106k, 001=212k, 010=424k)
bits 3:0 = Speed_Target → Initiator  (000=106k, 001=212k, 010=424k)
```

**Validation**:
- Response must be at least 3 bytes
- Response command code must be 0x8B

## Secondary Commands

### InListPassiveTarget (0x4A)

Discovers passive NFC targets in the RF field. Supports multiple modulation types.

**Parameter bytes**: `{MaxTg, BrTy, [InitiatorData...]}`

| Field | Size | Description |
|-------|------|-------------|
| MaxTg | 1 | Maximum targets to detect (1 or 2) |
| BrTy | 1 | Bit rate / modulation type (`pn532_brty_t`) |
| InitiatorData | 0-N | Optional extra data (e.g. SENSB_REQ for Type B) |

**Supported modulation types** (`pn532_brty_t`):

| Value | Type |
|-------|------|
| 0x00 | ISO/IEC 14443-A @ 106 kbps |
| 0x01 | FeliCa @ 212 kbps |
| 0x02 | FeliCa @ 424 kbps |
| 0x03 | ISO/IEC 14443-B @ 106 kbps |
| 0x04 | Jewel @ 106 kbps |

**Response format** (Type A): `{0x4B, NbTg, [Tg, ATQA[2], SAK, NFCIDLen, NFCID[n], [AtsLen, ATS[m]]]}`

- NbTg = 0 means no targets found (not an error — returns ESP_OK with `*num_targets_out = 0`)
- For Type A: ATQA (2 bytes) + SAK (1 byte) + NFCID (variable) + optional ATS (if SAK bit 5 set)
- ATS is present when the target supports ISO14443-4 (SAK & 0x20)

### TgGetInitiatorCommand (0x88)

Retrieves the raw command from the NFC initiator in ISO14443-4 PICC emulation mode.

**Response format**: `{0x89, status, [DataIn...]}`

- `status`: PN532 error code (0x00 = success)
- `DataIn`: 0 to 262 bytes of initiator data

### TgResponseToInitiator (0x90)

Sends a response to the initiator in ISO14443-4 PICC emulation mode. This is an alternative to TgSetData for the initial response after RATS.

**Parameter bytes**: `{DataOut...}` (0 to 262 bytes)

**Response format**: `{0x91, status}` — status byte only

### TgSetMetaData (0x94)

Sets meta-data to be appended to the next TgResponseToInitiator or TgSetData payload.

**Parameter bytes**: `{DataOut...}` (0 to 262 bytes)

**Response format**: `{0x95, status}` — status byte only

## Typical Usage: Card Emulation Loop

```c
// Boot: one-time init
pn532_init(&cfg, &h);
pn532_wakeup(h);
pn532_sam_configuration(h, PN532_SAM_NORMAL, 0, true);
pn532_set_parameters(h, PN532_PARAM_AUTO_ATR_RES);

// Main loop: wait for phone, exchange data, repeat
while (1) {
    pn532_tg_init_result_t result;
    pn532_tg_init_params_t params = {
        .sens_res = {0x04, 0x00},
        .nfcid1   = {0x01, 0x02, 0x03},
        .sel_res  = 0x20,
        .mode     = PN532_TG_MODE_PASSIVE_ONLY,
    };

    esp_err_t err = pn532_tg_init_as_target(h, &params, &result, 30000);
    if (err == ESP_ERR_TIMEOUT) continue;  // no phone, retry

    // Phone tapped — read data
    uint8_t apdu[64]; size_t len;
    pn532_tg_get_data(h, apdu, sizeof(apdu), &len, 2000);

    // Process APDU, decide to unlock, build response
    uint8_t response[] = {0x90, 0x00};  // SW1-SW2 OK
    pn532_tg_set_data(h, response, sizeof(response), 2000);
}
```

The IRQ pin makes this efficient: `TgInitAsTarget` blocks on a FreeRTOS semaphore (not a busy loop), and the PN532 GPIO ISR wakes the task when a reader activates the chip.
