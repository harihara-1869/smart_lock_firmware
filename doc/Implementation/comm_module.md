# Communication Module Facade — API Reference

The Communication Module facade is the Application Module’s only public entry
point into the NFC communication stack. It wires the Session, Transport, LLI,
and PN532 components together while keeping their internal handles and
configuration private from the Application Module.

This document describes the facade API and ownership contract only. For
protocol behavior, state transitions, cryptography, APDU framing, hardware
bindings, and conformance requirements, refer to:

- [`smartlock_communication_module_spec.pdf`](../Specifications/smartlock_communication_module_spec.pdf)
- [`Comms Module Master Reference Document.md`](Comms%20Module%20Master%20Reference%20Document.md)
- [`session.md`](session.md)
- [`transport.md`](transport.md)
- [`lli.md`](lli.md)
- The PN532 component documentation in [`../../components/pn532/docs/`](../../components/pn532/docs/)

## Public header

```c
#include "comm_module.h"
```

The implementation is provided by the `comm_module` ESP-IDF component.

## Error type

```c
typedef enum {
    COMM_OK = 0,
    COMM_ERR_TIMEOUT,
    COMM_ERR_INVALID_ARG,
    COMM_ERR_INTERNAL,
} comm_err_t;
```

`COMM_ERR_TIMEOUT` indicates that no reader or APDU arrived within the
configured timeout. Other transport, LLI, hardware, or initialization
failures are reported as `COMM_ERR_INTERNAL`.

## Application callback

The Application Module registers one plaintext command handler:

```c
typedef session_err_t (*comm_app_command_handler_t)(
    const uint8_t *plaintext_in, size_t len_in,
    uint8_t *plaintext_out, size_t *len_out,
    void *app_ctx);
```

The callback receives authenticated, decrypted Application bytes and writes
the plaintext response that the Session module will encrypt. It must keep
`*len_out` within the supplied 227-byte response capacity.

The callback must not inspect or construct APDUs, status words, nonces, or
ciphertext. Application command semantics are defined by Part II of the
consolidated specification.

## Configuration

```c
typedef struct {
    int sda_gpio;
    int scl_gpio;
    int irq_gpio;                 /* -1 for polling */
    int rst_gpio;                 /* -1 if not wired */
    i2c_port_t i2c_port;
    uint32_t i2c_clk_hz;          /* 0 = 400 kHz default */

    uint8_t sens_res[2];
    uint8_t nfcid1[3];
    uint8_t sel_res;
    uint8_t nfcid2[8];
    uint8_t pad[8];
    uint8_t system_code[2];
    uint8_t nfcid3t[10];
    uint8_t gt[47];
    uint8_t gt_len;               /* 0–47 */
    uint8_t tk[47];
    uint8_t tk_len;               /* 0–47 */

    uint32_t handshake_timeout_ms; /* 0 = 2000 ms */
    uint32_t activate_timeout_ms;  /* 0 = 30000 ms */
    uint32_t apdu_timeout_ms;      /* 0 = 2000 ms */

    uint8_t local_sk[64];
    uint8_t local_pk[32];
    session_peer_key_provider_t peer_key_provider;
    void *peer_key_provider_ctx;

    comm_app_command_handler_t app_handler;
    void *app_handler_ctx;
} comm_module_config_t;
```

`peer_key_provider` and `app_handler` are required. The facade copies the
identity and transport configuration during initialization. The Session
module owns all ephemeral and derived key erasure; callers must not modify or
reuse a live handle’s internal state.

## Handle

```c
typedef struct comm_module_t *comm_module_handle_t;
```

The handle is opaque. It owns one Session handle and one Transport handle.

## Lifecycle API

### `comm_module_init`

```c
comm_err_t comm_module_init(const comm_module_config_t *cfg,
                            comm_module_handle_t *out);
```

Validates required callbacks, creates the Session, constructs the lower-layer
configuration, and creates Transport. On failure, all partially initialized
resources are released.

### `comm_module_deinit`

```c
comm_err_t comm_module_deinit(comm_module_handle_t handle);
```

Releases the Transport and Session resources. Passing `NULL` is safe and
returns `COMM_OK`.

## Session execution API

### `comm_module_run_once`

```c
comm_err_t comm_module_run_once(comm_module_handle_t handle);
```

Runs one complete communication-session loop: activation, handshake, secure
payload processing, teardown, and link release. Call it repeatedly from the
Application task. A timeout is recoverable; the caller may invoke it again.

### `comm_module_run_forever`

```c
comm_err_t comm_module_run_forever(comm_module_handle_t handle);
```

Repeatedly invokes `comm_module_run_once`, continuing across reader and APDU
timeouts. It returns only for an invalid argument or non-recoverable internal
error.

### `comm_module_is_session_active`

```c
bool comm_module_is_session_active(comm_module_handle_t handle);
```

Returns true only when Transport is in `TRANSPORT_STATE_SECURE_SESSION` and a
fresh LLI link-status check reports an active link. The Application should use
this immediately before any state-changing actuator operation.

### `comm_module_force_abort`

```c
comm_err_t comm_module_force_abort(comm_module_handle_t handle);
```

Erases active Session material, aborts the LLI exchange, releases the NFC
link, and returns the Transport to its idle state. Passing `NULL` returns
`COMM_ERR_INVALID_ARG`.

## Ownership and layering

The facade owns and wires the lower layers as follows:

```text
Application Module
        │ plaintext callback
        ▼
Communication Module facade
        ├── Session
        └── Transport ── LLI ── PN532
```

Session and Application are collaborating peers. The Application owns command
dispatch, authorization policy, and actuator behavior; Session owns
authentication and secure messaging. The facade only connects their callback
and lifecycle contracts.

Do not include `transport.h`, `lli.h`, or PN532 headers in Application code.
