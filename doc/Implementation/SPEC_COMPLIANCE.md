# Spec Compliance Report

Compared against: `doc/Specifications/smartlock_communication_module_spec.pdf`
(v0.1, July 26, 2026)

Generated: July 26, 2026

---

## Part I — Session Module

### Crypto Backend (§2.2)

| Primitive | Spec says | Implementation |
|-----------|-----------|----------------|
| Ed25519 sign/verify | libsodium | Vendored Monocypher 4.x (`crypto_eddsa_*`) |
| X25519 agreement | libsodium | Monocypher (`crypto_x25519`) |
| HKDF-SHA-256 | mbedTLS | ~40-line hand-rolled implementation over `mbedtls_md_hmac_*` (mbedTLS `HKDF_C` is off in sdkconfig) |
| AES-256-GCM | mbedTLS | mbedTLS `mbedtls/gcm.h` — **matches spec** |
| Secure RNG | libsodium | `esp_fill_random` (ESP hardware RNG) |
| Secure erase | libsodium `sodium_memzero` | `mbedtls_platform_zeroize` |

**Delta:** The spec names libsodium for Ed25519/X25519/secure-RNG; the implementation uses Monocypher + ESP hardware RNG + mbedTLS. The choice was made because libsodium is not bundled with ESP-IDF and would add significant flash cost, while Monocypher is a single vendored source file. Both are audited, constant-time implementations. The `session_crypto.h` seam exists precisely so the backend can be swapped later (e.g. to an ATECC608A for signing).

**Functional equivalence:** All primitives produce identical wire-level results to what the spec defines. Monocypher's Ed25519 uses SHA-512 (RFC 8032), same as libsodium's `crypto_sign_*`.

### HKDF-SHA-256 (§5.2)

**Matches spec.** `PRK = HKDF-Extract(salt = c_P ‖ c_L, ikm = SharedSecret)`, `K_p2e = HKDF-Expand(PRK, "phone->esp", 32)`, `K_e2p = HKDF-Expand(PRK, "esp->phone", 32)`. Verified against all three RFC 5869 test vectors.

### Transcript Domain Separation (§4.3)

**Matches spec.** The signed payload is `"SLOCK-HS-v1" ‖ 0x01 ‖ pk_eph_P ‖ pk_eph_L ‖ c_P ‖ c_L` (140 bytes). The spec says a domain separation prefix and protocol version byte SHOULD be prepended; the implementation does both.

### M3 Peer-Key Resolution (§4.4)

**Implementation-defined, spec-compliant.** The spec requires "Lock MUST verify Sig_P using the Phone's provisioned long-term public key PK_P." M1 carries no identity hint (§4.2/4.3), so the implementation uses an iterator (`session_peer_key_provider_t`) to enumerate candidate keys and verify against each. First match wins; exhaustion = authentication failure. This is the only possible interpretation given a keyless M1.

### Replay Protection (§6.3)

**Conforms — explicitly deferred.** The spec says sequence-number replay protection is OPTIONAL within a session for low-volume use. The implementation does not add sequence numbers, consistent with the spec's current revision.

### Session Termination (§7)

**Matches spec.** `session_on_erase` securely destroys: `sk_eph_L`, `shared_secret`, `PRK`, `K_p2e`, `K_e2p`. Idempotent. The long-term identity key persists between sessions.

### Item 8 — Byte-Budget Assertion (Part V §8)

**Resolved.** Session retains the minimum-overhead check and adds the required debug assertion `assert(capdu->lc <= 255)`; Transport remains the owner of the 227-byte protocol budget.

---

## Part II — Application Module

**Not implemented.** The spec defines command dispatch, authorization, and lock actuation as a collaborating peer of the Session module. None of this exists in the codebase. The `session_app_cmd_handler_t` callback interface is the seam where the Application module will register.

The spec's peer-collaboration model (Session and Application as equal-standing peers, not a stack) is architecturally respected: Session treats plaintext as opaque and delegates to `app_handler`. The Application module's future `comm_module.h` facade will wire both sides.

---

## Part III — Transport

### State Machine (§3)

**Conforms.** The four active states and transitions match the spec:

| Spec name | Implementation name | Conforms |
|-----------|-------------------|----------|
| IDLE | `TRANSPORT_STATE_IDLE` | ✓ |
| ISO-DEP Activated | `TRANSPORT_STATE_ACTIVATED` | ✓ |
| Handshake Pending | `TRANSPORT_STATE_HANDSHAKE` | ✓ |
| Secure Session | `TRANSPORT_STATE_SECURE_SESSION` | ✓ |
| RELEASED | `TRANSPORT_STATE_RELEASED` | ✓ |

### Instruction Set (§5, §5.1)

**Conforms.** All four INS codes and their state-validity rules are correctly implemented:

- `0x10` (HANDSHAKE_INIT): ACTIVATED only. ✓
- `0x11` (HANDSHAKE_FINISH): HANDSHAKE only. ✓
- `0x20` (SECURE_PAYLOAD): SECURE_SESSION only. ✓
- `0x30` (SESSION_ABORT): ACTIVATED, HANDSHAKE, SECURE_SESSION. ✓

The ACTIVATED state now correctly handles `CMD_SESSION_ABORT` before the `INS_HANDSHAKE_INIT` check (fixed per Part V §2).

### Status Words (§9)

**Conforms.** Transport sends `0x69 0x82` for signature/tag failures (with erase), `0x69 0x85` for out-of-state INS, `0x6A 0x80` for malformed Lc / oversized payload, `0x90 0x00` on success.

### R-APDU Format (§4.2) — Transport appends SW1/SW2

**Resolved.** Transport serializes callback-produced responses as `Data + SW1 + SW2` (up to 258 bytes). Status-only error paths continue to send exactly SW1/SW2.

**Impact:** Phone-side handling must consume the two-byte status trailer on M2 and secure-payload responses.

### Handshake Timeout (§8.1)

**Conforms.** `handshake_timeout_ms` defaults to 2000 ms (configurable).

### Lc > 227 Handling (§4.3)

**Conforms.** Transport rejects `Lc > 227` with `0x6A 0x80` and stays in SECURE_SESSION (does not erase), exactly per spec.

### Secure-Erase Triggers (§8.4)

**Conforms.** All six trigger conditions are implemented:
- CMD_SESSION_ABORT in any active state ✓
- Handshake timeout ✓
- Link status returns RELEASED ✓
- Frame-integrity error in HANDSHAKE/SECURE_SESSION ✓
- Signature verification failure (M3) ✓
- AES-GCM auth-tag mismatch ✓

---

## Part IV — Hardware/Link Layer (PN532 Binding)

### Target Configuration (§1.1)

**Conforms.** Type A, 106 kbps, Passive, ISO-DEP. Mode flags are `PN532_TG_MODE_PICC_ONLY | PN532_TG_MODE_PASSIVE_ONLY` (defense-in-depth, per Part V §7).

### LLI Primitive Bindings (§2)

| Primitive | Binding | Conforms |
|-----------|---------|----------|
| `LLI Activate()` | `TgInitAsTarget (0x8C)` | ✓ |
| `LLI ReceiveAPDU()` | `TgGetData (0x86)` — raw path, status byte mapped directly | ✓ |
| `LLI SendAPDU()` | `TgSetData (0x8E)` | ✓ |
| `LLI GetLinkStatus()` | `TgGetTargetStatus (0x8A)` | ✓ |
| `LLI Abort()` | `InRelease (0x52)` + `pn532_wakeup` | ✓ |

### Status Byte Mapping (§2.2.1)

**Conforms.** `lli_receive_apdu` maps PN532 status bytes exactly per spec:
- `0x00` → `LLI_OK` ✓
- `0x01` → `LLI_ERR_TIMEOUT` ✓
- `0x29` → `LLI_ERR_LINK_RELEASED` ✓
- other → `LLI_ERR_FRAME_INTEGRITY` ✓

### WTX Handling (§2.3.1)

**Conforms by design.** The PN532 autonomously issues S(WTX) when needed; the LLI driver does not need to manage it.

### Frame Size Conformance (§3)

**Conforms.** The PN532 supports 261/258-byte Short APDUs, matching the spec's assumed bounds. The 227-byte unchained secure-payload budget holds.

---

## Part V — Driver Conformance Requirements (All 8 Items)

| # | Issue | Spec requirement | Status |
|---|-------|-----------------|--------|
| 1 | GetGeneralStatus pre-check in `lli_activate` | Remove the check entirely | **Fixed** |
| 2 | `CMD_SESSION_ABORT` not honored in ACTIVATED | Add INS check before the `0x10` check | **Fixed** |
| 3 | `lli_config_t.i2c_port` type mismatch / missing `i2c_clk_hz` | Change to `i2c_port_t`, add `i2c_clk_hz` | **Fixed** |
| 4 | No fail-fast on oversized `gt_len`/`tk_len` | Validate ≤ 47 in `lli_init` | **Fixed** |
| 5 | MI-bit chaining not in raw receive path | Document the 261 < 227 assumption | **Fixed** (comment added) |
| 6 | Silent hardware reset loses SAM/param config | Re-apply config in every `lli_activate` | **Fixed** |
| 7 | TgInitAsTarget mode doesn't set PASSIVE_ONLY | OR in `PN532_TG_MODE_PASSIVE_ONLY` | **Fixed** |
| 8 | Byte-budget re-derivation risk | Assert `in_len ≤ 255` instead of re-checking | **Fixed** |

---

## Explicitly Deferred Items

The spec explicitly scopes these as future work (§9 in each Part). The implementation correctly defers all of them:

| Item | Spec section | Status |
|------|-------------|--------|
| ATECC608A secure element migration | Part I §9.1 | Deferred (software keys today) |
| Relay attack resistance | Part I §9.2 | Deferred |
| OTA firmware update dispatch | Part II §9.1 | Deferred |
| ACL / roles / per-device authorization | Part II §9.2 | Deferred (auth=authz today) |
| Out-of-band BLE command source | Part II §9.3 | Deferred |
| Application Module | — | Deferred pending Application integration |
| Communication module facade (`comm_module.h`) | — | **Implemented** |

---

## Summary

**Conforms fully:** All LLI primitives, PN532 bindings, status byte mapping, instruction set and state-validity rules, handshake protocol (M1/M2/M3), HKDF-SHA-256 derivation, AES-256-GCM secure channel, secure-erase triggers and idempotency, WTX delegation, frame-size conformance, replay-protection scoping, all eight fixed Part V conformance items.

**Known deviations:**
1. **Crypto backend** — Monocypher + mbedTLS + esp_random instead of libsodium. Functionally equivalent; `session_crypto.h` seam enables future swap.
2. **Application Module** — Not yet built. Session's callback seam (`session_app_cmd_handler_t`) is ready for it; the Application-layer specification is not present in the current worktree.
