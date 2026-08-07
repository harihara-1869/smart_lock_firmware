# Storage Component — Implementation

The storage component provides persistence for the smart lock firmware. It is a
standalone ESP-IDF component and a **passive library**: no tasks, no timers, no
ISRs, no queues, no business logic, no policy. It is NOT a peer module — it
exists solely to read and write bytes on behalf of the Application Module.

## Position in the Stack

```
┌──────────────────────────┐
│   Application Module     │  dispatch / authorization / AAI
├──────────────────────────┤
│       key_store          │  authorized-phone-key list + lock identity
│       intent_log         │  actuation-target intent for boot recovery
├──────────────────────────┤
│      storage_hal         │  PRIVATE backend seam (NVS today, SE later)
├──────────────────────────┤
│   NVS flash / SE chip   │  physical persistence
└──────────────────────────┘
```

## Public API

Two headers, both included by `app_module`:

| Header | Purpose |
|--------|---------|
| `key_store.h` | Authorized phone public keys + lock identity |
| `intent_log.h` | Actuation-intent target for boot recovery |

No other file outside `components/storage` may include internal headers
(`storage_hal.h`, `monocypher-ed25519.h`).

## Architecture: RAM Cache + Write-Through

Every public read is served from a **RAM cache** warmed at boot. No HAL call
on any read path — the cache is small (64 × 32-byte keys, one 4-byte intent
target) and fits comfortably in SRAM. Writes commit to slow storage (NVS)
**first**, then mirror to RAM — write-through. A failed HAL write leaves RAM
untouched, so the lock never believes data was persisted when it wasn't.

### Key Store (`key_store.h`)

```c
key_store_err_t key_store_init(void);         // warm RAM cache from NVS
key_store_err_t key_store_add(const uint8_t pk[32]);   // write-through add
key_store_err_t key_store_revoke(const uint8_t pk[32]); // write-through remove
bool   key_store_get(size_t index, uint8_t out[32]);   // RAM-only iterator
bool   key_store_contains(const uint8_t pk[32]);       // RAM-only check
size_t key_store_count(void);                           // RAM-only count
```

Phone keys live in NVS namespace `"keys"`, stored as 32-byte blobs under
keys `k00`, `k01`, …, `k63`.

### Intent Log (`intent_log.h`)

```c
intent_err_t intent_log_init(void);                    // warm RAM cache
intent_err_t intent_log_write(intent_target_t target); // write-through
intent_target_t intent_log_get_cached(void);           // RAM-only read
```

The intent lives in NVS namespace `"intent"`, key `"target"`.

### HAL (`storage_hal.h` — private)

Four operations behind which the backend (NVS vs Secure Element) is swapped
via Kconfig:

```c
storage_err_t storage_hal_init(void);
storage_err_t storage_hal_read_blob(const char *ns, const char *key, void *out, size_t *len);
storage_err_t storage_hal_write_blob(const char *ns, const char *key, const void *in, size_t len);
storage_err_t storage_hal_erase_key(const char *ns, const char *key);
```

| Kconfig symbol | Backend | Persistence |
|---|---|---|
| `CONFIG_STORAGE_BACKEND_NVS` (default) | ESP32 SPI flash via `nvs_flash` | Survives power loss and firmware re-flash |
| `CONFIG_STORAGE_BACKEND_SE` | Secure Element (stub) | `TODO(hardware)` — honest errors logged |

### NVS namespaces

| Namespace | Purpose | Keys |
|-----------|---------|------|
| `keys` | Authorized phone public keys | `k00` … `k63` (32-byte Ed25519 PKs) |
| `intent` | Actuation-intent target | `target` (4-byte `intent_target_t`) |
| `identity` | Lock Ed25519 keypair | `lock` (96 bytes: 64 SK ‖ 32 PK), `booted` (1-byte sentinel) |

### Concurrency

The `key_store_get` iterator may execute on the **comm task** (via the
provider trampoline during M3 handshake). Mutation (`add`, `revoke`,
`identity_init`) happens only on the **Application task**. The ordering
rules — fill slot before publishing count, compact before shrinking count —
make the worst concurrent race a benign one-time missed candidate, never an
authorization bypass. No mutex.

## Lock Identity

The lock's long-term Ed25519 keypair is **generated on-device at first boot**
and persisted to NVS. It is loaded from NVS on every subsequent boot.

### Boot flow

`key_store_identity_init()` is **idempotent** and runs from `init_storage()`
in main — before `build_comm_config()` assembles the comm config, because
`comm_module_init` (and the session beneath it) snapshots `local_sk`/`local_pk`
at init time. `AppModule_Init()` calls it again as a no-op safety net.

```
init_storage() (main)            ← identity MUST exist before comm config
  └─ key_store_identity_init()   ← AppModule_Init() re-calls (idempotent)
       ├─ storage_hal_read_blob("identity", "lock") → STORAGE_OK (96 bytes)
       │    → "identity loaded from NVS"
       │
       ├─ storage_hal_read_blob("identity", "lock") → STORAGE_ERR_NOT_FOUND
       │    ├─ storage_hal_read_blob("identity", "booted") → NOT_FOUND
       │    │    → TRUE FIRST BOOT
       │    │    → generate fresh Ed25519 keypair from CSPRNG
       │    │    → persist to NVS + write "booted" sentinel
       │    │    → erase all stale phone keys from keys namespace
       │    │
       │    └─ storage_hal_read_blob("identity", "booted") → FOUND
       │         → "device has booted before but identity is missing"
       │         → NVS partition may have been erased — proceed as first boot
       │
       ├─ storage_hal_read_blob("identity", "lock") → STORAGE_OK (wrong size)
       │    → "refusing to overwrite" — corrupt NVS, don't orphan provisioned phones
       │
       └─ any other error → fail loud
```

### Keygen

```c
static void generate_fresh_keypair(uint8_t sk[64], uint8_t pk[32])
{
    uint8_t seed[32];
    esp_fill_random(seed, sizeof(seed));
    crypto_ed25519_key_pair(sk, pk, seed);
}
```

`crypto_ed25519_key_pair()` comes from Monocypher (`components/monocypher/`),
a standalone component depended on by both Session (for handshake crypto) and
Storage (for identity generation). The 32-byte seed is from the ESP32's
hardware TRNG via `esp_fill_random`. The seed is wiped internally by
Monocypher's `crypto_wipe` — no secret material survives the call stack.

### First-boot sentinel

After a successful identity generation, a 1-byte `"booted"` sentinel is
written to the `"identity"` namespace. This is the only way to distinguish
a true first boot from a subsequent boot where a prior persistence failure
left no identity. Without the sentinel, a lock that failed to write its
identity on first boot would silently generate a new identity on every boot,
each time losing the one it used to provision phones on the previous boot.

### First-boot key-store wipe

A generated identity means this is a **new lock**. Any phone keys left in
NVS from a previous NVS partition life belong to a different lock identity
and are cryptographically useless. On true first boot, all keys in the
`"keys"` namespace are erased and the RAM cache is zeroed.

### Important: NVS survives `idf.py flash`

`idf.py flash` writes the firmware binary to the app partition. It does NOT
erase the NVS partition. To start fresh (wipe all persisted state, including
identity and provisioned phone keys):

```bash
idf.py erase-flash
idf.py -p /dev/ttyUSB0 flash monitor
```

After `erase-flash`, the next boot is a true first boot — a fresh identity
is generated and all stale keys are cleared.

## Swap-In Plan

| Component | Interface (stable) | Current backend | Swap-in work |
|---|---|---|---|
| Storage | `key_store.h` / `intent_log.h` | `key_store.c` / `intent_log.c` (write-through over NVS HAL) | Implement `storage_hal_se.c` behind the same `storage_hal.h` interface. Select `CONFIG_STORAGE_BACKEND_SE`. |
