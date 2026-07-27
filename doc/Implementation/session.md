# Session Layer — Implementation

The session layer sits above Transport and below the Application Module.  It
implements the lock side of the three-message mutual-authentication handshake
(ephemeral X25519 + Ed25519 identity proof), derives the AES-256-GCM session
keys with HKDF-SHA-256, and then acts as a pure courier between the encrypted
NFC channel and the Application's plaintext command handler.

It plugs into Transport through the existing `on_apdu` / `on_erase` callback
slots — no adapter, no Transport changes.  It imports only `transport.h` and
has zero knowledge of LLI, PN532, or I2C.

## Position in the Stack

```
┌──────────────────────────┐
│   Application Module     │  dispatch / authorization / AAI (external peer)
├──────────────────────────┤
│      Session Layer       │  ← this component
├──────────────────────────┤
│     Transport Layer      │  state machine, C-APDU parsing, status words
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

## Wiring

```c
session_handle_t session;
session_init(&session_cfg, &session);

transport_config_t tcfg = {
    ...
    .on_apdu  = session_on_apdu,
    .on_erase = session_on_erase,
    .user_ctx = session,
};
```

`session_on_apdu` is type-compatible with `transport_apdu_handler_t` and
`session_on_erase` with `transport_erase_handler_t`.  Transport validates
INS-per-state before forwarding, so session's dispatch is a thin switch:

| INS | Name | Session handler |
|-----|------|-----------------|
| `0x10` | `CMD_HANDSHAKE_INIT` | `session_handle_m1` |
| `0x11` | `CMD_HANDSHAKE_FINISH` | `session_handle_m3` |
| `0x20` | `CMD_SECURE_PAYLOAD` | `session_handle_secure_payload` |

## Handshake Messages

### M1 — phone → lock (64 bytes)

```
pk_eph_P(32) ‖ c_P(32)
```

Phone's ephemeral X25519 public key and random challenge.  Carries **no
identity hint** — see "Peer-Key Resolution" below.

Checks applied: `Lc == 64` (else `TRANSPORT_ERR_INVALID_APDU`), and
`pk_eph_P` must not be the all-zero point (X25519 contributory-behavior
check, session spec §4.4 rule 3).

### M2 — lock → phone (128 bytes)

```
pk_eph_L(32) ‖ c_L(32) ‖ Sig_L(64)
```

`Sig_L` is an RFC 8032 Ed25519 signature with the lock's long-term key over
the domain-separated transcript:

```
"SLOCK-HS-v1" ‖ 0x01 ‖ pk_eph_P ‖ pk_eph_L ‖ c_P ‖ c_L        (140 bytes)
```

### M3 — phone → lock (64 bytes)

```
Sig_P(64)
```

The phone's Ed25519 signature over the same transcript.  Verified against
each candidate key from the peer-key provider until one verifies or the
list is exhausted.

### Key derivation (on M3 success)

```
SharedSecret = X25519(sk_eph_L, pk_eph_P)     — all-zero output rejected
PRK   = HKDF-Extract(salt = c_P ‖ c_L, ikm = SharedSecret)
K_p2e = HKDF-Expand(PRK, "phone->esp", 32)
K_e2p = HKDF-Expand(PRK, "esp->phone", 32)
```

`SharedSecret` is zeroized immediately after derivation.

## Secure Payloads

Wire format in both directions:

```
nonce(12) ‖ ciphertext ‖ GCM tag(16)
```

Inbound is opened with `K_p2e`; the reply is sealed with `K_e2p` under a
fresh CSPRNG nonce.  Maximum plaintext is 227 bytes (255 − 28 overhead).

Session never interprets plaintext.  The decrypted bytes go to the
Application's `session_app_cmd_handler_t` and its reply is sealed verbatim.
Application-level failures (for example access denied, actuator jam,
validation failure, or an invalid command) must be encoded in the plaintext
reply, while the handler still returns `SESSION_OK`.  Returning a non-OK value
does not communicate an application error to the phone: session replies with
an empty encrypted payload and Transport still returns `0x90 0x00`.  Non-OK
returns are reserved for genuine internal failures such as insufficient
buffer space, internal errors, or crypto/backend failures.  Only crypto-level
failures (GCM tag mismatch) terminate the session, and a tag mismatch is
treated identically to a handshake signature failure: Transport sends
`0x69 0x82`, erases, and releases.

## Peer-Key Resolution

M1 carries no identity hint, so M3 verification can only work by
enumeration.  `session_peer_key_provider_t` is an iterator, not a lookup:

```c
bool provider(size_t index, uint8_t pubkey_out[32], void *ctx);
```

Session calls it with `index = 0, 1, 2, ...` up to the fixed defensive bound
`SESSION_MAX_PEER_CANDIDATES` (64), or until it returns `false`.
Reaching the bound is treated exactly like the provider returning `false`:
the first candidate that verifies `Sig_P` is the authenticated identity for
the session, and exhaustion with no match is an authentication failure
(`0x69 0x82` + erase), indistinguishable from a single bad signature.  The
bound only protects against buggy provider implementations and does not
change the behavior of correctly implemented providers.  Any candidate that
verifies is by definition authorized — authentication implies authorization.

## Internal Stage Model

The handle tracks which material currently exists so that
`session_on_erase` is idempotent and no material survives across sessions:

```
EMPTY  ──M1 handled──►  EPHEMERAL  ──M3 verified──►  ESTABLISHED
  ▲                        │                            │
  └────────────────────────┴──── session_on_erase ◄─────┘
```

| Stage | Material present |
|-------|------------------|
| `EMPTY` | none (long-term identity in cfg persists) |
| `EPHEMERAL` | `sk_eph_L`, `pk_eph_L`, `pk_eph_P`, `c_P`, `c_L` |
| `ESTABLISHED` | ephemeral cache + `K_p2e`, `K_e2p` |

A fresh M1 always begins by erasing stale material, so an aborted previous
session can never contaminate the next one.  The stage checks in the M3 and
secure-payload handlers are defensive duplicates of Transport's state
machine and should never fire.

## Secure-Erase Contract

Transport invokes `on_erase` exactly once per teardown path, always before
entering RELEASED.  `session_on_erase` zeroizes all ephemeral and derived
material (`sk_eph_L`, transcript cache, `K_p2e`, `K_e2p`) and returns the
handle to `EMPTY`.  It is safe when no session ever started, when M1 was
answered but M3 never arrived, and when a full secure session ends.  The
long-term `local_sk` in the config copy survives erase; it is zeroized only
by `session_deinit`, which wipes the entire handle.

## Crypto Backend

All primitives are behind the private seam `src/session_crypto.h` — this is
the swap point for a future ATECC608A backend:

| Primitive | Implementation |
|-----------|----------------|
| Ed25519 sign/verify (RFC 8032) | Monocypher optional layer (`crypto_ed25519_*`), vendored in `src/third_party/` |
| X25519 (RFC 7748) | Monocypher (`crypto_x25519`) with all-zero output rejection |
| HKDF-SHA-256 (RFC 5869) | ~40 lines over mbedTLS `mbedtls_md_hmac` |
| AES-256-GCM | mbedTLS `mbedtls/gcm.h` |
| CSPRNG | `esp_fill_random` |
| Secure zeroize | `mbedtls_platform_zeroize` |

No other file in the component includes a crypto library header.

## API Reference

### Types

#### `session_err_t`

| Value | Description |
|-------|-------------|
| `SESSION_OK` | Success |
| `SESSION_ERR_INVALID_ARG` | NULL arguments / bad configuration |
| `SESSION_ERR_AUTH_FAILED` | Signature or AEAD tag mismatch |
| `SESSION_ERR_INTERNAL` | Allocation or crypto-backend failure |

#### `session_config_t`

| Field | Type | Description |
|-------|------|-------------|
| `local_sk` | `uint8_t[64]` | Lock's Ed25519 secret key (seed 32 ‖ public 32) |
| `local_pk` | `uint8_t[32]` | Lock's Ed25519 public key |
| `peer_key_provider` | `session_peer_key_provider_t` | Required. Candidate enumerator for M3 |
| `peer_key_provider_ctx` | `void *` | Passed to the provider |
| `app_handler` | `session_app_cmd_handler_t` | Required. Plaintext command handler |
| `app_handler_ctx` | `void *` | Passed to the handler |

#### `session_app_cmd_handler_t`

```c
typedef session_err_t (*session_app_cmd_handler_t)(
        const uint8_t *plaintext_in, size_t len_in,
        uint8_t *plaintext_out, size_t *len_out,
        void *app_ctx);
```

`len_out` enters as capacity (227) and leaves as bytes written; it must not
exceed the capacity.  Application-level failures must be encoded in the
plaintext response while returning `SESSION_OK`; non-OK returns are reserved
for genuine internal failures and produce an empty encrypted payload with
Transport status `0x90 0x00`.  Non-OK returns do not end the session.

#### `session_peer_key_provider_t`

```c
typedef bool (*session_peer_key_provider_t)(
        size_t index, uint8_t pubkey_out[32], void *provider_ctx);
```

Return `true` when `pubkey_out` was filled, `false` when exhausted.

### Functions

#### `session_init` / `session_deinit`

```c
session_err_t session_init(const session_config_t *cfg, session_handle_t *out);
session_err_t session_deinit(session_handle_t h);
```

Allocate / destroy a session instance.  `deinit` is NULL-safe and zeroizes
the entire handle, including the config copy holding `local_sk`.

#### `session_on_apdu`

```c
transport_err_t session_on_apdu(const transport_capdu_t *capdu,
                                transport_rapdu_t *rapdu, void *ctx);
```

Register as `transport_config_t.on_apdu` with `user_ctx` = session handle.
Returns `TRANSPORT_OK` on success; `TRANSPORT_ERR_INVALID_APDU` for
malformed input; a non-OK code for authentication/crypto failures, which
Transport maps to `0x69 0x82` + erase per the state machine.

#### `session_on_erase`

```c
void session_on_erase(void *ctx);
```

Register as `transport_config_t.on_erase`.  Idempotent secure erase of all
session-scoped key material.

## Standalone Usage

```c
#include "session.h"

static const uint8_t phone_pk[32] = { ... };

static bool one_key_provider(size_t index, uint8_t out[32], void *ctx)
{
    if (index != 0) return false;
    memcpy(out, phone_pk, 32);
    return true;
}

static session_err_t echo_handler(const uint8_t *in, size_t in_len,
                                  uint8_t *out, size_t *out_len, void *ctx)
{
    if (in_len > *out_len) return SESSION_ERR_INVALID_ARG;
    memcpy(out, in, in_len);
    *out_len = in_len;
    return SESSION_OK;
}

session_config_t scfg = {
    .local_sk = { ... },
    .local_pk = { ... },
    .peer_key_provider = one_key_provider,
    .app_handler       = echo_handler,
};

session_handle_t session;
session_init(&scfg, &session);

transport_config_t tcfg = {
    .lli_cfg = { ... },
    .handshake_timeout_ms = 2000,
    .activate_timeout_ms  = 30000,
    .apdu_timeout_ms      = 5000,
    .on_apdu  = session_on_apdu,
    .on_erase = session_on_erase,
    .user_ctx = session,
};

transport_handle_t transport;
transport_init(&tcfg, &transport);

while (1) {
    transport_run_session(transport);
}
```
