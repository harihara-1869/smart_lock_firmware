# API Reference

## Command Layer (`pn532_cmd.h`)

### Types

#### `pn532_error_code_t`

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

#### `pn532_firmware_version_t`

```c
typedef struct {
    uint8_t ic;      // IC code — 0x32 for PN532
    uint8_t ver;     // Firmware version (upper nibble major, lower minor)
    uint8_t rev;     // Firmware revision
    uint8_t support; // Capability bitmask: bit 0 = 14443A, bit 1 = 14443B, bit 2 = 18092
} pn532_firmware_version_t;
```

#### `pn532_sam_mode_t`

```c
typedef enum {
    PN532_SAM_NORMAL       = 0x01,
    PN532_SAM_VIRTUAL_CARD = 0x02,
    PN532_SAM_WIRED_CARD   = 0x03,
    PN532_SAM_DUAL_CARD    = 0x04,
} pn532_sam_mode_t;
```

#### `pn532_tg_init_params_t`

```c
typedef struct {
    uint8_t sens_res[2];    // ATQA (LSB first)
    uint8_t nfcid1[3];      // 3-byte NFCID1 (single size)
    uint8_t sel_res;        // SAK: 0x20 = ISO14443-4
    uint8_t nfcid2[8];      // FeliCa NFCID2
    uint8_t pad[8];         // FeliCa pad
    uint8_t system_code[2]; // FeliCa system code
    uint8_t nfcid3[10];     // NFCID3t for ATR_RES
    const uint8_t *gt;      // General bytes (optional, max 47)
    uint8_t gt_len;
    const uint8_t *tk;      // Historical bytes (optional, max 47)
    uint8_t tk_len;
    uint8_t mode;           // PN532_TG_MODE_* flags
} pn532_tg_init_params_t;
```

#### `pn532_tg_init_result_t`

```c
typedef struct {
    uint8_t mode;  // Actual mode PN532 was activated in
} pn532_tg_init_result_t;
```

#### `pn532_tg_state_t`

```c
typedef enum {
    PN532_TG_STATE_IDLE            = 0x00,  // TG_IDLE / TG_RELEASED
    PN532_TG_STATE_ACTIVATED       = 0x01,  // TG_ACTIVATED (DEP)
    PN532_TG_STATE_DESELECTED      = 0x02,  // TG_DESELECTED (DEP)
    PN532_TG_STATE_PICC_RELEASED   = 0x80,  // PICC_RELEASED (ISO14443-4)
    PN532_TG_STATE_PICC_ACTIVATED  = 0x81,  // PICC_ACTIVATED (ISO14443-4)
    PN532_TG_STATE_PICC_DESELECTED = 0x82,  // PICC_DESELECTED (ISO14443-4)
} pn532_tg_state_t;
```

#### `pn532_tg_status_t`

```c
typedef struct {
    pn532_tg_state_t state;  // Current target state
    uint8_t          br_it;  // Raw BRit byte (0x00 if not activated)
} pn532_tg_status_t;
```

`br_it` encodes baud rate in both directions when `state == PN532_TG_STATE_ACTIVATED`:
bits 7:4 = Initiator→Target speed, bits 3:0 = Target→Initiator speed
(000=106kbps, 001=212kbps, 010=424kbps).

#### `pn532_brty_t`

```c
typedef enum {
    PN532_BRTY_106A  = 0x00,  // ISO/IEC 14443-A @ 106 kbps
    PN532_BRTY_212F  = 0x01,  // FeliCa @ 212 kbps
    PN532_BRTY_424F  = 0x02,  // FeliCa @ 424 kbps
    PN532_BRTY_106B  = 0x03,  // ISO/IEC 14443-B @ 106 kbps
    PN532_BRTY_JEWEL = 0x04,  // Jewel @ 106 kbps
} pn532_brty_t;
```

#### `pn532_passive_target_t`

```c
typedef struct {
    uint8_t tg;          // Target number assigned by PN532 (1-based)
    uint8_t atqa[2];     // ATQA (Type A only, little-endian)
    uint8_t sak;         // SAK (Type A only)
    uint8_t nfcid_len;   // Length of NFCID / UID
    uint8_t nfcid[10];   // NFCID / UID bytes (up to 10 for triple-size)
    uint8_t ats_len;     // ATS length (0 if not ISO14443-4)
    uint8_t ats[256];    // ATS response bytes
} pn532_passive_target_t;
```

### Constants

#### SetParameters flags

| Constant | Bit | Description |
|----------|-----|-------------|
| `PN532_PARAM_NAD_USED` | 0 | Node Address byte enabled |
| `PN532_PARAM_DID_USED` | 1 | Device ID byte enabled |
| `PN532_PARAM_AUTO_ATR_RES` | 2 | Auto ATR_RES on activation |
| `PN532_PARAM_AUTO_RATS` | 4 | Auto RATS on ISO14443-4 activation |
| `PN532_PARAM_ISO14443_4_PICC` | 5 | ISO14443-4 PICC emulation |
| `PN532_PARAM_REMOVE_PREAMBLE` | 6 | Remove preamble from responses |

#### TgInitAsTarget mode flags

| Constant | Bit | Description |
|----------|-----|-------------|
| `PN532_TG_MODE_PASSIVE_ONLY` | 0 | Passive activation only |
| `PN532_TG_MODE_DEP_ONLY` | 1 | DEP activation only |
| `PN532_TG_MODE_PICC_ONLY` | 2 | PICC activation only |

### Functions

#### `pn532_get_firmware_version`

```c
esp_err_t pn532_get_firmware_version(pn532_handle_t h, pn532_firmware_version_t *out);
```

Query PN532 firmware version (connectivity check). Validates IC == 0x32.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Success |
| `ESP_ERR_INVALID_ARG` | NULL handle or output pointer |
| `ESP_FAIL` | IC != 0x32 or unexpected response |

#### `pn532_sam_configuration`

```c
esp_err_t pn532_sam_configuration(pn532_handle_t h,
                                  pn532_sam_mode_t mode,
                                  uint8_t timeout_50ms,
                                  bool irq_enabled);
```

Configure SAM mode. `timeout_50ms` only used in `PN532_SAM_VIRTUAL_CARD` mode.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Success |
| `ESP_ERR_INVALID_ARG` | NULL handle |
| `ESP_FAIL` | Unexpected PN532 response |

#### `pn532_set_parameters`

```c
esp_err_t pn532_set_parameters(pn532_handle_t h, uint8_t flags);
```

Set communication parameters. Rejects RFU bits 3 and 7.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Success |
| `ESP_ERR_INVALID_ARG` | NULL handle or RFU bits set |
| `ESP_FAIL` | Unexpected PN532 response |

#### `pn532_tg_init_as_target`

```c
esp_err_t pn532_tg_init_as_target(pn532_handle_t h,
                                  const pn532_tg_init_params_t *params,
                                  pn532_tg_init_result_t *result,
                                  uint32_t timeout_ms);
```

Initialise PN532 as NFC target. Blocks until an external reader activates the chip.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Activated by reader |
| `ESP_ERR_INVALID_ARG` | NULL args or gt/tk too long (>47) |
| `ESP_ERR_TIMEOUT` | No reader within timeout |
| `ESP_FAIL` | PN532 returned error status |

#### `pn532_tg_get_data`

```c
esp_err_t pn532_tg_get_data(pn532_handle_t h,
                            uint8_t *buf, size_t buf_size,
                            size_t *out_len, uint32_t timeout_ms);
```

Receive data from NFC initiator. Handles MI-bit chaining transparently.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Data received |
| `ESP_ERR_INVALID_ARG` | NULL args |
| `ESP_ERR_INVALID_SIZE` | Accumulated data exceeds buffer |
| `ESP_FAIL` | PN532 error status |

#### `pn532_tg_set_data`

```c
esp_err_t pn532_tg_set_data(pn532_handle_t h,
                            const uint8_t *data, size_t data_len,
                            uint32_t timeout_ms);
```

Send data to NFC initiator. Max 262 bytes.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Data sent |
| `ESP_ERR_INVALID_ARG` | NULL handle, or NULL data with non-zero len |
| `ESP_ERR_INVALID_SIZE` | data_len > 262 |
| `ESP_FAIL` | PN532 error status |

#### `pn532_tg_get_target_status`

```c
esp_err_t pn532_tg_get_target_status(pn532_handle_t    h,
                                     pn532_tg_status_t *out,
                                     uint32_t           timeout_ms);
```

Query the current state of the PN532 target session. Sends TgGetTargetStatus (0x8A). Valid to call at any point after TgInitAsTarget — useful for detecting reader departure without waiting for a TgGetData timeout.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Success |
| `ESP_ERR_INVALID_ARG` | NULL handle or output pointer |
| `ESP_ERR_TIMEOUT` | No response within timeout_ms |
| `ESP_FAIL` | Malformed response or unexpected command code |

#### `pn532_in_list_passive_target`

```c
esp_err_t pn532_in_list_passive_target(pn532_handle_t h,
                                       uint8_t max_targets,
                                       pn532_brty_t brty,
                                       const uint8_t *initiator_data,
                                       size_t initiator_data_len,
                                       pn532_passive_target_t *targets_out,
                                       uint8_t *num_targets_out,
                                       uint32_t timeout_ms);
```

Discover passive NFC targets in the RF field. NbTg=0 means no targets found (not an error).

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Success (check `*num_targets_out`) |
| `ESP_ERR_INVALID_ARG` | NULL args or max_targets not 1 or 2 |
| `ESP_FAIL` | PN532 error or truncated response |

#### `pn532_tg_get_initiator_command`

```c
esp_err_t pn532_tg_get_initiator_command(pn532_handle_t h,
                                         uint8_t *buf, size_t buf_size,
                                         size_t *out_len,
                                         uint32_t timeout_ms);
```

Retrieve raw command from NFC initiator in ISO14443-4 PICC emulation.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Command received |
| `ESP_ERR_INVALID_ARG` | NULL args |
| `ESP_ERR_INVALID_SIZE` | Data too large for buffer |
| `ESP_FAIL` | PN532 error status |

#### `pn532_tg_response_to_initiator`

```c
esp_err_t pn532_tg_response_to_initiator(pn532_handle_t h,
                                         const uint8_t *data, size_t data_len,
                                         uint32_t timeout_ms);
```

Send response to initiator in ISO14443-4 PICC emulation. Max 262 bytes.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Response sent |
| `ESP_ERR_INVALID_ARG` | NULL handle, or NULL data with non-zero len |
| `ESP_ERR_INVALID_SIZE` | data_len > 262 |
| `ESP_FAIL` | PN532 error status |

#### `pn532_tg_set_meta_data`

```c
esp_err_t pn532_tg_set_meta_data(pn532_handle_t h,
                                 const uint8_t *data, size_t data_len,
                                 uint32_t timeout_ms);
```

Set meta-data to append to next TgResponseToInitiator/TgSetData payload. Max 262 bytes.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Meta-data set |
| `ESP_ERR_INVALID_ARG` | NULL handle, or NULL data with non-zero len |
| `ESP_ERR_INVALID_SIZE` | data_len > 262 |
| `ESP_FAIL` | PN532 error status |

---

## Core Driver (`pn532.h`)

### Types

#### `pn532_handle_t`

```c
typedef struct pn532_t *pn532_handle_t;
```

Opaque handle to a PN532 driver instance. Created by `pn532_init`, destroyed by `pn532_deinit`.

#### `pn532_transport_ops_t`

```c
typedef struct {
    esp_err_t (*write)(void *ctx, const uint8_t *buf, size_t len);
    esp_err_t (*read_status)(void *ctx, uint8_t *status_out);
    esp_err_t (*read_frame)(void *ctx, uint8_t *buf, size_t len);
    esp_err_t (*wait_ready)(void *ctx, uint32_t timeout_ms);
    void      (*destroy)(void *ctx);
    esp_err_t (*reset_device)(void *ctx, uint32_t pulse_ms, uint32_t settle_ms);
    esp_err_t (*bus_lock)(void *ctx, uint32_t timeout_ms);
    void      (*bus_unlock)(void *ctx);
} pn532_transport_ops_t;
```

Transport abstraction vtable. All callbacks receive the transport-private `ctx` returned by the backend factory.

| Callback | Description | When called |
|----------|-------------|-------------|
| `write` | Send a complete frame | `pn532_send_command`, NACK retransmit |
| `read_status` | Read 1-byte status (polling only) | Polling-mode `wait_ready` |
| `read_frame` | Read status + frame in one transaction | `pn532_read_ack`, `pn532_receive_response` |
| `wait_ready` | Block until chip has data ready | Before every frame read |
| `destroy` | Release all transport resources | `pn532_deinit` |
| `reset_device` | Assert hardware reset (optional) | `pn532_reset` |
| `bus_lock` | Acquire bus mutex (optional) | `pn532_reset` |
| `bus_unlock` | Release bus mutex (optional) | `pn532_reset` |

The last three callbacks are optional (NULL-safe). They are used by `pn532_reset` to bracket a hardware reset with bus mutex acquisition.

#### `pn532_config_t`

```c
typedef struct {
    const pn532_transport_ops_t *ops;
    void *transport_ctx;
} pn532_config_t;
```

Configuration for `pn532_init`. `ops` must outlive the handle.

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `PN532_MAX_PAYLOAD_LEN` | 265 | Max TFI+DATA bytes in any frame |

### Functions

#### `pn532_init`

```c
esp_err_t pn532_init(const pn532_config_t *cfg, pn532_handle_t *out_handle);
```

Allocate and initialise a driver instance. No bus traffic — just binds transport to handle.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Success |
| `ESP_ERR_INVALID_ARG` | NULL cfg, ops, out_handle, or missing write/read_frame/wait_ready |
| `ESP_ERR_NO_MEM` | Allocation failed |

#### `pn532_deinit`

```c
void pn532_deinit(pn532_handle_t handle);
```

Destroy handle and invoke transport's `destroy` callback. Safe to call with NULL.

#### `pn532_send_command`

```c
esp_err_t pn532_send_command(pn532_handle_t h,
                             uint8_t cmd_code,
                             const uint8_t *params, size_t params_len);
```

Build a command frame, send it, wait for ACK. Does **not** read the response.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Command sent and ACKed |
| `ESP_ERR_INVALID_ARG` | NULL handle, or NULL params with non-zero len |
| `ESP_ERR_TIMEOUT` | ACK not received within 100ms |
| `ESP_FAIL` | Received frame is not a valid ACK |

#### `pn532_receive_response`

```c
esp_err_t pn532_receive_response(pn532_handle_t h,
                                 uint8_t *buf, size_t buf_size,
                                 size_t *out_len,
                                 uint32_t timeout_ms);
```

Wait for and parse a response frame. On CRC failure, sends NACK to request retransmit.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Valid response parsed |
| `ESP_ERR_INVALID_ARG` | NULL h, buf, or out_len |
| `ESP_ERR_TIMEOUT` | No frame within timeout |
| `ESP_ERR_INVALID_CRC` | LCS or DCS mismatch (NACK sent automatically) |
| `ESP_ERR_INVALID_SIZE` | Payload too large for buffer |
| `ESP_FAIL` | PN532 error frame (TFI 0x7F) |

**Output**: `buf` receives the DATA payload (response command byte + parameters). `*out_len` is the number of bytes written.

#### `pn532_wakeup`

```c
esp_err_t pn532_wakeup(pn532_handle_t h);
```

Wake the PN532 from Power-Down / LowVbat. Sends 6 bytes of 0x55, waits 2ms for oscillator.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Wakeup succeeded |
| `ESP_ERR_INVALID_ARG` | NULL handle |
| `ESP_ERR_TIMEOUT` | Bus error (chip may still have woken) |

#### `pn532_reset`

```c
esp_err_t pn532_reset(pn532_handle_t h, uint32_t pulse_ms, uint32_t settle_ms);
```

Assert hardware reset on the PN532. Acquires the bus mutex before asserting reset and releases it after settle_ms completes. Typical values: pulse_ms = 10, settle_ms = 100.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Reset asserted successfully |
| `ESP_ERR_INVALID_ARG` | NULL handle |
| `ESP_ERR_NOT_SUPPORTED` | Transport has no reset pin |

#### `pn532_send_ack`

```c
esp_err_t pn532_send_ack(pn532_handle_t h);
```

Send a fixed 6-byte ACK frame to the PN532. Used to abort a command that is currently being processed. After sending ACK, the PN532 discontinues the current operation and returns to waiting for a new command. No response is sent back by the PN532.

The ACK frame uses the same pattern as the chip-to-host ACK: `0x00 0x00 0xFF 0x00 0xFF 0x00`. This is the existing `PN532_ACK_FRAME` constant already used by the core driver for ACK validation.

The function writes the frame and returns immediately — no `wait_ready`, no response read.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | ACK sent |
| `ESP_ERR_INVALID_ARG` | NULL handle |
| `ESP_ERR_TIMEOUT` | Bus write failed |

---

## I2C Transport (`pn532_i2c.h`)

### Types

#### `pn532_i2c_config_t`

```c
typedef struct {
    int        sda_gpio;   // SDA GPIO (required)
    int        scl_gpio;   // SCL GPIO (required)
    int        irq_gpio;   // IRQ GPIO; -1 = polling mode
    int        rst_gpio;   // RST GPIO; -1 = no hardware reset
    i2c_port_t port;       // I2C port (e.g. I2C_NUM_0)
    uint32_t   clk_speed;  // Hz; 0 = 400 kHz default
} pn532_i2c_config_t;
```

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `PN532_I2C_ADDRESS` | 0x24 | 7-bit I2C address |
| `PN532_I2C_DEFAULT_CLK_HZ` | 400000 | Default clock (400 kHz fast mode) |

### Functions

#### `pn532_i2c_create`

```c
esp_err_t pn532_i2c_create(const pn532_i2c_config_t *cfg,
                           pn532_transport_ops_t *ops_out,
                           void **ctx_out);
```

Create an I2C transport. Initialises bus, device, probe, IRQ, mutex.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Success |
| `ESP_ERR_INVALID_ARG` | NULL args or invalid GPIOs |
| `ESP_ERR_NO_MEM` | Allocation failed |
| `ESP_ERR_TIMEOUT` | PN532 not found on bus (probe failed) |
| Other `esp_err_t` | I2C or GPIO driver error |

#### `pn532_i2c_destroy`

```c
void pn532_i2c_destroy(void *ctx);
```

Destroy transport. Tears down IRQ ISR, I2C device, I2C bus, mutex. Safe with NULL.

#### `pn532_i2c_reset_device`

```c
esp_err_t pn532_i2c_reset_device(void *ctx, uint32_t pulse_ms, uint32_t settle_ms);
```

Assert hardware reset on the PN532. Drives rst_gpio LOW for pulse_ms milliseconds, then HIGH. After releasing reset, waits settle_ms milliseconds for oscillator stabilisation.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Reset asserted |
| `ESP_ERR_NOT_SUPPORTED` | No rst_gpio configured (rst_gpio = -1) |
