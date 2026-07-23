# Core Driver (`pn532.c`)

## Overview

Transport-agnostic PN532 protocol implementation. Knows the PN532 frame format, checksums, and command/response handshake. Talks to the chip exclusively through the `pn532_transport_ops_t` vtable.

**Source**: `components/pn532/src/pn532.c`
**Header**: `components/pn532/include/pn532.h`

## PN532 Frame Format

### Normal Information Frame

```
Byte:  0x00  0x00 0xFF  LEN  LCS  TFI  DATA[0..N-1]  DCS  0x00
       ────  ──────────  ───  ───  ───  ────────────  ───  ────
       pre   start code  │    │    │    payload       │    post
                         │    │    │                  │
                    info_len  ~LEN  0xD4/0xD5    ~(TFI+sum(DATA))+1
                    = 1+N+1        +1 mod 256
```

| Field | Size | Description |
|-------|------|-------------|
| PREAMBLE | 1 byte | Always 0x00 |
| STARTCODE | 2 bytes | Always 0x00 0xFF |
| LEN | 1 byte | Number of bytes in TFI+DATA (1 to 254 for normal frames) |
| LCS | 1 byte | Length checksum: `(LEN + LCS) & 0xFF == 0`, i.e. `LCS = (~LEN + 1) & 0xFF` |
| TFI | 1 byte | Direction indicator: 0xD4 = host→PN532, 0xD5 = PN532→host |
| DATA | N bytes | Command/response payload |
| DCS | 1 byte | Data checksum: `(TFI + DATA[0] + ... + DATA[N-1] + DCS) & 0xFF == 0` |
| POSTAMBLE | 1 byte | Always 0x00 |

**LEN range**: 1 (no DATA, just TFI) to 254 (253 DATA bytes). If `info_len > 254`, use an extended frame.

### Extended Information Frame

```
0x00  0x00 0xFF  0xFF 0xFF  LEN_HI  LEN_LO  LCS  TFI  DATA[0..N-1]  DCS  0x00
```

- The two 0xFF bytes serve as a marker distinguishing extended from normal frames
- `LEN_HI:LEN_LO` is a 16-bit big-endian info length (up to 265 = max payload)
- LCS covers both length bytes: `(LEN_HI + LEN_LO + LCS) & 0xFF == 0`

### ACK Frame (fixed pattern)

```
0x00  0x00  0xFF  0x00  0xFF  0x00
```

Sent by the PN532 to acknowledge receipt of a command. Always 6 bytes.

### NACK Frame (fixed pattern)

```
0x00  0x00  0xFF  0xFF  0x00  0x00
```

Sent by the host to request retransmission after a CRC error.

### Error Frame (from PN532)

```
0x00  0x00  0xFF  0x01  0xFF  0x7F  0x81  0x00
```

Identified by TFI = 0x7F. Indicates an application-level error in the PN532.

## Buffer Sizes

| Constant | Value | Purpose |
|----------|-------|---------|
| `PN532_MAX_PAYLOAD_LEN` | 265 | Max TFI+DATA bytes (extended frame limit) |
| `PN532_TX_BUF_SIZE` | 275 | Worst-case outgoing frame buffer |
| `PN532_RX_BUF_SIZE` | 276 | Worst-case incoming read (status + frame) |

## Frame Builder (`pn532_build_frame`)

Serialises a command into a PN532 information frame.

**Input**: `cmd_code` (e.g. 0x02 for GetFirmwareVersion), `params[]`, `params_len`
**Output**: complete frame bytes in `out[]`, byte count in `*out_len`

**Algorithm**:

1. Compute `data_len = 1 + params_len` (cmd + params)
2. Compute `info_len = 1 + data_len` (TFI + cmd + params)
3. If `info_len > PN532_MAX_PAYLOAD_LEN` → error
4. Write preamble + start code
5. If `info_len > 254`: write extended frame header (0xFF 0xFF + 2-byte len + LCS)
6. Else: write normal frame header (1-byte len + LCS)
7. Write TFI (0xD4) + cmd_code + params, accumulating checksum
8. Write DCS + postamble

**Checksum formulas**:
- LCS (normal): `(~LEN + 1) & 0xFF`
- LCS (extended): `(~(LEN_HI + LEN_LO) + 1) & 0xFF`
- DCS: `(~(TFI + sum of all DATA bytes) + 1) & 0xFF`

## Frame Parser (`pn532_parse_frame`)

Parses a received frame and extracts the DATA payload.

**Input**: raw bytes from transport (including status byte at `raw[0]`)
**Output**: DATA payload in `out[]`, byte count in `*out_len`

**Algorithm**:

1. Skip status byte (`raw[0]`)
2. Check for error frame (TFI = 0x7F) → return `ESP_FAIL`
3. Scan for start code (0x00 0xFF) — handles preamble padding
4. Read LEN (and detect extended marker 0xFF 0xFF)
5. Validate LCS: `(LEN + LCS) & 0xFF == 0`
6. Read TFI — must be 0xD5 (PN532→host)
7. Read DATA + DCS, validate: `(TFI + DATA + DCS) & 0xFF == 0`
8. Copy DATA to output buffer (info_len - 1 bytes, dropping TFI)

**Error returns**:
- `ESP_ERR_INVALID_RESPONSE` — no start code, bad TFI, truncated frame
- `ESP_ERR_INVALID_CRC` — LCS or DCS mismatch
- `ESP_ERR_INVALID_SIZE` — payload doesn't fit in output buffer
- `ESP_FAIL` — error frame (TFI 0x7F)

### Start Code Scanner (`pn532_find_start`)

The PN532 may emit leading 0x00 bytes before the start code. This function scans for the 0x00 0xFF pattern:

```c
for (size_t i = 0; i + 1 < len; i++) {
    if (buf[i] == 0x00 && buf[i + 1] == 0xFF) {
        *off = i + 2;  // offset just past start code
        return ESP_OK;
    }
}
```

### Fixed Frame Matcher (`pn532_match_fixed`)

Matches ACK/NACK frames by finding the start code and comparing the 3 bytes that follow against the pattern.

### Error Frame Detector (`pn532_is_error_frame`)

After the start code, checks for `LEN=0x01, LCS=0xFF, TFI=0x7F` — the PN532 error frame signature.

## ACK Handshake (`pn532_read_ack`)

After sending a command, the PN532 replies with an ACK before processing:

1. `tp_wait_ready(timeout)` — block until chip signals readiness
2. `tp_read_frame(raw, 7)` — read status + 6 ACK bytes in one transaction
3. `pn532_match_fixed()` — validate against the ACK pattern

**Timeout**: 100ms (`PN532_ACK_TIMEOUT_MS`)

## Command/Response Flow

### `pn532_send_command`

1. Build frame via `pn532_build_frame()`
2. Write frame via `tp_write()`
3. Wait for and validate ACK via `pn532_read_ack()`

Does **not** read the response — that's a separate call.

### `pn532_receive_response`

1. `tp_wait_ready(timeout_ms)` — block until chip has response ready
2. `tp_read_frame(raw, PN532_RX_BUF_SIZE)` — over-read status + worst-case frame in one transaction
3. `pn532_parse_frame()` — validate and extract DATA payload
4. On `ESP_ERR_INVALID_CRC`: send NACK to request retransmit (best-effort, ignores write result)

### `pn532_wakeup`

1. Write 6 bytes of 0x55 — wakes chip from Power-Down / LowVbat mode
2. Wait 2ms for oscillator stabilisation (T_osc_start worst case)
3. Returns the write error (but logs a warning rather than failing — chip may wake even on NACK)

### `pn532_reset`

Asserts hardware reset on the PN532 while holding the bus mutex to prevent I2C transactions during reset.

1. Check that transport provides `reset_device` callback — return `ESP_ERR_NOT_SUPPORTED` if not
2. Acquire bus mutex via `bus_lock` callback (5-second timeout)
3. Call `reset_device(ctx, pulse_ms, settle_ms)` — drives RST LOW for pulse_ms, then HIGH, waits settle_ms
4. Release bus mutex via `bus_unlock` callback

**Typical values**: pulse_ms = 10, settle_ms = 100

**Returns**:
- `ESP_OK` — reset asserted successfully
- `ESP_ERR_INVALID_ARG` — NULL handle
- `ESP_ERR_NOT_SUPPORTED` — transport has no reset pin

**Note**: the caller is responsible for re-running SAMConfiguration after reset.

### `pn532_send_ack`

Sends a fixed 6-byte ACK frame from host to PN532. Used to abort a command that is currently being processed — the PN532 discontinues the operation and returns to waiting for a new command. No response is sent back by the PN532 after an abort ACK.

This reuses the same `PN532_ACK_FRAME` constant that `pn532_read_ack` uses for validation:

```
0x00  0x00  0xFF  0x00  0xFF  0x00
```

**Implementation**:
1. Validate handle is not NULL
2. Write the 6-byte ACK frame via `tp_write()`
3. Return immediately — no `wait_ready`, no response read

**Returns**:
- `ESP_OK` — ACK sent
- `ESP_ERR_INVALID_ARG` — NULL handle
- `ESP_ERR_TIMEOUT` — bus write failed

**Use case**: fault recovery when the PN532 is stuck mid-command (e.g. after a timeout in TgInitAsTarget). Sending ACK puts the chip back into a known idle state.

## Frame Resync (`resync_frame`)

When `pn532_parse_frame` returns `ESP_ERR_INVALID_CRC`, the core driver attempts a frame resync before giving up.

**Purpose**: drain and discard incoming bytes from the PN532 until a valid preamble sequence is found (0x00 0x00 0xFF), or until timeout elapses.

**Algorithm**:

1. Maintain a 3-byte sliding window
2. Read one byte at a time using `tp_read_frame` (single-byte over-reads)
3. When the window matches `{0x00, 0x00, 0xFF}` return ESP_OK
4. Hard cap: 512 bytes maximum. If consumed with no preamble found, return ESP_FAIL
5. If timeout elapses first, return ESP_ERR_TIMEOUT

**Integration with `pn532_receive_response`**:

1. `pn532_parse_frame()` returns `ESP_ERR_INVALID_CRC`
2. Log at WARN: "checksum mismatch — attempting frame resync"
3. Call `resync_frame(h, timeout_ms)`
4. If resync succeeds (ESP_OK): retry the receive exactly once
5. If resync fails or retry still fails: return original `ESP_ERR_INVALID_CRC`

**Note**: resync is only called on CRC errors, not on ESP_ERR_TIMEOUT.

## Handle Structure

```c
struct pn532_t {
    const pn532_transport_ops_t *ops;   // transport vtable (borrowed, not owned)
    void *ctx;                          // transport-private context
};
```

Minimal — just binds a transport to the protocol layer. Allocated by `pn532_init`, freed by `pn532_deinit`.
