# Communication Module — Master Reference

**Scope:** PN532 I2C Transport → PN532 Core Driver → PN532 Command Layer → LLI
→ Transport → Session. The Application Module (command dispatch, authorization,
AAI/lock actuation) is an external peer and is out of scope for this document.

**Status of each layer**, per the current implementation:

| Layer | Status | Source docs |
|---|---|---|
| PN532 I2C Transport | Built | `i2c-transport.md`, `api-reference.md` |
| PN532 Core Driver | Built | `core-driver.md`, `api-reference.md` |
| PN532 Command Layer | Built | `command-layer.md`, `api-reference.md` |
| LLI | Built | `lli.md` |
| Transport | Built | `transport.md` |
| Session | Built | `session.md` |
| Comm Module Facade | **Not yet built** — designed below | — |

The current repository builds the PN532, LLI, Transport, and Session layers,
but does not yet provide the planned facade or Application Module.  The
firmware entry point is currently an LLI hardware test harness in
`main/smart_lock_firmware.c`; it does not wire Session into Transport.

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
│  comm_module.h  ◄── planned facade; not yet implemented            │
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
directly; Transport never calls `pn532_*` directly. The planned facade will
be the only header exposed to the Application Module. This boundary is
already true of the implemented layers and must be preserved when the
facade is added.

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

`lli_abort`: ACK → `InRelease(Tg=0x00)` → `pn532_wakeup`, best-effort,
always returns `LLI_OK`.

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
    uint8_t len, sw1, sw2;
} transport_rapdu_t;

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

---

## 8. Comm Module Facade — design (not yet built)

`comm_module.h` — the only header the Application Module includes. Wires
Session + Transport + LLI + drivers behind one config struct and re-exports
just enough surface for the Application's actual needs (register a handler,
run, check liveness, force-abort).

```c
typedef struct comm_module_t *comm_module_handle_t;

typedef enum {
    COMM_OK = 0, COMM_ERR_TIMEOUT, COMM_ERR_INVALID_ARG, COMM_ERR_INTERNAL,
} comm_err_t;

typedef session_err_t (*comm_app_command_handler_t)(
    const uint8_t *plaintext_in, size_t len_in,
    uint8_t *plaintext_out, size_t *len_out,
    void *app_ctx);

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

    // Application's one required registration
    comm_app_command_handler_t app_handler;
    void *app_handler_ctx;
} comm_module_config_t;

comm_err_t comm_module_init(const comm_module_config_t *cfg, comm_module_handle_t *out);
comm_err_t comm_module_deinit(comm_module_handle_t h);
comm_err_t comm_module_run_once(comm_module_handle_t h);
comm_err_t comm_module_run_forever(comm_module_handle_t h);
bool       comm_module_is_session_active(comm_module_handle_t h);
comm_err_t comm_module_force_abort(comm_module_handle_t h);
```

`comm_module_init` builds `lli_config_t` → `transport_config_t` →
`session_config_t` internally, sets
`transport_config_t.on_apdu = session_on_apdu`,
`transport_config_t.on_erase = session_on_erase`,
`transport_config_t.user_ctx = <session_handle_t>`, then calls
`session_init` → `transport_init`. Nothing in this wiring requires any of
the five lower layers to change.

`comm_module_is_session_active` is what the Application spec's §5.2 step 2
needs — "confirm session still active immediately before actuation." It's a
thin call to `transport_get_state(h) == TRANSPORT_STATE_SECURE_SESSION`
combined with a fresh `lli_get_link_status` check, so it reflects RF-loss
promptly rather than trusting stale state.

---

## 9. End-to-End Call Chain (implemented layers)

```
Current firmware (`app_main`):
  app_main → lli_init(test_cfg)
    lli → pn532_i2c_create → pn532_init → pn532_wakeup
    lli → pn532_get_firmware_version
    lli → configure PN532 for ISO-DEP card emulation
  test harness → lli_activate(...)
    lli → pn532_tg_init_as_target(...)
  test harness → lli_receive_apdu(...)
    lli → pn532_send_command(TgGetData) → pn532_receive_response(...)
  test harness → lli_send_apdu(...)
    lli → pn532_tg_set_data(...)
  test harness → lli_abort(...) → lli_deinit(...)
```

The Session-enabled path described by the planned facade is not currently
exercised by `app_main`. Once the facade exists, it will construct a Session
handle, register `session_on_apdu` and `session_on_erase` in a Transport
configuration, construct the Transport handle, and repeatedly call
`transport_run_session`. The resulting runtime path will be:

```
facade → transport_run_session → LLI → PN532
                         ↘ session callbacks
                           ↘ application plaintext handler
```

The implemented layers preserve the intended dependency direction: Session
depends on Transport, Transport depends on LLI, and LLI depends on the PN532
driver stack. No facade or application-level authorization behavior is
implemented yet.
