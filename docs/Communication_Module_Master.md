# Communication Module — Master Reference

**Scope:** PN532 I2C Transport → PN532 Core Driver → PN532 Command Layer → LLI
→ Transport → Session → Comm Module Facade → provisioning (§11). The
Application Module (lock state, motor control, tamper detection, sensor
monitoring, business logic, authorization policy — and, per §11, the
Provision Secret, QR display, and authorized-key storage) is an external
peer and its internals are out of scope here — §10 and §11 cover only the
contract it must honor at the facade boundary. As of this revision, the
facade (§8) hands commands and session events to the Application via a
mailbox + task-notification handoff rather than direct callbacks, so that
the Communication Module never executes application code on its own task.

**Status of each layer**, per the current implementation:

| Layer | Status | Source docs |
|---|---|---|
| PN532 I2C Transport | Built | `i2c-transport.md`, `api-reference.md` |
| PN532 Core Driver | Built | `core-driver.md`, `api-reference.md` |
| PN532 Command Layer | Built | `command-layer.md`, `api-reference.md` |
| LLI | Built | `lli.md` |
| Transport | Built | `transport.md` |
| Session | Built | `session.md` |
| Comm Module Facade | Built | `comm_module.md` |

The current repository builds the PN532, LLI, Transport, Session, and Comm
Module Facade layers, but does not yet provide the production Application Module.
The Comm Module Facade (§8) is built per `comm_module.md` and `comm_module.h`;
where the as-built code diverges from §8's original design, the implementation is
the source of truth. The firmware entry point in `main/smart_lock_firmware.c` exercises
the full end-to-end stack by initializing the Comm Module Facade and running a smoke test application task.

---

## 1. Layer Stack and Ownership

```
┌─────────────────────────────────────────────────────────────────┐
│                      Application Module (peer)                   │
│         dispatch / authorization / AAI — NOT part of this module │
└───────────────────────────────┬───────────────────────────────────┘
                                │ plaintext OPCODE/ARGS in, status/ARGS out
┌───────────────────────────────┴───────────────────────────────────┐
│                     COMMUNICATION MODULE                           │
│                                                                     │
│  comm_module.h  ◄── Communication Module facade (as built)          │
│       │                                                             │
│  ┌────┴─────────────────────────────────────────────────────┐     │
│  │ Session (session.h/.c)                                    │     │
│  │   identity, 3-message handshake, HKDF, AES-256-GCM,        │     │
│  │   peer-key resolution, secure erase                        │     │
│  │   implements: transport_apdu_handler_t, transport_erase_   │     │
│  │   handler_t  — registers into Transport                       │     │
│  └────┬─────────────────────────────────────────────────────┘     │
│       │ transport_capdu_t / transport_rapdu_t (opaque to Session   │
│       │ w.r.t. framing, but Session reads/writes their fields)     │
│  ┌────┴─────────────────────────────────────────────────────┐     │
│  │ Transport (transport.h/.c)                                 │     │
│  │   APDU framing, INS validity per state, THS timer,         │     │
│  │   status-word dictionary, state machine                     │     │
│  └────┬─────────────────────────────────────────────────────┘     │
│       │ lli_* calls only                                           │
│  ┌────┴─────────────────────────────────────────────────────┐     │
│  │ LLI (lli.h/.c)                                             │     │
│  │   lli_activate / lli_receive_apdu / lli_send_apdu /         │     │
│  │   lli_get_link_status / lli_abort                          │     │
│  └────┬─────────────────────────────────────────────────────┘     │
│  ┌────┴─────────────────────────────────────────────────────┐     │
│  │ PN532 Command Layer (pn532_cmd.h/.c)                       │     │
│  │   TgInitAsTarget, TgGetData, TgSetData, TgGetTargetStatus,  │     │
│  │   InListPassiveTarget, TgGetInitiatorCommand, …             │     │
│  └────┬─────────────────────────────────────────────────────┘     │
│  ┌────┴─────────────────────────────────────────────────────┐     │
│  │ PN532 Core Driver (pn532.h/.c)                             │     │
│  │   frame build/parse, checksums, ACK, wakeup, resync, reset  │     │
│  └────┬─────────────────────────────────────────────────────┘     │
│  ┌────┴─────────────────────────────────────────────────────┐     │
│  │ PN532 I2C Transport (pn532_i2c.h/.c)                       │     │
│  │   bus/device, IRQ/polling, bus recovery, hw reset           │     │
│  └─────────────────────────────────────────────────────────────┘     │
└─────────────────────────────────────────────────────────────────────┘
```

**Rule that must hold at every seam:** a layer only calls the API of the
layer immediately beneath it. Session never calls `lli_*` or `pn532_*`
directly; Transport never calls `pn532_*` directly. The facade (`comm_module.h`) is
the only header exposed to the Application Module. This boundary is preserved
across all implemented layers.

---

## 2. PN532 I2C Transport — as built

`pn532_i2c.h` — implements `pn532_transport_ops_t` over ESP-IDF's
`i2c_master.h`.

```c
esp_err_t pn532_i2c_create(const pn532_i2c_config_t *cfg,
                           pn532_transport_ops_t *ops_out,
                           void **ctx_out);
void      pn532_i2c_destroy(void *ctx);
esp_err_t pn532_i2c_reset_device(void *ctx, uint32_t pulse_ms, uint32_t settle_ms);
```

```c
typedef struct {
    int        sda_gpio;
    int        scl_gpio;
    int        irq_gpio;   // -1 => polling mode
    int        rst_gpio;   // -1 => no hardware reset
    i2c_port_t port;
    uint32_t   clk_speed;  // 0 => 400 kHz default
} pn532_i2c_config_t;
```

Key behaviors to remember when anything above it misbehaves:

- **Bus recovery is automatic.** After 5 failed write retries,
  `pn532_i2c_write` calls `recover_i2c_bus()` and, if `rst_gpio` is wired,
  a full `pn532_i2c_reset_device()`. Higher layers observe only the result of
  the operation and may reapply PN532 configuration on the next activation.
- IRQ mode and polling mode are both supported and selected purely by
  `irq_gpio >= 0`; nothing above the I2C layer needs to know which is active.
- `scl_wait_us = 50000` tolerates PN532 clock-stretching without any
  retry logic elsewhere.

Nothing needs to change here for Session to be built. Treat this layer as
frozen.

---

## 3. PN532 Core Driver — as built

`pn532.h` — transport-agnostic frame protocol.

```c
typedef struct pn532_t *pn532_handle_t;

typedef struct {
    esp_err_t (*write)(void *ctx, const uint8_t *buf, size_t len);
    esp_err_t (*read_status)(void *ctx, uint8_t *status_out);
    esp_err_t (*read_frame)(void *ctx, uint8_t *buf, size_t len);
    esp_err_t (*wait_ready)(void *ctx, uint32_t timeout_ms);
    void      (*destroy)(void *ctx);
    esp_err_t (*reset_device)(void *ctx, uint32_t pulse_ms, uint32_t settle_ms); // optional
    esp_err_t (*bus_lock)(void *ctx, uint32_t timeout_ms);                       // optional
    void      (*bus_unlock)(void *ctx);                                          // optional
} pn532_transport_ops_t;

typedef struct {
    const pn532_transport_ops_t *ops;
    void *transport_ctx;
} pn532_config_t;

esp_err_t pn532_init(const pn532_config_t *cfg, pn532_handle_t *out_handle);
void      pn532_deinit(pn532_handle_t handle);
esp_err_t pn532_send_command(pn532_handle_t h, uint8_t cmd_code,
                             const uint8_t *params, size_t params_len);
esp_err_t pn532_receive_response(pn532_handle_t h, uint8_t *buf, size_t buf_size,
                                 size_t *out_len, uint32_t timeout_ms);
esp_err_t pn532_wakeup(pn532_handle_t h);
esp_err_t pn532_reset(pn532_handle_t h, uint32_t pulse_ms, uint32_t settle_ms);
esp_err_t pn532_send_ack(pn532_handle_t h);
```

`pn532_send_command` + `pn532_receive_response` is the pair LLI's
`lli_receive_apdu` uses **directly**, bypassing the command layer's
`pn532_tg_get_data` — see §5 for why.

CRC-error handling (`resync_frame`) and reset are transparent below this
layer's callers; nothing above needs new logic to benefit from them.

Nothing needs to change here for Session. Treat this layer as frozen.

---

## 4. PN532 Command Layer — as built

`pn532_cmd.h` — one wrapper per PN532 command, still transport-agnostic
(uses only `pn532_send_command`/`pn532_receive_response`).

```c
esp_err_t pn532_get_firmware_version(pn532_handle_t h, pn532_firmware_version_t *out);
esp_err_t pn532_sam_configuration(pn532_handle_t h, pn532_sam_mode_t mode,
                                  uint8_t timeout_50ms, bool irq_enabled);
esp_err_t pn532_set_parameters(pn532_handle_t h, uint8_t flags);

esp_err_t pn532_tg_init_as_target(pn532_handle_t h,
                                  const pn532_tg_init_params_t *params,
                                  pn532_tg_init_result_t *result,
                                  uint32_t timeout_ms);
esp_err_t pn532_tg_get_data(pn532_handle_t h, uint8_t *buf, size_t buf_size,
                            size_t *out_len, uint32_t timeout_ms);
esp_err_t pn532_tg_set_data(pn532_handle_t h, const uint8_t *data,
                            size_t data_len, uint32_t timeout_ms);
esp_err_t pn532_tg_get_target_status(pn532_handle_t h, pn532_tg_status_t *out,
                                     uint32_t timeout_ms);

// secondary / not used by the current LLI, kept for completeness:
esp_err_t pn532_in_list_passive_target(...);
esp_err_t pn532_tg_get_initiator_command(...);
esp_err_t pn532_tg_response_to_initiator(...);
esp_err_t pn532_tg_set_meta_data(...);
```

Important asymmetry to keep in mind: **`pn532_tg_get_data` collapses the
PN532 status byte into a plain `esp_err_t`** (`ESP_FAIL` for any nonzero
status). LLI needs the *specific* status byte (timeout vs. target-released
vs. anything-else) to satisfy the Hardware/Link-Layer spec's error taxonomy,
so it does **not** call `pn532_tg_get_data` — it drives
`pn532_send_command(h, 0x86, NULL, 0)` +
`pn532_receive_response(h, buf, cap, &len, timeout)` itself and reads
`buf[1]` directly. This is intentional. MI-bit reassembly is implemented in
the ESP32-side `pn532_cmd.c` wrapper, not in PN532 firmware. The current raw
LLI path remains safe because the protocol maximum C-APDU is 261 bytes while
one `TgGetData` response carries 262 bytes, so chaining cannot occur under
the current protocol limits.

`pn532_tg_init_as_target`'s `result` struct is intentionally thin:

```c
typedef struct { uint8_t mode; } pn532_tg_init_result_t;
```

It does **not** surface the RATS bytes the PN532 auto-consumed while
establishing ISO-DEP activation — that's correct, because Transport/Session
only care about the first real C-APDU (M1), which arrives later via
`lli_receive_apdu` → `TgGetData`, not via `TgInitAsTarget`'s own response.

The command layer is complete for the current LLI use case. Its
`pn532_tg_get_data` wrapper includes ESP32-side MI-bit reassembly for callers
that need it; LLI intentionally uses the raw core-driver path to preserve the
PN532 status byte.

---

## 5. LLI — as built

`lli.h` — the sole PN532-shaped surface above the drivers, matching the
Hardware/Link-Layer spec's five normative primitives exactly.

```c
typedef enum {
    LLI_OK = 0, LLI_ERR_TIMEOUT, LLI_ERR_FRAME_INTEGRITY, LLI_ERR_SEND_FAILED,
    LLI_ERR_NOT_SUPPORTED, LLI_ERR_INVALID_ARG, LLI_ERR_INTERNAL,
    LLI_ERR_LINK_RELEASED,
} lli_err_t;

typedef enum { LLI_STATUS_ACTIVE, LLI_STATUS_RELEASED, LLI_STATUS_ERROR } lli_link_status_t;

typedef struct lli_t *lli_handle_t;

lli_err_t lli_init(const lli_config_t *cfg, lli_handle_t *handle_out);
lli_err_t lli_deinit(lli_handle_t handle);
lli_err_t lli_activate(lli_handle_t handle, uint32_t timeout_ms);
lli_err_t lli_receive_apdu(lli_handle_t handle, uint8_t *buf, size_t buf_len,
                          size_t *len_out, uint32_t timeout_ms);
lli_err_t lli_send_apdu(lli_handle_t handle, const uint8_t *data,
                       size_t len, uint32_t timeout_ms);
lli_link_status_t lli_get_link_status(lli_handle_t handle);
lli_err_t lli_abort(lli_handle_t handle);
```

Status-byte → taxonomy mapping (`lli_receive_apdu`, per `lli.md` §"lli_receive_apdu"):

| PN532 status byte | LLI result |
|---|---|
| `0x00` | `LLI_OK` |
| `0x01` (timeout) | `LLI_ERR_TIMEOUT` |
| `0x29` (target released) | `LLI_ERR_LINK_RELEASED` |
| anything else | `LLI_ERR_FRAME_INTEGRITY` (fail closed) |

Link-status mapping (`lli_get_link_status`):

| PN532 `pn532_tg_state_t` | LLI result |
|---|---|
| `PICC_ACTIVATED` (0x81) | `LLI_STATUS_ACTIVE` |
| `PICC_DESELECTED` (0x82) | `LLI_STATUS_ACTIVE` (pause, not departure) |
| `PICC_RELEASED` (0x80) / `IDLE` (0x00) | `LLI_STATUS_RELEASED` |
| I2C/bus error | `LLI_STATUS_ERROR` |

`lli_abort`: Perform local driver state cleanup only (always returns `LLI_OK`).
Unsolicited ACK and `InRelease` (`0x52`) were removed to prevent $\text{T}_{\text{osc\_start}}$
bus wedging; target state is naturally overwritten by `SAMConfiguration` + `TgInitAsTarget`
on the subsequent session activation loop.

The LLI is implemented and is the only layer above the PN532 stack that
exposes card-emulation operations. It validates the configured general and
historical-byte lengths, initializes the PN532, reapplies ISO-DEP target
configuration on activation, and maps PN532 target status/error codes to the
LLI taxonomy shown above.

---

## 6. Transport — as built

`transport.h` — ISO-DEP-shaped state machine, framing, INS dispatch, THS.
Zero PN532 knowledge, zero crypto knowledge.

```c
typedef enum {
    TRANSPORT_STATE_IDLE, TRANSPORT_STATE_ACTIVATED, TRANSPORT_STATE_HANDSHAKE,
    TRANSPORT_STATE_SECURE_SESSION, TRANSPORT_STATE_RELEASED,
} transport_state_t;

typedef enum {
    TRANSPORT_OK = 0, TRANSPORT_ERR_TIMEOUT, TRANSPORT_ERR_LINK_LOST,
    TRANSPORT_ERR_INVALID_STATE, TRANSPORT_ERR_INVALID_APDU,
    TRANSPORT_ERR_PAYLOAD_TOO_LARGE, TRANSPORT_ERR_INTERNAL,
} transport_err_t;

typedef struct {
    uint8_t cla, ins, p1, p2, lc;
    uint8_t data[255];
} transport_capdu_t;

typedef struct {
    uint8_t data[256];
    uint16_t len;
    uint8_t sw1, sw2;
} transport_rapdu_t;

Callback-produced responses are serialized as `Data || SW1 || SW2`; the
maximum short R-APDU is therefore 258 bytes. Status-only error responses send
the two status bytes directly.

typedef transport_err_t (*transport_apdu_handler_t)(
    const transport_capdu_t *capdu, transport_rapdu_t *rapdu, void *user_ctx);
typedef void (*transport_erase_handler_t)(void *user_ctx);

typedef struct {
    lli_config_t lli_cfg;
    uint32_t handshake_timeout_ms;   // THS, recommended 2000
    uint32_t activate_timeout_ms;
    uint32_t apdu_timeout_ms;
    transport_apdu_handler_t on_apdu;
    transport_erase_handler_t on_erase;
    void *user_ctx;
} transport_config_t;

typedef struct transport_t *transport_handle_t;

transport_err_t transport_init(const transport_config_t *cfg, transport_handle_t *handle_out);
transport_err_t transport_deinit(transport_handle_t handle);
transport_err_t transport_run_session(transport_handle_t handle);
transport_state_t transport_get_state(transport_handle_t handle);
```

The single `on_apdu` callback is sufficient for Session — it does not need to be split into
per-INS hooks. Transport already validates INS-vs-state and only forwards
APDUs that are legal to receive in the current state;
Session's implementation of `on_apdu` just switches on `capdu->ins`
internally (0x10 → build M2, 0x11 → verify M3, 0x20 → decrypt/dispatch/
encrypt) and returns `TRANSPORT_OK` or an error. Transport is already wired
to accept these callbacks; the remaining integration work is constructing
both handles from an application-level facade.

`on_erase` is invoked before every RELEASED transition, exactly once per
teardown path, per the secure-erase invariant already documented in
`transport.md`. Session's `on_erase` implementation must be idempotent,
since it can legitimately be called from ACTIVATED (no keys exist yet — a
no-op), HANDSHAKE (ephemeral keys exist — wipe them), or SECURE_SESSION
(ephemeral keys + directional AES keys exist — wipe them all).

---

## 7. Session — as built

`session.h/.c` implements exactly two function pointers that plug directly
into `transport_config_t` with **no adapter shim**:

```c
transport_err_t session_on_apdu(const transport_capdu_t *capdu,
                                transport_rapdu_t *rapdu, void *ctx);
void            session_on_erase(void *ctx);
```

`ctx` is the `session_handle_t`, passed through as `transport_config_t.user_ctx`.

### 7.1 Config and lifecycle

```c
typedef struct session_t *session_handle_t;

typedef enum {
    SESSION_OK = 0,
    SESSION_ERR_INVALID_ARG,
    SESSION_ERR_AUTH_FAILED,     // signature or AEAD tag mismatch
    SESSION_ERR_INTERNAL,
} session_err_t;

// Application Module's plaintext handler — OPCODE+ARGS in, status+ARGS out.
typedef session_err_t (*session_app_cmd_handler_t)(
    const uint8_t *plaintext_in, size_t len_in,
    uint8_t *plaintext_out, size_t *len_out,
    void *app_ctx);

// Enumerates candidate long-term Ed25519 public keys for M3 verification.
// Called with index = 0, 1, 2, ... up to SESSION_MAX_PEER_CANDIDATES (64),
// or until it returns false.
typedef bool (*session_peer_key_provider_t)(
    size_t index, uint8_t pubkey_out[32], void *provider_ctx);

typedef struct {
    // Local long-term identity (Lock's own Ed25519 keypair).
    // Raw bytes today; the private crypto seam is the migration point for
    // a future hardware-backed signing implementation.
    uint8_t local_sk[64];
    uint8_t local_pk[32];

    session_peer_key_provider_t peer_key_provider;
    void *peer_key_provider_ctx;

    session_app_cmd_handler_t app_handler;
    void *app_handler_ctx;
} session_config_t;

session_err_t session_init(const session_config_t *cfg, session_handle_t *out);
session_err_t session_deinit(session_handle_t h);
```

> **Who actually supplies `app_handler`?** Under the mailbox architecture
> (§8), this function pointer is **not** application code — it's a small
> trampoline owned and implemented by the Comms facade
> (`comm_module_dispatch_via_mailbox`, §8.3) that populates the mailbox,
> notifies the Application task, and blocks for its response. Session's
> contract here is completely unchanged: it calls whatever `app_handler`
> was registered at `session_init` time and treats it as an opaque
> synchronous function. It has no idea — and doesn't need to know — that
> the function on the other side now hands off to a different task instead
> of running inline. This is precisely what keeps "Session never executes
> application code" true without Session itself changing at all.

### 7.2 Why a resolver, and how it actually resolves identity

Worth being explicit about this, because it's the one place the protocol
itself is easy to misread: **M1 carries no identity hint at all** — per the
session spec (§4.2/§4.3), M1 = `(pk_eph_P, c_P)` only. There is no key ID,
no phone ID, nothing that tells the Lock in advance *which* provisioned
public key to try when it later verifies `Sig_P` in M3.

That means the resolver cannot look anything up by name — it can only
**enumerate candidates**, and M3 verification becomes "try each candidate
`PK_P` against `Sig_P` until one verifies, or exhaust the list and fail."
`session_peer_key_provider_t` above is written for exactly that: it's an
iterator (`index` 0..N), not a lookup-by-identity function. `session_on_m3`
must:

1. Build the transcript `pk_eph_P ‖ pk_eph_L ‖ c_P ‖ c_L` (domain-separated,
   §7.3 below).
2. Loop `index = 0, 1, 2, ...` up to `SESSION_MAX_PEER_CANDIDATES` (64),
   calling the provider each time. Reaching the bound is treated exactly like
   the provider returning `false`.
3. For each candidate `PK_P`, attempt Ed25519 verification against `Sig_P`.
4. On the first success, that candidate is the authenticated identity for
   this session — proceed to key derivation.
5. If the provider returns `false`, or the bound is reached, with no
   successful verification, treat it identically to a single bad signature:
   the callback returns a non-OK transport error, which Transport maps to
   `0x69 0x82` + erase.

This is the same trust model as an `authorized_keys` file: cheap per-key
verification (~ms), a realistically small candidate count (owner + a
handful of guests), and it fits inside the ~1s WTX budget with room to
spare even at a few dozen candidates. It also means "authentication implies
authorization" (per the Application spec's §4) still holds unmodified — any
candidate that verifies is, by definition, an authorized long-term identity,
regardless of which index it was found at.

If the candidate list ever grows large enough that linear scan becomes a
real cost, that's an Application-layer ACL concern (session spec §9.1,
application spec §9.2), not something to solve inside Session — the
provider interface doesn't change, only what backs it (flat array today,
indexed store or backend lookup later).

### 7.3 Handshake and secure-payload handling

```c
// Called when capdu->ins == 0x10 (CMD_HANDSHAKE_INIT), state == ACTIVATED
static transport_err_t session_handle_m1(session_handle_t h,
                                         const uint8_t *m1, size_t m1_len,
                                         transport_rapdu_t *rapdu);
// Called when capdu->ins == 0x11 (CMD_HANDSHAKE_FINISH), state == HANDSHAKE
static transport_err_t session_handle_m3(session_handle_t h,
                                         const uint8_t *m3, size_t m3_len,
                                         transport_rapdu_t *rapdu);
// Called when capdu->ins == 0x20 (CMD_SECURE_PAYLOAD), state == SECURE_SESSION
static transport_err_t session_handle_secure_payload(session_handle_t h,
                                                     const uint8_t *in, size_t in_len,
                                                     transport_rapdu_t *rapdu);
```

`session_on_apdu` is a thin switch over these three, keyed on `capdu->ins`
— Transport has already rejected anything not valid for the current state,
so the `default` branch here is defensive-only and should never fire in
practice.

**`session_handle_m1`:**
1. Validate `m1_len == 64` (32-byte `pk_eph_P` ‖ 32-byte `c_P`); anything
   else → `TRANSPORT_ERR_INVALID_APDU` (Transport maps to `0x6A 0x80`).
2. Reject `pk_eph_P` if it's the all-zero point (X25519 contributory-behavior
   check, per session spec §4.4 rule 3).
3. Generate ephemeral X25519 pair `(pk_eph_L, sk_eph_L)`, fresh CSPRNG
   challenge `c_L`.
4. Build the domain-separated transcript
   `"SLOCK-HS-v1" ‖ version_byte ‖ pk_eph_P ‖ pk_eph_L ‖ c_P ‖ c_L` and sign
   it with `local_sk` → `Sig_L`.
5. Write `pk_eph_L ‖ c_L ‖ Sig_L` (128 bytes) into `rapdu`, `sw1/sw2 = 0x90 0x00`.
6. Cache `pk_eph_P`, `c_P`, `sk_eph_L`, `pk_eph_L`, `c_L` in the session
   handle for use at M3.

**`session_handle_m3`:**
1. Validate `m3_len == 64` (`Sig_P`); wrong length →
   `TRANSPORT_ERR_INVALID_APDU`.
2. Rebuild the same transcript from cached M1/M2 values.
3. Run the resolver loop from §7.2. No match → a non-OK transport error.
4. On match: `SharedSecret = X25519(sk_eph_L, pk_eph_P)`;
   `PRK = HKDF-Extract(salt = c_P ‖ c_L, ikm = SharedSecret)`;
   `K_p2e = HKDF-Expand(PRK, "phone->esp", 32)`;
   `K_e2p = HKDF-Expand(PRK, "esp->phone", 32)`.
5. Empty `rapdu`, `sw1/sw2 = 0x90 0x00`.
6. On any failure at step 3: still write `sw1/sw2 = 0x69 0x82` into `rapdu`
   before returning the error — Transport sends this R-APDU and *then*
   erases and transitions, per the existing state-machine behavior for
   `HANDSHAKE`.

**`session_handle_secure_payload`:**
1. Parse `in` as `nonce(12) ‖ ciphertext ‖ tag(16)`. Transport currently
   enforces `Lc ≤ 227` for the complete encrypted payload; after the 28-byte
   GCM overhead this permits up to 199 plaintext bytes on the active wire
   path. Session's internal plaintext buffer remains sized for 227 bytes.
2. AES-256-GCM decrypt with `K_p2e`. Tag mismatch → a non-OK transport
   error (maps to `0x69 0x82` + erase, per transport spec §7: "An AES-GCM
   authentication tag mismatch MUST be treated identically to a handshake
   signature failure").
3. On success, call `app_handler(plaintext, len, out_plain, &out_len,
   app_handler_ctx)`. Session does not interpret `plaintext` — it is purely
   a courier here, per the session spec's §10 collaboration contract.
4. Encrypt `out_plain` with `K_e2p` + fresh CSPRNG nonce → write
   `nonce ‖ ciphertext ‖ tag` into `rapdu`, `sw1/sw2 = 0x90 0x00`.
5. Application-level failures must be encoded in the plaintext response and
   the handler must still return `SESSION_OK`. A non-`SESSION_OK` return is
   reserved for genuine internal failures; Session then returns an empty
   encrypted payload while Transport returns `sw1/sw2 = 0x90 0x00`. Only
   crypto-level failures (step 2) terminate the session.

**`session_on_erase`:** `session_crypto_zeroize` clears the cached M1/M2
values, ephemeral keys, challenges, and directional keys, then returns the
stage to `EMPTY`. It is idempotent and safe to call on a session that never
got past ACTIVATED. The long-term identity survives erase and is cleared by
`session_deinit`.

### 7.4 Session lifecycle events — new, additive seam

Nothing in Session as built today tells anyone *when* a session becomes
authenticated or when it ends — `session_on_apdu` returns per-APDU, and
`session_on_erase` fires on every teardown regardless of how far the
session got. Neither is "a session now exists" or "a session just ended,"
and this is exactly what the facade needs to expose to the Application (see
§8.2). Session is the right place to add it, not Transport: Session's
`stage` field (`EMPTY`/`EPHEMERAL`/`ESTABLISHED`, §7 above) already knows
precisely when the real transition happens — Transport has no equivalent
concept, it only knows APDU-level states.

Two new **optional** fields on `session_config_t`, both NULL-safe, neither
changing any existing signature:

```c
typedef void (*session_event_handler_t)(void *event_ctx);

typedef struct {
    // ... all existing fields unchanged ...
    session_event_handler_t on_established;  // fires once, stage EPHEMERAL -> ESTABLISHED
    session_event_handler_t on_terminated;   // fires once, only if stage was ESTABLISHED at erase time
    void *event_ctx;
} session_config_t;
```

Wiring, no new call sites beyond the two existing handlers:

- **`on_established`** is called from the tail of `session_handle_m3`,
  after key derivation succeeds and the stage is set to `ESTABLISHED`, but
  before returning `TRANSPORT_OK` to Transport. If NULL, skipped.
- **`on_terminated`** is called from inside `session_on_erase`, guarded by
  `if (stage == ESTABLISHED) on_terminated(event_ctx);` *before* the stage
  is reset to `EMPTY`. This means it does **not** fire for a session that
  never got past ACTIVATED or HANDSHAKE (no real session existed to end),
  only for one that actually reached SECURE_SESSION at some point. If NULL,
  skipped.

Both callbacks run on whatever task is currently inside
`transport_run_session` — the same task, the same call stack, as
`on_apdu`. They must be fast and non-blocking for the same reason
`app_handler` must be: they're inline in the middle of the state machine,
not dispatched asynchronously.

---

## 8. Comm Module Facade — as built

### 8.0 Where the IRQ actually is, and why the facade doesn't touch it

Worth stating plainly, because it's easy to assume the facade needs its own
interrupt handling: **there is exactly one PN532 IRQ pin in this entire
module, it is already fully owned and wired by `pn532_i2c_create` (§2), and
nothing above the I2C transport layer — not the core driver, not LLI, not
Transport, not Session, and not the facade — should ever touch a GPIO or
install an ISR of their own.**

What the existing IRQ actually does: `pn532_i2c_ctx_t` installs a
falling-edge ISR (`pn532_irq_isr`) that does one thing —
`xSemaphoreGiveFromISR(c->irq_sem, ...)`. That's it. It's a "the PN532 has
something ready" doorbell, nothing more. The task-level code that's
actually blocked — inside `pn532_i2c_wait_ready` → `wait_ready_irq` →
`xSemaphoreTake(irq_sem, timeout)` — is what "wakes up" when the ISR fires.
That blocked call could be sitting anywhere in the stack: inside
`TgInitAsTarget` waiting for a reader to tap, inside `TgGetData` waiting for
the next C-APDU, or inside the ACK/response read for any other command.
**The IRQ fires many times over the life of one session** (once per
reader-activation, then once per APDU exchanged) — it is not a single
"session created" event and was never designed to be one.

This has a direct consequence for where "establishing a session" runs:
handshake verification (Ed25519 verify, X25519, HKDF, AES-GCM) happens
inside `session_handle_m3`, which is called synchronously from
`session_on_apdu`, which is called synchronously from
`transport_run_session`, which blocks on `lli_receive_apdu`, which blocks
on the exact same IRQ semaphore described above. All of that — the crypto
included — necessarily executes in whatever **task** is running
`transport_run_session`. It cannot run in ISR context: ISRs on this
platform have a small dedicated stack, cannot call blocking FreeRTOS or
I2C APIs, and neither Monocypher nor mbedTLS are written to be
interrupt-safe. Trying to "have the ISR establish the session" would mean
moving multi-millisecond blocking I2C transactions and crypto into
interrupt context — not a design tweak, a rewrite that breaks the whole
stack's threading model.

**So: `comm_module_init` sets up zero ISRs.** The IRQ ISR is created once,
inside `pn532_i2c_create`, as a side effect of `lli_init` — exactly as it
already is today. What the facade *does* own is a **FreeRTOS task**, whose
entire body is a loop calling the already-blocking `transport_run_session`.
The IRQ is what makes that task's blocking calls efficient (semaphore wait,
not a busy-poll); the facade's job is task lifecycle, not interrupt
plumbing.

### 8.1 Why this section was rewritten: mailbox instead of direct callbacks

The design in the rest of this document (and in an earlier revision of
this section) had the facade call `app_handler` and
`on_session_established`/`on_session_ended` as plain C function pointers,
executed inline on the comm task. That violates a boundary worth being
strict about: **the Communication Module must never directly execute
application code.** A function pointer supplied by the Application and
invoked synchronously on the comm task's stack, inside the middle of the
ISO-DEP/crypto state machine, is exactly that — regardless of how thin the
wrapper looks. If application code — lock state checks, motor control,
authorization policy — ever grows a blocking call, a long computation, or a
bug, it now stalls the comm task, which stalls the NFC protocol timing
(`THS`, `apdu_timeout_ms`) underneath it.

The fix is a **mailbox + task-notification handoff**: the Application runs
on its own task, with its own stack, its own scheduling, and its own
ongoing responsibilities (tamper/integrity monitoring chief among them)
that must keep running whether or not a phone is anywhere nearby. The comm
task and the Application task each own a strict half of a small shared
struct, and pass control back and forth with `xTaskNotifyGive`/
`ulTaskNotifyTake` — no queues, no dynamic allocation, no mutex.

This does **not** change Session or Transport at all (per the note at the
end of §7.1) — only what the facade plugs into `session_config_t.app_handler`
and into the §7.4 event seam changes, from "call app code directly" to
"hand off via mailbox."

### 8.2 Mailbox

The mailbox struct itself is an **internal** type — it does not appear in
`comm_module.h`. The Application never sees its layout or holds a pointer
into it; it only calls accessor functions. This is deliberate: exposing the
raw struct (even as `const`) would either leak internal layout into the one
header the Application is supposed to depend on, or invite the Application
to write into fields out of turn, which is the one thing that would break
the handoff invariant below.

```c
// comm_module.c — internal only, never exposed in comm_module.h

typedef enum {
    COMM_APP_EVENT_NONE = 0,
    COMM_APP_EVENT_SESSION_STARTED,
    COMM_APP_EVENT_SESSION_ENDED,
} comm_app_event_t;

typedef struct {
    // Comms task writes these; Application task only reads them.
    bool    command_valid;
    uint8_t command[227];       // sized to the plaintext budget, §7.3
    size_t  command_length;
    comm_app_event_t pending_event;

    // Application task writes these; Comms task only reads them.
    uint8_t response[227];
    size_t  response_length;
} comm_mailbox_t;
```

**Hard invariant — no mutex protects this struct.** Its correctness relies
entirely on strict ping-pong ownership, enforced by the notification
handshake, never by locking:

- The comm task may only write `command*`/`pending_event` and then must
  not touch the mailbox again until it has been notified back.
- The Application task may only write `response*`, and only after it has
  been notified that a command (or event) is waiting, and must not touch
  the mailbox again until notified of the next one.
- `xTaskNotifyGive`/`ulTaskNotifyTake` already impose the memory barrier
  this handoff needs — no additional synchronization is required, but the
  ping-pong discipline itself must never be violated (e.g. by adding a
  second producer, or by either side "peeking" out of turn to optimize
  latency).

### 8.3 Config and public API

```c
typedef enum {
    COMM_OK = 0, COMM_ERR_TIMEOUT, COMM_ERR_INVALID_ARG, COMM_ERR_INTERNAL,
} comm_err_t;

typedef struct {
    // → lli_config_t (GPIOs, I2C port/clock, card identity bytes)
    int sda_gpio, scl_gpio, irq_gpio, rst_gpio;
    i2c_port_t i2c_port;
    uint32_t   i2c_clk_hz;             // 0 = 400 kHz default
    uint8_t    sens_res[2], nfcid1[3], sel_res;
    uint8_t    nfcid2[8], pad[8], system_code[2], nfcid3t[10];
    uint8_t    gt[47], gt_len, tk[47], tk_len;

    // → transport_config_t timing
    uint32_t handshake_timeout_ms;     // THS, recommended 2000
    uint32_t activate_timeout_ms;
    uint32_t apdu_timeout_ms;

    // → session_config_t identity
    uint8_t local_sk[64], local_pk[32];
    session_peer_key_provider_t peer_key_provider;
    void *peer_key_provider_ctx;

    // Bounds the "waiting for the Application task to answer a command"
    // step (§8.4) — this is new; nothing bounded this step before.
    uint32_t app_response_timeout_ms;  // recommended >= integrity-check period (§10)

    // Internal task ownership
    uint32_t    task_stack_size;   // 0 => facade default (>= 8 KiB: crypto + I2C both on this stack)
    UBaseType_t task_priority;     // 0 => facade default
    BaseType_t  task_core_id;      // tskNO_AFFINITY by default
} comm_module_config_t;

// One instance per device — this firmware has exactly one PN532 and one
// lock, so the facade is a singleton rather than a handle, matching the
// shape of the mailbox itself (there is exactly one).
comm_err_t comm_module_init(const comm_module_config_t *cfg);
comm_err_t comm_module_deinit(void);

// Must be called once, after the Application task exists, before comm_module_start().
// Comms needs this handle to notify the Application task; nothing before
// this point can deliver a command or event.
comm_err_t comm_module_register_app_task(TaskHandle_t app_task);

void comm_module_start(void);   // spawns the comm task, returns immediately
void comm_module_stop(void);    // requests the comm task to exit; see §8.5 limitation
comm_err_t comm_module_force_abort(void);

bool comm_module_session_active(void);

// Application task calls these three from its own task context only.
comm_app_event_t comm_module_poll_event(void);     // returns and clears pending_event
bool             comm_module_has_command(void);    // true if a command awaits a response
comm_err_t       comm_module_get_command(uint8_t *buf, size_t buf_cap, size_t *len_out);
void             comm_module_complete_response(const uint8_t *response, size_t response_length);
```

`comm_module_complete_response` intentionally takes the response bytes
directly, copies them into the mailbox, and performs the notify in one
call — it is not split into "get a mutable pointer, write into it yourself,
then call complete with just a length." A two-step version invites the
Application to hold a stale pointer or forget the second call; this
version has no window where the invariant in §8.2 can be broken by mistake.

`comm_module_init` builds `lli_config_t` → `transport_config_t` →
`session_config_t` internally exactly as before, but now wires
`session_config_t.app_handler` to an **internal** trampoline,
`comm_dispatch_via_mailbox` (§8.4) — never to anything the Application
supplies — and wires the §7.4 event seam
(`session_config_t.on_established`/`.on_terminated`) to a second internal
trampoline that posts `COMM_APP_EVENT_SESSION_STARTED`/`_ENDED` and
notifies, rather than calling an application-supplied function pointer
directly.

### 8.4 The two handoffs: commands vs. lifecycle events

These are **not symmetric**, and that asymmetry is deliberate:

**Commands are a synchronous round-trip** — the phone is sitting there over
NFC waiting for an R-APDU, so the comm task must actually wait for the
Application's answer before it can encrypt and send one:

```c
// Internal trampoline registered as session_config_t.app_handler.
static session_err_t comm_dispatch_via_mailbox(
        const uint8_t *plaintext_in, size_t len_in,
        uint8_t *plaintext_out, size_t *len_out, void *ctx)
{
    mailbox.command_length = len_in;
    memcpy(mailbox.command, plaintext_in, len_in);
    mailbox.command_valid = true;

    xTaskNotifyGive(g_app_task);

    // Bounded — see the honesty note below.
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(g_cfg.app_response_timeout_ms)) == 0) {
        mailbox.command_valid = false;         // give up waiting; don't leave it set
        return SESSION_ERR_INTERNAL;           // → empty encrypted ack, 0x90 0x00 (§7.3)
    }

    *len_out = mailbox.response_length;
    memcpy(plaintext_out, mailbox.response, *len_out);
    return SESSION_OK;
}
```

**Session lifecycle events are fire-and-forget** — there is no APDU tied to
"a session started," so the comm task does not wait for the Application to
acknowledge it, it just posts the event and moves straight on to whatever
Transport does next:

```c
// Internal trampolines registered as session_config_t.on_established / .on_terminated.
static void comm_on_established(void *ctx) {
    mailbox.pending_event = COMM_APP_EVENT_SESSION_STARTED;
    xTaskNotifyGive(g_app_task);
}
static void comm_on_terminated(void *ctx) {
    mailbox.pending_event = COMM_APP_EVENT_SESSION_ENDED;
    xTaskNotifyGive(g_app_task);
}
```

**Honesty note on the bounded wait:** if the Application task never calls
`comm_module_complete_response` within `app_response_timeout_ms` — stuck in
a long integrity check, deadlocked, crashed — the comm task gives up,
Session returns an empty successful ack to the phone (per the existing
non-OK-handler contract, §7.3), and the *phone* sees a normal-looking
response for a command that was never actually processed. That's a real
failure mode worth monitoring for, not one this design eliminates: a
repeated-timeout counter feeding `comm_module_force_abort` (or a
watchdog/reboot at the Application level) is a reasonable mitigation, but
it's the Application/system-integration layer's job to add, not something
implicit here.

### 8.5 Task model and its limitation

The comm task's own loop just calls `transport_run_session` repeatedly:

```c
static void comm_task_fn(void *arg) {
    while (!g_stop_requested) {
        transport_err_t err = transport_run_session(g_transport);
        if (err == TRANSPORT_ERR_BUS_FATAL) {
            // I2C bus/controller wedged beyond recovery — stop instead of
            // looping forever (the Application's supervision decides recovery).
            break;
        }
        // TRANSPORT_ERR_TIMEOUT (no reader) and TRANSPORT_OK (session ran to
        // RELEASED) are both expected, steady-state outcomes — loop immediately.
    }
    g_task_handle = NULL;
    vTaskDelete(NULL);
}
```

**Fatal bus error.** When the PN532 transport reports a bus/controller that is
wedged beyond recovery (`TRANSPORT_ERR_BUS_FATAL`, surfaced from the driver's
`ESP_ERR_INVALID_STATE` via LLI's `LLI_ERR_BUS_FATAL`), the comm task logs once
and exits instead of retrying forever. Previously a wedged bus produced an
infinite `TRANSPORT: activate failed: 6` loop. The Application task is NOT
stopped — its integrity cadence and button ISR continue; a future watchdog/
supervision layer decides whether to re-init or fault.

`comm_module_stop` requests exit and blocks until the task clears — it does
**not** interrupt a session mid-flight, only takes effect the next time the
loop reaches its top. `comm_module_force_abort` has the same underlying
limitation noted before: during the long blocking wait inside
`lli_activate(activate_timeout_ms)` (tens of seconds, waiting for a reader
tap), there is currently no check point at all, so it takes effect only
once a reader taps or that timeout elapses — closing that gap is real,
scoped future work (a cancel-safe primitive below LLI), not something to
claim already works.

### 8.6 What the facade deliberately does not add

- No new GPIO/ISR ownership (§8.0) — unchanged from before.
- Exactly one comm task, exactly one Application task — the facade does
  not spawn worker tasks, pools, or queues on the Application's behalf.
- No buffering of more than one in-flight command — the NFC protocol is
  strictly synchronous (one C-APDU outstanding at a time), so the mailbox
  never needs to hold more than one command and one response.
- No interpretation of the plaintext OPCODE/ARGS envelope, and no exposure
  of the mailbox struct itself outside `comm_module.c` — the Application
  only ever sees `comm_module.h`'s accessor functions.
- No secret storage, no display/UI ownership, and no persistent identity
  store — including for provisioning (§11). The facade's only
  provisioning-related surface is arming a one-shot handshake exception
  and a deferred signature check; the secret, the QR rendering, and the
  authorized-key list all live on the Application side of the boundary.

---

## 9. End-to-End Call Chain (implemented layers)

```
Current firmware (`app_main` smoke test):
  app_main → comm_module_init(&cfg)
    comm_module → session_init → transport_init → lli_init → pn532_i2c_create → pn532_init
  app_main → xTaskCreate(app_task)
    app_task → comm_module_register_app_task(app_task_handle)
    app_task → comm_module_start()
      comm_module → xTaskCreate(comm_task_fn)
```

The Session-enabled path described by the facade is fully wired and exercised
by `main/smart_lock_firmware.c`. `comm_module_init` constructs the Session handle
(registering the internal mailbox trampolines from §8.4 in place of the §7.4 seam's callbacks),
registers `session_on_apdu`/`session_on_erase` into the Transport configuration, and
constructs the Transport handle; `comm_module_start` spawns the single FreeRTOS comm task that
repeatedly calls `transport_run_session`. No new ISR is created anywhere in
this wiring (§8.0). The resulting runtime path involves two tasks:

```
Application:  creates its own task → comm_module_register_app_task(app_task)
              → comm_module_start()
                                          │
                              [comm task, spawned once]
                                          │
                    loop: transport_run_session(transport_h)
                              │
                     lli_activate / lli_receive_apdu / lli_send_apdu
                     (each blocks on the PN532 IRQ semaphore, owned by
                      pn532_i2c.c, unrelated to comm_module or Session)
                              │
              M3 verifies ──► session stage → ESTABLISHED
                              │
              comm task: mailbox.pending_event = SESSION_STARTED
                         xTaskNotifyGive(app_task)  [fire-and-forget, §8.4]
                              │
              secure payload arrives ──► comm_dispatch_via_mailbox runs
                              │            (still on the comm task)
                    mailbox.command = plaintext; notify app task
                    comm task BLOCKS on ulTaskNotifyTake(app_response_timeout_ms)
                              │
                                          ╲
                                           ╲  [Application task, separate stack]
                                            ╲   ulTaskNotifyTake wakes it
                                             ╲  comm_module_get_command(...)
                                              ╲ business logic runs HERE, not on comm task
                                               ╲ comm_module_complete_response(...)
                                                ╲
              comm task wakes ◄─────────────────╯
                    reads mailbox.response → returns to Session → AES-GCM
                    encrypt → lli_send_apdu (R-APDU back to phone)
                              │
              erase while ESTABLISHED ──►
              comm task: mailbox.pending_event = SESSION_ENDED
                         xTaskNotifyGive(app_task)  [fire-and-forget]
```

The key difference from the earlier direct-callback draft: everything
below the dashed diagonal line runs on the **Application's own task and
stack**, never on the comm task. The comm task's only involvement in that
window is blocking on a notification with a bounded timeout.

The implemented layers preserve the intended dependency direction: Session
depends on Transport, Transport depends on LLI, and LLI depends on the PN532
driver stack. No facade or application-level authorization behavior is
implemented yet.

---

## 10. Guidelines for Designing the Application Module

This module's internals — lock state, motor control, tamper detection,
sensor monitoring, business logic, authorization policy — are explicitly
**out of scope** for this document; they belong to their own design. What
follows is the contract the Application Module must honor at the boundary
described in §8, since getting this boundary wrong is exactly how a
"clean separation" ends up quietly recoupled.

**Own a single dedicated task, and never block it indefinitely.** The
Application must have exactly one task that both runs its own recurring
work (tamper/integrity monitoring chief among them) and services the
mailbox. Use a bounded wait, not an unbounded one:

```c
while (1) {
    perform_integrity_checks();   // application-owned; must run on a fixed cadence

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(INTEGRITY_PERIOD_MS))) {
        comm_app_event_t ev = comm_module_poll_event();
        if (ev == COMM_APP_EVENT_SESSION_STARTED) { /* e.g. arm a UI indicator */ }
        if (ev == COMM_APP_EVENT_SESSION_ENDED)   { /* e.g. clear it */ }

        if (comm_module_has_command()) {
            uint8_t cmd[227]; size_t cmd_len;
            comm_module_get_command(cmd, sizeof(cmd), &cmd_len);

            uint8_t resp[227];
            size_t resp_len = dispatch_and_build_response(cmd, cmd_len, resp);

            comm_module_complete_response(resp, resp_len);
        }
    }
}
```

If `INTEGRITY_PERIOD_MS` is not comfortably shorter than
`app_response_timeout_ms` (§8.3), integrity checks will run late whenever a
command is being processed — pick the period first, then set
`app_response_timeout_ms` to something the Comms Module can wait for
without that period slipping noticeably.

**Encode every application-level outcome inside the response bytes, never
via a return code the Comms Module can see.** `comm_module_complete_response`
has no "this failed" signal — by design (§7.1's contract note). Access
denied, wrong lock state, actuator fault, invalid command: all of these are
the Application's own status byte inside `resp[]`, with a normal, non-empty
call to `comm_module_complete_response`. A genuinely empty response should
be rare and intentional, not a stand-in for "something went wrong."

**Keep the handler itself fast; do slow or physically consequential work
outside of it.** The comm task is blocked waiting for
`comm_module_complete_response` for the entire duration between reading a
command and calling it (§8.4) — that's borrowed time from the NFC protocol
budget underneath it, not free. If an action genuinely takes long (a motor
cycle, a flash write), the pattern to reach for is: reply promptly with an
"accepted, in progress" status, run the physical action afterward on the
Application's own schedule, and let a **separate**, later command (or a
polled status field the Application owns) tell the phone it's done. Don't
stretch `app_response_timeout_ms` to cover a slow actuator instead.

**Authentication is not authorization.** Reaching `SECURE_SESSION` (and the
`SESSION_STARTED` event) means the phone proved possession of a registered
long-term key — nothing more. Whether that identity is allowed to unlock
*this* lock *right now* — time-of-day policy, a tamper-triggered lockout,
an explicitly revoked credential the resolver hasn't caught up to yet — is
entirely the Application's decision, made per-command, inside its own
business logic. Don't treat `COMM_APP_EVENT_SESSION_STARTED` as a signal to
do anything irreversible; treat it as advisory (UI, logging, arming a
timeout) and gate every actual action on the specific command received.

**Integrity/tamper monitoring must not depend on NFC activity.** The
bounded-wait loop above is what guarantees this — `perform_integrity_checks()`
runs every `INTEGRITY_PERIOD_MS` regardless of whether a phone is present,
mid-handshake, or the comm task is sitting in a long `lli_activate` wait.
If tamper monitoring is ever moved onto a different task than the one
servicing the mailbox, the same rule still applies: it must not share a
task with anything that can block on `comm_module_*` calls.

**Depend only on `comm_module.h`.** No Application source file should
`#include` `session.h`, `transport.h`, `lli.h`, or any `pn532_*.h` — every
example above only calls facade functions. If a task ever needs something
this header doesn't expose, that's a signal to extend the facade
deliberately, not to reach around it.

**Own provisioning end-to-end.** §11 covers this in full, but the short
version for this section: the button press, the Provision Secret, the QR
rendering, comparing the secret, and adding the new identity to the
authorized-key store are all Application responsibilities, using the same
mailbox path as every other command. Comms' role is limited to arming a
one-shot handshake exception and a deferred signature check — nothing about
provisioning should require the Application to touch `session.h` either.

---

## 11. Provisioning Mode (QR-Based) — corrected design

This section evaluates and corrects a proposal to add QR-based phone
provisioning, reusing the existing handshake rather than building a second
one. The core idea is sound and adopted; two things in the original
proposal are changed for reasons explained inline: (1) a runtime
"verification disabled" mode is replaced with a deferred, single-shot
signature check, so no code path exists anywhere that unconditionally
skips Ed25519 verification, and (2) button/secret/QR/storage ownership is
moved to the Application, matching every other ownership line in this
document. Everything else — reusing Session/Transport, one packet format,
requiring physical presence, a single-use secret, exiting the window after
the first success — is unchanged from the original idea.

### 11.1 Why "skip verification" doesn't survive review

A mode switch that causes `crypto_ed25519_check` to never run is a
standing authentication-bypass path, not a provisioning feature that
happens to be gated by a button. Its risk doesn't scale with how well the
button-press gating works — it scales with the mere fact that the branch
exists at all, reachable by any future state-machine bug, race, or fault
injection on the trigger GPIO, not just by the intended provisioning flow.
Session already has no such branch today, and this design must not
introduce one.

It also under-specifies what the check *would* be verifying against: in
normal operation, a resolver enumerates known long-term keys and M3's
`Sig_P` is checked against each; during provisioning, by definition, the
key being registered isn't in that list yet, so there is genuinely nothing
to check `Sig_P` against **at the moment M3 arrives**. That's the real
reason verification can't happen synchronously during the handshake — not
a reason to skip it, but a reason to defer it until the claimed public key
becomes known (which happens moments later, inside CMD_PROVISION).

### 11.2 Corrected flow

```
Application: button press detected (App's own GPIO/sensor concern)
Application: generates Provision Secret (any CSPRNG source), starts its own timeout
Application: comm_module_arm_provisioning_window(timeout_ms)
                — arms exactly one upcoming handshake to accept an unresolved
                  identity; takes no secret, no key, no display concern
Application: renders Provision Secret as a QR code on its own display peripheral

Phone: scans QR, taps NFC, runs the identical M1/M2/M3 handshake
Comms (Session): M1/M2 unchanged. At M3, because the window is armed,
                 Session does NOT run the resolver loop — it caches
                 Sig_P and the M3 transcript, unverified, and proceeds
                 directly to SECURE_SESSION (fresh ephemeral keys/HKDF
                 derivation happen exactly as normal — only the M3
                 candidate-verification step is skipped, and only because
                 there is nothing yet to verify it against)

Phone → Lock (ordinary encrypted command, via the existing mailbox path,
              §8 — Session does not parse this; it's opaque plaintext):
    CMD_PROVISION { Provision Secret, Claimed Ed25519 Public Key }

Application (in its own command handler, receiving this like any other
             mailbox command):
    1. comm_module_provision_verify_identity(claimed_pubkey)
       — asks Comms to check the CACHED Sig_P/transcript from M3 against
         this specific candidate key. Pure cryptographic fact-check, no
         authorization judgment — same category of operation as the
         existing AEAD tag check, and the only new Comms-side surface
         this feature needs beyond arming the window.
    2. Compare the received secret against its own copy, constant-time,
       and confirm its own provisioning timeout hasn't elapsed.
    3. Only if BOTH (1) and (2) succeed: add claimed_pubkey to its own
       persistent authorized-key store, and disarm the provisioning
       window (single-use — a second CMD_PROVISION, or a second phone
       entirely, gets nothing without another deliberate button press).
    4. Encode success/failure in the response payload as usual (§7.1's
       contract note) — this is an ordinary application-level outcome,
       not a Comms-level one.

Next session: the Application's own `peer_key_provider` callback (already
              backed by its own storage, per §7.2) enumerates the new key
              automatically — no notification back to Comms is needed at
              all, because Comms never held a copy of the identity store
              to update in the first place.
```

###11.2.1 Provision Session Restrictions

A Provision Session is **not** a general-purpose authenticated session.

Although an encrypted channel has been established using the normal
X25519 → HKDF → AES-256-GCM flow, the phone's long-term identity has not yet
been verified. The M3 signature has been cached and will only be verified after
the claimed public key is received.

The Session layer SHALL continue to behave identically to a normal secure
session. It SHALL decrypt incoming payloads and deliver the resulting plaintext
to the Communication Module without interpreting application commands.

The Communication Module SHALL continue to deliver decrypted messages to the
Application Module using the normal mailbox interface. It SHALL NOT enforce
Provision Mode restrictions.

The Application Module owns the Provision Mode state, including the physical
button state, the Provision Secret, and the provisioning workflow. Therefore,
while Provision Mode is active, the Application Module SHALL accept only
`CMD_PROVISION`.

If any other application command is received during Provision Mode, the
Application Module SHALL reject the command, terminate provisioning, request
the Communication Module to terminate the session, and invoke the normal secure
erase procedure.

Only after:

1. The Application validates the Provision Secret.
2. The Application supplies the claimed phone public key.
3. The Communication Module successfully verifies the cached M3 signature
   against the supplied public key.
4. The Application stores the new public key.

is provisioning considered complete.

The provisioning session SHALL then terminate normally.

The newly provisioned identity becomes usable only during a subsequent
connection, where it participates in the standard authenticated handshake.

### 11.3 The two new facade functions, and nothing else

```c
// Arms exactly one upcoming session to skip resolver-based M3 verification
// and instead cache Sig_P + the M3 transcript for a later deferred check.
// Auto-disarms after the timeout or after one CMD_PROVISION-driven identity
// is added, whichever the Application decides — Comms itself does not
// track "was a key added"; it only tracks "is the window still armed" via
// the timeout, and disarms immediately once a session using it completes
// (successfully or not), so it is inherently single-session, not just
// single-key. If the Application wants a hard single-key guarantee across
// multiple connection attempts within the timeout, it enforces that itself
// in step 3 above, not by re-arming.
comm_err_t comm_module_arm_provisioning_window(uint32_t timeout_ms);

// Verifies the CACHED (unverified-at-handshake-time) Sig_P from the most
// recently completed provisioning-mode M3 against a caller-supplied
// candidate public key. Returns true only if that specific key's
// signature over the cached transcript is valid. This is a pure
// crypto fact-check — it makes no judgment about secrets, timeouts, or
// whether to trust the result; that judgment is entirely the caller's.
bool comm_module_provision_verify_identity(const uint8_t claimed_pubkey[32]);
```

Everything else in the original proposal — the Provision Secret itself,
generating it, rendering it as a QR code, comparing it, the persistent
authorized-key store, the timeout that gates the *Application's* decision
window (as opposed to Comms' single-session arming above) — stays entirely
on the Application side, reachable only through the ordinary
`comm_module.h` surface already described in §8 and §10. No new Session or
Transport wire format, no new INS code, no new mailbox event type: `CMD_PROVISION`
is just application-defined content inside the existing `command`/`response`
byte arrays, exactly like any other opcode the Application chooses to define.

### 11.4 Remaining points from the original proposal, confirmed or tightened

- **Replay protection is not "subject to the protocol" — it's unconditional.**
  Every session, provisioning or not, generates fresh ephemeral X25519 keys
  and fresh `c_P`/`c_L` challenges (§7). Nothing about provisioning weakens
  or depends on this; it doesn't need restating as a caveat.
- **Single-use is a hard requirement, not an implication of the diagram.**
  §11.3's window disarms after exactly one session, successful or not —
  the Application must not re-arm automatically; a new provisioning
  attempt requires a new deliberate button press.
- **The 64-candidate resolver bound (§7.2) is the natural ceiling** on how
  many identities provisioning can ever add — no separate limit is needed,
  but it's worth the Application logging when it's approached, since
  exhausting it silently degrades future M3 verification into "always
  fails," not an obvious error.
- **Provision Secret comparison must be constant-time** — ordinary secret-
  comparison hygiene, unrelated to anything specific to this design.
- **The button input deserves the same scrutiny as any other trust
  boundary** — debounced, not remotely triggerable, and ideally logged or
  otherwise surfaced to the owner when used, since it's the one physical
  action this entire feature's security rests on. This is squarely the
  Application's "tamper detection / sensor monitoring" territory (§1), not
  a new Comms responsibility.
- **The QR display's physical exposure is a deployment assumption, not a
  protocol property**, and should be documented as one: if the display
  showing the Provision Secret can be read or photographed by anyone other
  than the intended installer at the moment of provisioning, the "physical
  access required" guarantee this entire feature rests on is void
  regardless of anything described above.
