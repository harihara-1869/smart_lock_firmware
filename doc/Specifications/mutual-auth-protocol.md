# Mutual Authentication Protocol — Specification

This document specifies the NFC mutual-authentication protocol used between
a mobile phone (initiator) and the smart lock (target).  The protocol provides
identity verification in both directions and establishes an AES-256-GCM
session for subsequent command exchange.

---

## 1. Design Goals

| Goal | Mechanism |
|------|-----------|
| Lock → Phone authentication | Ed25519 signature in M2 over a fresh challenge |
| Phone → Lock authentication | AES-GCM authentication tag in every secure payload |
| Forward secrecy | Ephemeral X25519 key agreement per session |
| Replay resistance | Fresh 16-byte random nonces from both sides |
| Confidentiality | AES-256-GCM with per-session derived keys |
| Key erasure | All session material wiped on every teardown path |

---

## 2. Key Material

### Long-term keys (provisioned once)

| Owner | Key | Algorithm | Storage |
|-------|-----|-----------|---------|
| Lock | `LockPriv`, `LockPub` | Ed25519 | Secure element / NVS (ESP32-S3) |
| Phone | `LockPub` | Ed25519 | App keychain |
| Phone | `PhonePriv`, `PhonePub` | Ed25519 | Secure enclave (iOS/Android) |
| Lock | `PhonePub` (allowlist) | Ed25519 | NVS (one or more authorised keys) |

### Ephemeral keys (generated per session)

| Owner | Key | Algorithm |
|-------|-----|-----------|
| Phone | `E_ph` | X25519 |
| Lock | `E_lock` | X25519 |

---

## 3. Session Key Derivation

After the ECDH exchange completes, both sides derive session keys using
HKDF-SHA-256:

```
shared_secret = X25519(E_ph, E_lock)

(okm) = HKDF-SHA-256(
    salt  = "smart-lock-session-v1",
    ikm   = shared_secret,
    info  = N_ph || N_lock,
    L     = 64
)

Keys extracted from okm:
    K_enc  = okm[0..31]     (AES-256 encryption key)
    K_mac  = okm[32..63]    (HMAC-SHA-256 key for integrity)
    IV_base = 0^96          (96-bit zero IV, XORed with counter)
```

`N_ph` and `N_lock` are the 16-byte random nonces exchanged during the
handshake.  The HKDF `info` field binds the keys to this specific session.

---

## 4. Protocol Messages

All messages travel as ISO 7816-4 C-APDUs inside the transport layer.  The
application layer receives parsed `transport_capdu_t` structs and fills
`transport_rapdu_t` responses.

### 4.1 M1 — Handshake Init (Phone → Lock)

| Offset | Length | Field | Description |
|--------|--------|-------|-------------|
| 0 | 32 | `E_ph` | Phone's ephemeral X25519 public key |
| 32 | 16 | `N_ph` | Phone's random nonce |

**C-APDU:**

| Field | Value |
|-------|-------|
| CLA | `0x80` |
| INS | `0x10` |
| P1 | `0x00` |
| P2 | `0x00` |
| Lc | `48` |
| Data | `E_ph \|\| N_ph` |

**Lock processing:**
1. Validate Lc == 48.
2. Generate ephemeral X25519 keypair `E_lock`.
3. Generate 16-byte random nonce `N_lock`.
4. Compute `shared_secret = X25519(E_lock, E_ph)`.
5. Derive `K_enc`, `K_mac` via HKDF (see §3).
6. Store all handshake state; start handshake timer (THS).

### 4.2 M2 — Handshake Response (Lock → Phone)

| Offset | Length | Field | Description |
|--------|--------|-------|-------------|
| 0 | 32 | `E_lock` | Lock's ephemeral X25519 public key |
| 32 | 16 | `N_lock` | Lock's random nonce |
| 48 | 16 | `N_ph` | Echo of phone's nonce (confirmation) |
| 64 | 64 | `Sig` | Ed25519 signature |

**Signature input:**

```
msg = "smart-lock-handshake" || E_ph || N_ph || E_lock || N_lock
Sig = Ed25519_Sign(LockPriv, msg)
```

The domain separator string `"smart-lock-handshake"` prevents cross-protocol
signature reuse.

**R-APDU:**

| Field | Value |
|-------|-------|
| Data | `E_lock \|\| N_lock \|\| N_ph \|\| Sig` |
| len | `128` |
| SW1 | `0x90` |
| SW2 | `0x00` |

**Phone processing:**
1. Extract `E_lock`, `N_lock`, echo of `N_ph`.
2. Verify `N_ph` matches what was sent in M1.
3. Compute `shared_secret = X25519(E_ph, E_lock)`.
4. Derive `K_enc`, `K_mac` via HKDF.
5. Verify `Sig` against `LockPub` and the expected message.
6. If verification fails → abort session.
7. Proceed to send M3.

### 4.3 M3 — Handshake Finish (Phone → Lock)

| Offset | Length | Field | Description |
|--------|--------|-------|-------------|
| 0 | 32 | `T_ph` | HMAC-SHA-256 authentication tag |

```
T_ph = HMAC-SHA-256(K_mac, "smart-lock-finish" || N_lock)
```

**C-APDU:**

| Field | Value |
|-------|-------|
| CLA | `0x80` |
| INS | `0x11` |
| P1 | `0x00` |
| P2 | `0x00` |
| Lc | `32` |
| Data | `T_ph` |

**Lock processing:**
1. Recompute expected `T_ph` using `K_mac` and `N_lock`.
2. Compare in constant-time against received `T_ph`.
3. If mismatch → send SW=0x69 0x82, erase all session keys, release link.
4. If match → send SW=0x90 0x00, transition to SECURE_SESSION.

**R-APDU (success):**

| Field | Value |
|-------|-------|
| Data | (empty) |
| len | `0` |
| SW1 | `0x90` |
| SW2 | `0x00` |

---

## 5. Secure Payload Exchange

Once in SECURE_SESSION, every C-APDU with INS=0x20 carries an encrypted
payload, and every R-APDU carries an encrypted response.

### 5.1 Encryption

```
counter = 0, 1, 2, ...
IV      = IV_base XOR counter       (96-bit, big-endian counter)

plaintext → AES-256-GCM(K_enc, IV, aad) → ciphertext || tag
```

| Field | Size | Description |
|-------|------|-------------|
| `counter` | 4 bytes | Big-endian, incremented per direction per exchange |
| `IV` | 12 bytes | `0^96 XOR counter` |
| `aad` | variable | Protocol version byte (`0x01`) + counter |
| `tag` | 16 bytes | GCM authentication tag, appended to ciphertext |

The counter is maintained independently for each direction (phone→lock and
lock→phone).  The phone's counter starts at 0 for M1; the lock's counter
starts at 0 for M2.  Each successful send/increment in a direction increments
its counter by 1.

### 5.2 Secure C-APDU (Phone → Lock)

| Field | Value |
|-------|-------|
| CLA | `0x80` |
| INS | `0x20` |
| P1 | `0x00` |
| P2 | `0x00` |
| Lc | `len(ciphertext) + 16` |
| Data | `ciphertext \|\| tag[16]` |

Plaintext budget: `255 - 12(nonce) - 16(tag) = 227 bytes`.

### 5.3 Secure R-APDU (Lock → Phone)

| Field | Value |
|-------|-------|
| Data | `ciphertext \|\| tag[16]` |
| len | `len(ciphertext) + 16` |
| SW1 | `0x90` |
| SW2 | `0x00` |

On GCM tag mismatch the lock sends SW=0x69 0x82, invokes erase, and
transitions to RELEASED.

---

## 6. Session Abort

Either side may send `CMD_SESSION_ABORT` (INS=0x30) at any point during
HANDSHAKE or SECURE_SESSION.

**C-APDU:**

| Field | Value |
|-------|-------|
| CLA | `0x80` |
| INS | `0x30` |
| P1 | `0x00` |
| P2 | `0x00` |
| Lc | `0` |

**Lock processing:**
1. Invoke erase callback (destroy all session keys).
2. Send SW=0x90 0x00 (best-effort — link may already be gone).
3. Transition to RELEASED.

---

## 7. Key Erasure

All session material is wiped on every teardown path:

- `K_enc`, `K_mac` zeroed
- `E_lock` private key zeroed
- `N_ph`, `N_lock` zeroed
- Shared secret zeroed
- HKDF intermediate state zeroed

The erase callback is invoked **before** the transport state is set to
RELEASED, guaranteeing that no code path can observe the RELEASED state
without the keys already being destroyed.

Erase triggers:
- Handshake timer (THS) expiry
- NFC link release at any point
- Any receive error during HANDSHAKE or SECURE_SESSION
- M3 signature verification failure
- GCM tag mismatch in SECURE_SESSION
- Session abort (INS=0x30)
- Application callback error

---

## 8. Threat Model and Security Properties

### 8.1 Passive eavesdropping

All handshake data is transmitted in the clear over the NFC RF interface.
An eavesdropper can observe `E_ph`, `E_lock`, `N_ph`, `N_lock`, and the
Ed25519 signature `Sig`.  However, the session keys `K_enc` and `K_mac` are
derived from the X25519 shared secret, which requires knowledge of at least
one ephemeral private key.  The secure payload exchange is encrypted with
AES-256-GCM.

### 8.2 Active man-in-the-middle

The Ed25519 signature in M2 binds the lock's identity to the handshake
transcript.  A MITM cannot forge this signature without `LockPriv`.  The
phone verifies the signature before sending M3, so a MITM fails at step 2.

The HMAC in M3 proves the phone derived the same session keys, confirming
it has the correct `shared_secret`.  A MITM without the ability to complete
the X25519 exchange cannot produce a valid M3.

### 8.3 Replay attacks

Fresh random nonces from both sides (`N_ph`, `N_lock`) ensure that every
handshake transcript is unique.  Replaying M1 produces a different `N_lock`
and therefore different session keys.  Replaying M2 or M3 fails because the
nonces and ECDH shares do not match.

### 8.4 Key compromise

- **LockPriv compromised:** Attacker can impersonate the lock.  Re-key
  requires re-provisioning the lock's NVS.
- **PhonePriv compromised:** Attacker can complete M3.  The lock's allowlist
  can be updated to remove the compromised phone's `PhonePub`.
- **Session key compromise:** Forward secrecy limits the blast radius to the
  current session.  Long-term keys are not exposed.

### 8.5 Denial of service

The lock allocates no dynamic memory after init and performs all handshake
work in a single FreeRTOS task.  A flood of M1 messages is rate-limited by
the NFC RF interface (one reader at a time) and the handshake timeout (THS).

---

## 9. Timing Parameters

| Parameter | Recommended | Description |
|-----------|-------------|-------------|
| `activate_timeout_ms` | 30000 (30 s) | How long to wait for a reader tap |
| `handshake_timeout_ms` (THS) | 2000 (2 s) | Max time from M2 sent to M3 received |
| `apdu_timeout_ms` | 5000 (5 s) | Per-APDU receive timeout in secure session |

---

## 10. Future Extensions

- **Key rotation:** Periodic re-provisioning of `LockPriv` / `PhonePub`
  via a secure out-of-band channel.
- **Multi-user allowlist:** Multiple `PhonePub` keys stored in NVS, each
  identified by a key ID included in M1.
- **Audit log:** Persistent log of successful/failed authentication attempts
  with timestamps and key IDs.
- **OTA firmware update:** Secure firmware delivery over the same NFC session
  using a dedicated INS code.
- **BLE companion channel:** Out-of-band BLE channel for key provisioning
  and session auditing.
