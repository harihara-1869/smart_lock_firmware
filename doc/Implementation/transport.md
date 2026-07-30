# Transport Layer — Implementation

The transport layer sits above the LLI and below the application.  It
implements the NFC mutual-authentication state machine: reader activation,
handshake message exchange, secure-session dispatch, and session teardown.
It passes opaque byte payloads up and down — it never inspects plaintext,
ciphertext, signatures, or keys.  All cryptographic work is delegated to the
application through callbacks.

## Position in the Stack

```
┌──────────────────────────┐
│   Application Module     │  dispatch / authorization / AAI (external peer)
├──────────────────────────┤
│      Session Layer       │  Mutual-auth handshake, AES-256-GCM, secure erase
├──────────────────────────┤
│     Transport Layer      │  ← this component
├──────────────────────────┤
│           LLI            │  card emulation, APDU I/O
├──────────────────────────┤
│   PN532 Command Layer    │  NFC commands
├──────────────────────────┤
│    PN532 Core Driver     │  frame format, checksums
├──────────────────────────┤
│   PN532 I2C Transport    │  I2C master, IRQ/polling
├──────────────────────────┤
│      ESP32-S3 (I2C)      │
└──────────────────────────┘
```

The transport layer imports only `lli.h`.  It has zero knowledge of PN532
commands, I2C, or any controller-specific type.

---

## C-APDU / R-APDU Format

All NFC data exchange uses ISO 7816-4 command/response pairs:

### Command APDU (C-APDU) — reader to lock

| Byte(s) | Field | Description |
|---------|-------|-------------|
| 1 | CLA | Class byte (must be `0x80`) |
| 1 | INS | Instruction code |
| 1 | P1 | Parameter 1 |
| 1 | P2 | Parameter 2 |
| 1 | Lc | Length of data field |
| Lc | Data | Command payload |

Minimum 5 bytes (no data).  The transport layer validates CLA, Lc, and INS.
Unknown INS values receive SW=0x6A 0x81 (not supported).

### Response APDU (R-APDU) — lock to reader

| Byte(s) | Field | Description |
|---------|-------|-------------|
| 0–255 | Data | Response payload (may be empty) |
| 1 | SW1 | Status word, high byte |
| 1 | SW2 | Status word, low byte |

---

## INS Codes

Defined internally in `transport.c`, not exposed in the header:

| INS | Name | Used in state |
|-----|------|---------------|
| `0x10` | `CMD_HANDSHAKE_INIT` | ACTIVATED (M1) |
| `0x11` | `CMD_HANDSHAKE_FINISH` | HANDSHAKE (M3) |
| `0x20` | `CMD_SECURE_PAYLOAD` | SECURE_SESSION |
| `0x30` | `CMD_SESSION_ABORT` | ACTIVATED, HANDSHAKE, SECURE_SESSION |

---

## Status Words

| SW1 | SW2 | Mnemonic | Meaning |
|-----|-----|----------|---------|
| `0x90` | `0x00` | OK | Success |
| `0x69` | `0x82` | AUTH_FAILED | Signature/tag verification failed |
| `0x69` | `0x85` | WRONG_STATE | INS not valid in current state |
| `0x6A` | `0x80` | INVALID_DATA | Lc mismatch or payload too large |
| `0x6A` | `0x81` | NOT_SUPPORTED | Unknown INS or CLA != 0x80 |

---

## Payload Size Budget

The maximum plaintext that can be sent through a secure session is:

```
PLAINTEXT_BUDGET = 255 - 12 (GCM nonce) - 16 (MAC tag) = 227 bytes
```

If a `CMD_SECURE_PAYLOAD` C-APDU has Lc > 227, the transport replies with
SW=0x6A 0x80 and stays in SECURE_SESSION (does not erase).

---

## State Machine

`transport_run_session` is a blocking call that runs one complete session.  It
returns when the session ends for any reason.  The caller loops to accept the
next session.

```
                 lli_activate()
    IDLE ─────────────────────────► ACTIVATED
      ▲                               │
      │ timeout/error                 │ receive M1 (INS=0x10)
      │ (return to caller)            │ on_apdu → build M2
      │                               │ send M2
      │                               ▼
      │                          HANDSHAKE
      │                               │
      │                     ┌─────────┴──────────┐
      │                     │                    │
      │              timer/link-lost       receive M3 (INS=0x11)
      │              erase → RELEASED      on_apdu → verify signature
      │                     │                    │
      │                     │             ┌──────┴──────┐
      │                     │             │             │
      │                     │          ok           invalid
      │                     │          │               │
      │                     │          ▼               ▼
      │                     │   SECURE_SESSION    erase → RELEASED
      │                     │          │
      │                     │    ┌─────┴──────┐
      │                     │    │            │
      │                     │  link-lost   exchange payloads
      │                     │  erase         │
      │                     │    │       (loop until abort/link-lost)
      │                     │    ▼            │
      │                     ▼    ▼            ▼
      │◄─────────────────── RELEASED ◄───────┘
      │                     │
      │                     │ lli_abort()
      │                     │ state → IDLE
      └─────────────────────┘
```

### IDLE

Calls `lli_activate` with `activate_timeout_ms`.  On timeout, returns
`TRANSPORT_ERR_TIMEOUT` to the caller — the caller is expected to loop and
call `transport_run_session` again.

### ACTIVATED (awaiting M1)

Receives one C-APDU.  If the receive fails for any reason (timeout, link lost,
frame error), invokes the erase callback and transitions to RELEASED.

Parses the C-APDU.  If INS is `0x30` (SESSION_ABORT), invokes the erase
callback, sends SW=0x90 0x00 (best-effort), and transitions to RELEASED.

If INS is not `0x10` (HANDSHAKE_INIT) and not `0x30`, sends SW=0x69 0x85
and stays in ACTIVATED — the reader may retry.

Passes the valid M1 to the `on_apdu` callback.  The application fills the
R-APDU with M2.  If the callback returns an error, sends SW=0x69 0x82, erases,
and transitions to RELEASED.

Sends M2.  If the send fails, erases and transitions to RELEASED.

Starts the handshake timer (`handshake_timeout_ms`) and transitions to
HANDSHAKE.

### HANDSHAKE (awaiting M3)

Each iteration:
1. Check `lli_get_link_status` — if RELEASED, erase and transition.
2. Check handshake timer — if expired, erase and transition.
3. Compute remaining time and call `lli_receive_apdu` with that timeout.

Receive error handling:
- `LINK_RELEASED` or `TIMEOUT` → erase, transition to RELEASED.
- `FRAME_INTEGRITY` → log and retry (the reader may retransmit).

C-APDU handling:
- `INS_SESSION_ABORT` (0x30) → erase, send SW=0x90 0x00, transition to
  RELEASED.
- Anything other than `INS_HANDSHAKE_FINISH` (0x11) → send SW=0x69 0x85, stay
  in HANDSHAKE (no erase).
- `INS_HANDSHAKE_FINISH` → pass to `on_apdu` callback.  If the callback
  returns error (signature invalid), send SW=0x69 0x82, erase, transition to
  RELEASED.  If ok, send SW=0x90 0x00, transition to SECURE_SESSION.

### SECURE_SESSION (per-exchange loop)

Each iteration:
1. Check `lli_get_link_status` — if RELEASED, erase and transition.
2. Call `lli_receive_apdu`.  Any error → erase, transition to RELEASED.

C-APDU handling:
- `INS_SESSION_ABORT` (0x30) → erase, best-effort send SW=0x90 0x00,
  transition to RELEASED.
- Anything other than `INS_SECURE_PAYLOAD` (0x20) → send SW=0x69 0x85, stay.
- `INS_SECURE_PAYLOAD` with Lc > 227 → send SW=0x6A 0x80, stay.
- Valid `INS_SECURE_PAYLOAD` → pass to `on_apdu` callback.  If error (GCM tag
  mismatch), send SW=0x69 0x82, erase, transition to RELEASED.  If ok, send
  the R-APDU from the callback, stay in SECURE_SESSION.

### RELEASED

Calls `lli_abort()`, resets state to IDLE, returns `TRANSPORT_OK` to the
caller.

---

## Secure-Erase Invariant

The `on_erase` callback is always invoked **before** the state is set to
RELEASED, never after.  This guarantees that key material is destroyed before
any code could observe the RELEASED state and potentially skip the erase.

The callback is invoked exactly once per session teardown path.

---

## Using the Transport Layer Standalone

The transport layer can be used without the application crypto layer by
providing stub callbacks.  A minimal example:

```c
#include "transport.h"
#include <string.h>

static transport_err_t handle_apdu(const transport_capdu_t *capdu,
                                   transport_rapdu_t *rapdu, void *ctx)
{
    /* Echo the command data back as the response. */
    memcpy(rapdu->data, capdu->data, capdu->lc);
    rapdu->len = capdu->lc;
    rapdu->sw1 = 0x90;
    rapdu->sw2 = 0x00;
    return TRANSPORT_OK;
}

static void handle_erase(void *ctx) { /* wipe keys here */ }

void app_main(void)
{
    transport_config_t cfg = {
        .lli_cfg = {
            .sda_gpio = 8, .scl_gpio = 9,
            .irq_gpio = 10, .rst_gpio = 11,
            .i2c_port = 0,
            .sens_res = {0x04, 0x00},
            .nfcid1   = {0x01, 0x02, 0x03},
            .sel_res  = 0x20,
        },
        .handshake_timeout_ms = 2000,
        .activate_timeout_ms  = 30000,
        .apdu_timeout_ms      = 5000,
        .on_apdu   = handle_apdu,
        .on_erase  = handle_erase,
    };

    transport_handle_t h;
    transport_init(&cfg, &h);

    while (1) {
        transport_run_session(h);
    }

    transport_deinit(h);
}
```

---

## API Reference

### Types

#### `transport_state_t`

| Value | Description |
|-------|-------------|
| `TRANSPORT_STATE_IDLE` | Not in a session |
| `TRANSPORT_STATE_ACTIVATED` | Post RATS/ATS, awaiting M1 |
| `TRANSPORT_STATE_HANDSHAKE` | M1 sent, awaiting M3 |
| `TRANSPORT_STATE_SECURE_SESSION` | Authenticated, secure payloads only |
| `TRANSPORT_STATE_RELEASED` | Session ended, keys erased |

#### `transport_err_t`

| Value | Code | Description |
|-------|------|-------------|
| `TRANSPORT_OK` | 0 | Success |
| `TRANSPORT_ERR_TIMEOUT` | 1 | Activation timed out (no reader) |
| `TRANSPORT_ERR_LINK_LOST` | 2 | NFC link dropped |
| `TRANSPORT_ERR_INVALID_STATE` | 3 | Call in wrong state |
| `TRANSPORT_ERR_INVALID_APDU` | 4 | Malformed C-APDU |
| `TRANSPORT_ERR_PAYLOAD_TOO_LARGE` | 5 | Payload exceeds plaintext budget |
| `TRANSPORT_ERR_INTERNAL` | 6 | Internal error |

#### `transport_capdu_t`

| Field | Type | Description |
|-------|------|-------------|
| `cla` | `uint8_t` | Class byte (0x80) |
| `ins` | `uint8_t` | Instruction code |
| `p1` | `uint8_t` | Parameter 1 |
| `p2` | `uint8_t` | Parameter 2 |
| `lc` | `uint8_t` | Data length |
| `data[255]` | `uint8_t` | Command data |

#### `transport_rapdu_t`

| Field | Type | Description |
|-------|------|-------------|
| `data[256]` | `uint8_t` | Response data |
| `len` | `uint16_t` | Response data length (0–256) |
| `sw1` | `uint8_t` | Status word high byte |
| `sw2` | `uint8_t` | Status word low byte |

Callback-produced responses are serialized on the wire as `Data || SW1 || SW2`,
so the maximum short R-APDU is 258 bytes. Status-only error paths send the two
status bytes directly.

#### `transport_apdu_handler_t`

```c
typedef transport_err_t (*transport_apdu_handler_t)(
    const transport_capdu_t *capdu,
    transport_rapdu_t *rapdu,
    void *user_ctx
);
```

Called for every valid C-APDU.  The application fills `rapdu` and returns
`TRANSPORT_OK`, or returns an error which the transport layer maps to an
appropriate status word.

#### `transport_erase_handler_t`

```c
typedef void (*transport_erase_handler_t)(void *user_ctx);
```

Called when secure-erase must be performed.  The application destroys all
ephemeral key material.

#### `transport_config_t`

| Field | Type | Description |
|-------|------|-------------|
| `lli_cfg` | `lli_config_t` | Passed through to `lli_init` |
| `handshake_timeout_ms` | `uint32_t` | THS — recommended 2000 ms |
| `activate_timeout_ms` | `uint32_t` | How long to wait for a reader |
| `apdu_timeout_ms` | `uint32_t` | Per-APDU receive timeout |
| `on_apdu` | `transport_apdu_handler_t` | Called for every valid C-APDU |
| `on_erase` | `transport_erase_handler_t` | Called on every secure-erase trigger |
| `user_ctx` | `void *` | Opaque pointer passed to callbacks |

#### `transport_handle_t`

Opaque handle.  Created by `transport_init`, destroyed by `transport_deinit`.

### Functions

#### `transport_init`

```c
transport_err_t transport_init(const transport_config_t *cfg,
                               transport_handle_t *handle_out);
```

Allocate and initialise a transport instance.  Calls `lli_init` internally.
Returns `TRANSPORT_ERR_INVALID_APDU` if `cfg` or `handle_out` is NULL.

#### `transport_deinit`

```c
transport_err_t transport_deinit(transport_handle_t handle);
```

Tear down the transport instance.  Calls `lli_deinit` internally.  NULL-safe.

#### `transport_run_session`

```c
transport_err_t transport_run_session(transport_handle_t handle);
```

Blocking call that runs one complete session (activate → exchanges → release).
Returns when the session ends for any reason.  Returns `TRANSPORT_ERR_TIMEOUT`
if no reader is detected; `TRANSPORT_OK` if a session completed normally.
The caller loops to accept the next session.

#### `transport_get_state`

```c
transport_state_t transport_get_state(transport_handle_t handle);
```

Query the current transport state.  Returns `TRANSPORT_STATE_IDLE` for a NULL
handle.
