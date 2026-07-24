# Link Layer Interface (LLI) — Implementation

The LLI sits directly above the PN532 driver and below the transport layer. It
abstracts the PN532 card-emulation protocol behind a clean C API that exposes
no NFC controller internals. A caller works entirely in terms of APDU
byte-buffers and link status queries.

## Position in the Stack

```
┌──────────────────────────┐
│     Transport Layer      │  C-APDU parsing, state machine, crypto callbacks
├──────────────────────────┤
│           LLI            │  ← this component
├──────────────────────────┤
│   PN532 Command Layer    │  TgInitAsTarget, TgGetData, TgSetData, …
├──────────────────────────┤
│    PN532 Core Driver     │  Frame format, checksums, ACK, error recovery
├──────────────────────────┤
│   PN532 I2C Transport    │  I2C master, IRQ/polling, bus recovery
├──────────────────────────┤
│      ESP32-S3 (I2C)      │
└──────────────────────────┘
```

The LLI imports only `pn532.h`, `pn532_cmd.h`, and `pn532_i2c.h`.  It never
leaks PN532 types through its own public header (`lli.h`), so an application
that uses the LLI directly does not need to include any PN532 header.

---

## Internal State

```c
struct lli_t {
    pn532_handle_t pn532;
    void           *i2c_ctx;
    pn532_transport_ops_t ops;

    /* Card identity cached from lli_config_t at init time. */
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
```

The struct is allocated once by `lli_init` and freed by `lli_deinit`.  No
dynamic allocation occurs after init completes.

---

## Initialisation Sequence

`lli_init` performs these steps in order.  On any failure after allocation the
function cleans up in reverse order and returns `LLI_ERR_INTERNAL`.

| Step | Call | Purpose |
|------|------|---------|
| 1 | `pn532_i2c_create` | Bring up I2C bus, add PN532 device, configure IRQ/RST GPIOs |
| 2 | `pn532_init` | Bind transport vtable to a new driver handle (no bus traffic) |
| 3 | `pn532_wakeup` | Send 0x55 burst, wait 2 ms for oscillator start |
| 4 | `pn532_get_firmware_version` | Validate IC == 0x32; log version/caps |
| 5 | `pn532_sam_configuration` | SAM Normal mode, IRQ enabled |
| 6 | `pn532_set_parameters` | Enable `AUTO_ATR_RES` + `ISO14443_4_PICC` |
| 7 | — | Copy card identity fields from `lli_config_t` into the handle |

The firmware check comes before any configuration command so that a wrong chip
or absent PN532 is caught early.

---

## Function Details

### `lli_activate`

1. Sends `GetGeneralStatus` (0x04) to check the external RF field flag.  If
   the field byte is 0x01 a reader's field is already present — the function
   returns `LLI_ERR_INTERNAL` without entering card-emulation mode.
2. Builds a `pn532_tg_init_params_t` from the cached card identity.  Mode is
   fixed to `PN532_TG_MODE_PICC_ONLY` (ISO-DEP card emulation only, no DEP).
3. Calls `pn532_tg_init_as_target`.  This blocks until an NFC reader
   activates the PN532 or the timeout elapses.

Error mapping:

| `pn532_tg_init_as_target` result | LLI result |
|---|---|
| `ESP_ERR_TIMEOUT` | `LLI_ERR_TIMEOUT` |
| Any other error | `LLI_ERR_INTERNAL` |

### `lli_receive_apdu`

Uses the raw command layer (`pn532_send_command` + `pn532_receive_response`)
instead of the high-level `pn532_tg_get_data` wrapper.  This is necessary
because the high-level wrapper abstracts away the PN532 status byte, and the
LLI must map it to distinct error codes.

Command 0x86 (TgGetData) is sent, then the response is parsed:

| Response byte | Meaning |
|---|---|
| `resp[0]` | 0x87 (TgGetData response code) |
| `resp[1]` | PN532 status byte |
| `resp[2..]` | Payload data |

Status byte mapping:

| Status bits 0–5 | LLI result | Data copied? |
|---|---|---|
| 0x00 | `LLI_OK` | Yes |
| 0x01 | `LLI_ERR_TIMEOUT` | No |
| 0x29 (TARGET_RELEASED) | `LLI_ERR_LINK_RELEASED` | No |
| Anything else | `LLI_ERR_FRAME_INTEGRITY` | No |

On any non-OK return, `*len_out` is zeroed before the function returns.  The
PN532 handles MI-bit chaining internally — the LLI receives the fully
reassembled payload.

### `lli_send_apdu`

Thin wrapper around `pn532_tg_set_data`.  Returns `LLI_ERR_SEND_FAILED` on
any error.  Maximum payload is 262 bytes (PN532 limit).

### `lli_get_link_status`

Calls `pn532_tg_get_target_status` with a 100 ms timeout.  Returns
`lli_link_status_t` directly (not `lli_err_t`).

| PN532 state | LLI result |
|---|---|
| `PICC_ACTIVATED` (0x81) | `LLI_STATUS_ACTIVE` |
| `PICC_DESELECTED` (0x82) | `LLI_STATUS_ACTIVE` |
| `PICC_RELEASED` (0x80) | `LLI_STATUS_RELEASED` |
| `IDLE` (0x00) | `LLI_STATUS_RELEASED` |
| I2C/bus error | `LLI_STATUS_ERROR` |
| Unknown state | `LLI_STATUS_ERROR` (logged at WARN) |

`PICC_DESELECTED` maps to ACTIVE because a deselect/reselect cycle is normal
ISO14443-4 behaviour — the reader has paused the session but not departed.

### `lli_abort`

Best-effort teardown sequence.  Always returns `LLI_OK`.

1. `pn532_send_ack` — abort any in-progress PN532 command.
2. `InRelease` (0x52, Tg=0x00) — release all targets.  Sent through the raw
   command layer because no high-level wrapper exists.
3. `pn532_wakeup` — the chip may have entered Power-Down after the release;
   this guarantees it is awake for the next `lli_activate` call.

Steps 1 and 2 are best-effort: failures are logged at DEBUG but do not cause
early return.

---

## Using the LLI Standalone

The LLI can be used without the transport layer for applications that need
direct APDU control.  A minimal example:

```c
#include "lli.h"

static const lli_config_t cfg = {
    .sda_gpio = 8, .scl_gpio = 9,
    .irq_gpio = 10, .rst_gpio = 11,
    .i2c_port = 0,
    .sens_res = {0x04, 0x00},
    .nfcid1   = {0x01, 0x02, 0x03},
    .sel_res  = 0x20,
    /* ... remaining fields zeroed ... */
};

void app_main(void)
{
    lli_handle_t h;
    lli_init(&cfg, &h);

    while (1) {
        if (lli_activate(h, 30000) != LLI_OK)
            continue;

        uint8_t buf[264];
        size_t  len;
        while (lli_get_link_status(h) == LLI_STATUS_ACTIVE) {
            if (lli_receive_apdu(h, buf, sizeof(buf), &len, 5000) != LLI_OK)
                break;

            /* Process buf[0..len-1] as a C-APDU. */
            uint8_t sw[] = {0x90, 0x00};
            lli_send_apdu(h, sw, sizeof(sw), 2000);
        }

        lli_abort(h);
    }

    lli_deinit(h);
}
```

---

## API Reference

### Types

#### `lli_config_t`

Configuration passed to `lli_init`.  All fields are value-copied; the caller
does not need to keep the struct alive after init returns.

| Field | Type | Description |
|-------|------|-------------|
| `sda_gpio` | `int` | I2C SDA pin |
| `scl_gpio` | `int` | I2C SCL pin |
| `irq_gpio` | `int` | PN532 IRQ pin; `-1` for polling mode |
| `rst_gpio` | `int` | PN532 RST pin; `-1` if not wired |
| `i2c_port` | `int` | I2C port number (e.g. `I2C_NUM_0`) |
| `sens_res[2]` | `uint8_t` | ATQA, LSB first |
| `nfcid1[3]` | `uint8_t` | 3-byte NFCID1 (single-size UID) |
| `sel_res` | `uint8_t` | SAK byte (`0x20` = ISO14443-4) |
| `nfcid2[8]` | `uint8_t` | 8-byte NFCID2 (FeliCa) |
| `pad[8]` | `uint8_t` | FeliCa padding |
| `system_code[2]` | `uint8_t` | FeliCa system code |
| `nfcid3t[10]` | `uint8_t` | NFCID3t for ATR_RES in DEP mode |
| `gt[47]` | `uint8_t` | General bytes (Gt) |
| `gt_len` | `uint8_t` | Length of Gt (0–47) |
| `tk[47]` | `uint8_t` | Historical bytes (Tk / ATS) |
| `tk_len` | `uint8_t` | Length of Tk (0–47) |

#### `lli_link_status_t`

| Value | Meaning |
|-------|---------|
| `LLI_STATUS_ACTIVE` | Reader is connected and session is live |
| `LLI_STATUS_RELEASED` | Reader has departed or link is idle |
| `LLI_STATUS_ERROR` | Bus error or unknown state |

#### `lli_err_t`

| Value | Code | Meaning |
|-------|------|---------|
| `LLI_OK` | 0 | Success |
| `LLI_ERR_TIMEOUT` | 1 | Operation timed out |
| `LLI_ERR_FRAME_INTEGRITY` | 2 | PN532 frame checksum or status error |
| `LLI_ERR_SEND_FAILED` | 3 | `TgSetData` failed |
| `LLI_ERR_NOT_SUPPORTED` | 4 | Feature not supported |
| `LLI_ERR_INVALID_ARG` | 5 | NULL argument |
| `LLI_ERR_INTERNAL` | 6 | Internal / transport error |
| `LLI_ERR_LINK_RELEASED` | 7 | Reader departed mid-session |

#### `lli_handle_t`

Opaque handle.  Created by `lli_init`, destroyed by `lli_deinit`.

### Functions

#### `lli_init`

```c
lli_err_t lli_init(const lli_config_t *cfg, lli_handle_t *handle_out);
```

Allocate and initialise an LLI instance.  Returns `LLI_ERR_INVALID_ARG` if
`cfg` or `handle_out` is NULL.

#### `lli_deinit`

```c
lli_err_t lli_deinit(lli_handle_t handle);
```

Tear down the LLI instance and free resources.  NULL-safe.

#### `lli_activate`

```c
lli_err_t lli_activate(lli_handle_t handle, uint32_t timeout_ms);
```

Enter card-emulation mode and block until a reader activates the PN532 or the
timeout elapses.

#### `lli_receive_apdu`

```c
lli_err_t lli_receive_apdu(lli_handle_t handle, uint8_t *buf, size_t buf_len,
                           size_t *len_out, uint32_t timeout_ms);
```

Receive the next APDU from the reader.  On success `*len_out` receives the
payload length.  On any error `*len_out` is zeroed.

#### `lli_send_apdu`

```c
lli_err_t lli_send_apdu(lli_handle_t handle, const uint8_t *data,
                        size_t len, uint32_t timeout_ms);
```

Send an APDU to the reader.  Maximum 262 bytes.

#### `lli_get_link_status`

```c
lli_link_status_t lli_get_link_status(lli_handle_t handle);
```

Query the current link state.  Returns `lli_link_status_t` directly.

#### `lli_abort`

```c
lli_err_t lli_abort(lli_handle_t handle);
```

Best-effort teardown: ACK → InRelease → wakeup.  Always returns `LLI_OK`.
