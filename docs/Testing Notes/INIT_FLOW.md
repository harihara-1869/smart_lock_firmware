# Smart Lock Firmware — Initialization Flow

This document walks the **full boot-time initialization** of the firmware in
`FULL_APPLICATION` test mode (the production path), from `app_main()` to the
point where the application task is running and the comm stack is ready.

It exists because a subtle **ordering bug** lived here: the lock's Ed25519
identity was generated *after* the comm module had already snapshotted an
all-zero copy of it, so every session handshake failed with "invalid lock
signature" on both the mock phone and a real phone. The flow below is the
*correct* order, annotated so the constraint is impossible to miss.

---

## 1. The entry point: `app_main()`

`main/smart_lock_firmware.c`

`app_main()` prints the banner, then branches on the compiled-in test mode.
In `FULL_APPLICATION` it calls `run_full_application()`:

```c
void app_main(void)
{
    ESP_LOGI(TAG, "=== Smart Lock Firmware ===");
    ESP_LOGI(TAG, "test mode: %s",
#if defined(CONFIG_TEST_MODE_FULL_APPLICATION)
             "FULL_APPLICATION");
    run_full_application();
#elif defined(CONFIG_TEST_MODE_COMM_ONLY)
    ...
#endif
}
```

`run_full_application()` is a strictly **sequential** dependency-ordered
sequence — no tasks exist yet. Every step's initialization is performed and
verified before the next begins:

```c
static void run_full_application(void)
{
    ESP_LOGI(TAG, "=== Full Application: dependency-order init ===");

    /* 1. Storage (key store + intent log) — no nvs_flash here; the storage
     *    component owns persistence. */
    if (!init_storage()) {
        ESP_LOGE(TAG, "storage init failed; aborting");
        return;
    }
    /* 2. Display. */
    if (!init_display()) {
        ESP_LOGE(TAG, "display init failed; aborting");
        return;
    }
    /* 3. Actuator — boot-state verification against the intent log happens
     *    inside AppModule_Init (after this, so the intent log is readable). */
    if (!init_actuator()) {
        ESP_LOGE(TAG, "actuator init failed; aborting");
        return;
    }
    /* 4. Integrity. */
    if (!init_integrity()) {
        ESP_LOGE(TAG, "integrity init failed; aborting");
        return;
    }

    /* 5. Comm module. */
    comm_module_config_t cfg = build_comm_config();
    comm_err_t err = comm_module_init(&cfg);
    if (err != COMM_OK) {
        ESP_LOGE(TAG, "comm_module_init failed: %d", err);
        return;
    }

    /* 6. Application coordinator (boot recovery runs inside AppModule_Init). */
    if (AppModule_Init(&cfg) != APP_MODULE_OK) {
        ESP_LOGE(TAG, "AppModule_Init failed");
        return;
    }

    /* 7. Spawn the Application task; it registers with the facade and takes
     *    over as coordinator. */
    if (AppModule_Start() != APP_MODULE_OK) {
        ESP_LOGE(TAG, "AppModule_Start failed");
        return;
    }

#ifdef MOCK_LLI_FOR_TESTING
    /* Let the app task register/start, then run the mock-phone integration
     * test (provisioning flow, key persistence). */
    vTaskDelay(pdMS_TO_TICKS(100));
    xTaskCreate((TaskFunction_t)test_integration_run, "test_task",
                8192, NULL, 5, NULL);
#endif

    /* Keep main task alive so it doesn't clean up memory under other tasks. */
    vTaskSuspend(NULL);
}
```

---

## 2. Step 1 — Storage init (`init_storage()`)

`main/smart_lock_firmware.c`

Storage is the first peer. It warms the key-store RAM cache, initializes the
intent log, and opens the three NVS namespaces:

```c
static bool init_storage(void)
{
    if (key_store_init() != KEY_STORE_OK) {
        ESP_LOGE(TAG, "key_store_init failed");
        return false;
    }
    if (intent_log_init() != INTENT_OK) {
        ESP_LOGE(TAG, "intent_log_init failed");
        return false;
    }
    return true;
}
```

### 2a. `key_store_init()` — NVS + warm the cache

`components/storage/src/key_store.c`

This boots the slow-storage backend (NVS flash) and loads the authorized-key
list into RAM. **All reads from here on are RAM-only.**

```c
key_store_err_t key_store_init(void)
{
    /* Ensure the HAL is ready before reading. */
    storage_err_t serr = storage_hal_init();
    if (serr != STORAGE_OK) {
        ESP_LOGE(TAG, "hal init failed: %d", serr);
        return KEY_STORE_ERR_STORAGE;
    }

    s_count = 0;
    size_t len;

    for (uint8_t i = 0; i < KEY_MAX; i++) {   /* KEY_MAX = 64 */
        char name[4];
        idx_key_name(i, name);

        len = 32;
        storage_err_t serr = storage_hal_read_blob(HAL_NS, name,
                                                   s_keys[i], &len);
        if (serr == STORAGE_ERR_NOT_FOUND) {
            break;  /* no more keys */
        }
        if (serr != STORAGE_OK) {
            ESP_LOGE(TAG, "cache warm failed at index %u: hal error %d", i, serr);
            return KEY_STORE_ERR_STORAGE;
        }
        if (len != 32) {
            ESP_LOGE(TAG, "cache warm: corrupt blob at index %u (len %u)",
                     i, (unsigned)len);
            return KEY_STORE_ERR_STORAGE;
        }
        s_count = i + 1;
    }

    ESP_LOGI(TAG, "cache warmed: %u keys", (unsigned)s_count);
    return KEY_STORE_OK;
}
```

### 2b. `storage_hal_init()` — the NVS backend

`components/storage/src/storage_hal.c`

`key_store_init()` delegates to the HAL, which initializes flash NVS and opens
the three namespaces the storage component owns (`keys`, `intent`, `identity`):

```c
storage_err_t storage_hal_init(void)
{
    if (s_inited) {
        return STORAGE_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return STORAGE_ERR_IO;
    }

    err = nvs_open(NVS_NS_KEYS, NVS_READWRITE, &s_keys_h);
    ...
    err = nvs_open(NVS_NS_INTENT, NVS_READWRITE, &s_intent_h);
    ...
    err = nvs_open(NVS_NS_IDENTITY, NVS_READWRITE, &s_identity_h);
    ...

    s_inited = true;
    ESP_LOGI(TAG, "NVS storage backend ready");
    return STORAGE_OK;
}
```

### 2c. `intent_log_init()`

`components/storage/src/` — loads the persisted lock intent (e.g.
`UNLOCKED`) into RAM, or starts clean if none exists.

```
I (306) STORAGE_NVS: NVS storage backend ready
I (316) KEY_STORE: cache warmed: 0 keys
I (316) INTENT_LOG: no persisted intent — starting clean
```

---

## 3. Steps 2–4 — Display, Actuator, Integrity

`main/smart_lock_firmware.c` — each is a thin init-and-check:

```c
static bool init_display(void)
{
    if (Display_Init() != DISPLAY_OK) { ... return false; }
    return true;
}

static bool init_actuator(void)
{
    if (AAI_Init() != AAI_OK) { ... return false; }
    return true;
}

static bool init_integrity(void)
{
    if (Integrity_Init() != INTEGRITY_INIT_OK) { ... return false; }
    return true;
}
```

> The actuator's **boot-state recovery** (does the physical lock state match
> the persisted intent?) is deliberately *not* here — it runs inside
> `AppModule_Init()` (step 6), after the intent log is readable.

---

## 4. Step 5 — Comm module init (`build_comm_config()` → `comm_module_init()`)

This is the step the ordering bug lived in. The identity must exist **before**
`comm_module_init()` because it *snapshots* the identity into the session.

### 4a. `build_comm_config()` — where the app module takes over

`main/smart_lock_firmware.c`

```c
static comm_module_config_t build_comm_config(void)
{
    comm_module_config_t cfg = AppModule_GetCommConfig();

    /* LLI / I2C — see README "Default GPIO Mapping". */
    cfg.sda_gpio = 8;
    cfg.scl_gpio = 9;
    ...
    /* Timing. */
    cfg.handshake_timeout_ms = 2000;
    cfg.activate_timeout_ms  = 30000;
    cfg.apdu_timeout_ms      = 5000;

    /* Lock identity is populated by AppModule_GetCommConfig() from the key
     * store (generated on first boot, persisted to NVS). No hardcoded test
     * key here. */
    ...
    return cfg;
}
```

### 4b. `AppModule_GetCommConfig()` — identity init lives here

`components/app_module/src/app_module.c`

The application module owns the identity lifecycle. This function **loads or
generates the lock keypair** (idempotently) and copies it into the config —
*before* the comm module can snapshot it:

```c
comm_module_config_t AppModule_GetCommConfig(void)
{
    comm_module_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    /* Timing rule (§10): integrity period first, response timeout above it. */
    cfg.app_response_timeout_ms = APP_RESPONSE_TIMEOUT_MS;
    cfg.peer_key_provider       = AppModule_GetPeerKeyByIndex;
    cfg.peer_key_provider_ctx   = NULL;

    /* Load/generate the lock identity BEFORE this config is snapshotted by
     * comm_module_init — the session signs M2 with its copy of local_sk.
     * If this fails, local_sk/local_pk stay zero and AppModule_Init's own
     * (idempotent) call will report the failure. */
    if (key_store_identity_init() != KEY_STORE_OK) {
        ESP_LOGE(TAG, "identity init failed — lock identity unavailable");
    }

    /* Populate lock identity from the key store. */
    memcpy(cfg.local_sk, key_store_identity_sk(), 64);
    memcpy(cfg.local_pk, key_store_identity_pk(), 32);

    return cfg;
}
```

### 4c. `key_store_identity_init()` — first-boot keygen or NVS load

`components/storage/src/key_store.c`

Idempotent: if the identity is already in RAM, it returns immediately. On a
true first boot it generates an Ed25519 keypair from the CSPRNG, persists it,
and wipes any stale provisioned phone keys. On later boots it just loads the
persisted 96-byte blob (64-byte SK ‖ 32-byte PK).

```c
key_store_err_t key_store_identity_init(void)
{
    /* Idempotent: main's storage init and AppModule_Init both call this. */
    if (s_identity_loaded) {
        return KEY_STORE_OK;
    }

    size_t len = IDENTITY_BLOB_LEN;
    uint8_t blob[IDENTITY_BLOB_LEN];

    storage_err_t serr = storage_hal_read_blob(IDENTITY_NS, IDENTITY_KEY,
                                               blob, &len);
    if (serr == STORAGE_OK && len == IDENTITY_BLOB_LEN) {
        memcpy(s_identity_sk, blob, 64);
        memcpy(s_identity_pk, blob + 64, 32);
        s_identity_loaded = true;
        ESP_LOGI(TAG, "identity loaded from NVS");
        return KEY_STORE_OK;
    }

    /* Blob found but wrong size → corrupt NVS. Treat as a fatal error —
     * generating a new keypair would orphan the old one (provisioned phones
     * would permanently lose access). */
    if (serr == STORAGE_OK) {
        ESP_LOGE(TAG, "persisted identity blob wrong size (%u != %u);"
                      " NVS may be corrupt — refusing to overwrite",
                 (unsigned)len, (unsigned)IDENTITY_BLOB_LEN);
        return KEY_STORE_ERR_STORAGE;
    }

    /* Identity not found. Check the sentinel to distinguish true first boot
     * from a prior failure that left no persisted identity. */
    if (serr == STORAGE_ERR_NOT_FOUND) {
        bool is_first_boot = false;
        size_t sb_len = 1;
        uint8_t sentinel = 0;
        storage_err_t sb_err = storage_hal_read_blob(
                IDENTITY_NS, "booted", &sentinel, &sb_len);
        if (sb_err == STORAGE_ERR_NOT_FOUND) {
            is_first_boot = true;
        }
        ...
        generate_fresh_keypair(s_identity_sk, s_identity_pk);
        ...
        s_identity_loaded = true;
        ESP_LOGI(TAG, "fresh identity generated and persisted");
        ...
        return KEY_STORE_OK;
    }

    /* STORAGE_ERR_IO or STORAGE_ERR_FULL — NVS is unreachable or dead. */
    ESP_LOGE(TAG, "identity init: NVS backend failure (err %d) — "
                  "cannot load or generate identity", serr);
    return KEY_STORE_ERR_STORAGE;
}
```

> **The bug, restated:** previously `build_comm_config()` ran before
> `key_store_identity_init()`, so `cfg.local_sk/local_pk` were all-zero when
> `comm_module_init()` snapshotted them. The QR payload (drawn from the real,
> later-loaded PK) and the M2 signature (drawn from the zero SK) disagreed,
> and every phone rejected the handshake. The identity must be loaded *here*,
> before the snapshot — which is exactly what `AppModule_GetCommConfig()` now
> guarantees.

### 4d. `comm_module_init()` — the snapshot

`components/comm_module/src/comm_module.c`

The comm module copies `local_sk`/`local_pk` into the **session config**, and
the session keeps its own copy in the session handle. From this moment on,
the identity is fixed for the life of the session — M2 will be signed with
whatever `local_sk` was copied here.

```c
comm_err_t comm_module_init(const comm_module_config_t *cfg)
{
    if (!cfg) {
        return COMM_ERR_INVALID_ARG;
    }

    memset(&g_mailbox, 0, sizeof(g_mailbox));
    g_cfg = *cfg;

    /* --- lli_config_t --- */
    lli_config_t lli_cfg = {
        .sda_gpio     = cfg->sda_gpio,
        .scl_gpio     = cfg->scl_gpio,
        .irq_gpio     = cfg->irq_gpio,
        .rst_gpio     = cfg->rst_gpio,
        .i2c_port     = cfg->i2c_port,
        .i2c_clk_hz   = cfg->i2c_clk_hz,
    };
    ...

    /* --- session_config_t --- */
    session_config_t sess_cfg = {
        .local_sk            = {0},
        .local_pk            = {0},
        .peer_key_provider   = (session_peer_key_provider_t)cfg->peer_key_provider,
        .peer_key_provider_ctx = cfg->peer_key_provider_ctx,
        .app_handler         = comm_dispatch_via_mailbox,
        .app_handler_ctx     = NULL,
        .on_established      = comm_on_established,
        .on_terminated       = comm_on_terminated,
        .event_ctx           = NULL,
    };
    memcpy(sess_cfg.local_sk, cfg->local_sk, sizeof(sess_cfg.local_sk));
    memcpy(sess_cfg.local_pk, cfg->local_pk, sizeof(sess_cfg.local_pk));

    session_err_t serr = session_init(&sess_cfg, &g_session);
    if (serr != SESSION_OK) {
        ESP_LOGE(TAG, "session_init failed: %d", serr);
        return COMM_ERR_INTERNAL;
    }

    /* --- transport_config_t --- */
    transport_config_t tcfg = {
        .lli_cfg              = lli_cfg,
        .handshake_timeout_ms = cfg->handshake_timeout_ms,
        .activate_timeout_ms  = cfg->activate_timeout_ms,
        .apdu_timeout_ms      = cfg->apdu_timeout_ms,
        .on_apdu              = session_on_apdu,
        .on_erase             = session_on_erase,
        .user_ctx             = g_session,
    };

    transport_err_t terr = transport_init(&tcfg, &g_transport);
    if (terr != TRANSPORT_OK) {
        ESP_LOGE(TAG, "transport_init failed: %d", terr);
        session_deinit(g_session);
        g_session = NULL;
        return COMM_ERR_INTERNAL;
    }

    g_stop_requested = false;
    g_session_active = false;

    ESP_LOGI(TAG, "initialised");
    return COMM_OK;
}
```

---

## 5. Step 6 — `AppModule_Init()`

`components/app_module/src/app_module.c`

The application module caches the lock PK (for the provisioning response),
then performs **boot-state recovery** — it verifies the physical lock state
against the persisted intent log:

```c
app_module_err_t AppModule_Init(const comm_module_config_t *comm_cfg)
{
    if (!comm_cfg) {
        return APP_MODULE_ERR_INVALID_ARG;
    }

    /* Identity init (idempotent): normally already done by
     * AppModule_GetCommConfig(), before comm_module_init snapshots
     * local_sk/local_pk. This call is a safety net for any path that
     * reaches AppModule_Init without assembling the comm config first. */
    if (key_store_identity_init() != KEY_STORE_OK) {
        ESP_LOGE(TAG, "identity init failed");
        return APP_MODULE_ERR_INIT;
    }

    s_comm_cfg = *comm_cfg;
    /* Cache lock PK from the identity store, not from the (still-unpopulated)
     * comm_cfg. AppModule_GetCommConfig() fills local_pk afterward. */
    memcpy(s_lock_pk, key_store_identity_pk(), sizeof(s_lock_pk));

    s_last_error = APP_LAST_ERROR_NONE;

    /* Boot-state verification against the intent log (§2.3). */
    boot_recovery();

    return APP_MODULE_OK;
}
```

---

## 6. Step 7 — `AppModule_Start()` and the Application task

`components/app_module/src/app_module.c`

The application task is spawned; **it** is what starts the comm task (via
`comm_module_start()`), after registering itself as the app task and wiring
the provision button ISR:

```c
app_module_err_t AppModule_Start(void)
{
    BaseType_t res = xTaskCreate(app_task, "app_task",
                                 APP_TASK_STACK_SIZE, NULL,
                                 APP_TASK_PRIORITY, NULL);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "failed to create app task");
        return APP_MODULE_ERR_TASK;
    }
    return APP_MODULE_OK;
}
```

```c
static void app_task(void *arg)
{
    (void)arg;

    comm_err_t err = comm_module_register_app_task(xTaskGetCurrentTaskHandle());
    if (err != COMM_OK) {
        ESP_LOGE(TAG, "comm_module_register_app_task failed: %d", err);
        vTaskDelete(NULL);
        return;
    }

    /* Event-driven provision button: the ISR notifies THIS task. Init after
     * the task handle exists so the doorbell has a valid target. */
    if (ProvisionButton_Init(xTaskGetCurrentTaskHandle()) != PROV_BUTTON_OK) {
        ESP_LOGW(TAG, "provision button unavailable — provisioning via button disabled");
    }

    comm_module_start();
    ESP_LOGI(TAG, "app task registered; comm module started");

    const TickType_t period = pdMS_TO_TICKS(INTEGRITY_PERIOD_MS);

    while (1) {
        perform_integrity_checks();
        actuation_tick();

        provisioning_button_tick();

        if (ulTaskNotifyTake(pdTRUE, period)) {
            /* Drain BOTH the event and the command on every wake. */
            handle_session_event(comm_module_poll_event());

            if (comm_module_has_command()) {
                ...
            }
        }
    }
}
```

> **Two tasks, one flow.** `comm_module_start()` (inside `app_task`) is what
> spawns the comm/transport task. So "App task and Comm task" are both real,
> but they are **not** the source of the ordering bug — by the time either task
> runs, the identity was already snapshotted correctly during the sequential
> init in `run_full_application()`.

---

## 7. Runtime flow after init

After init, the two tasks cooperate via a mailbox:

| Task          | Started by            | Role                                                                 |
|---------------|-----------------------|----------------------------------------------------------------------|
| `app_task`    | `AppModule_Start()`   | Coordinator: integrity checks, actuation ticks, provision-button ISR evaluation, command dispatch |
| `comm_task`   | `comm_module_start()` | Drives transport (NFC/LLI), session handshake, APDU flow; posts events/commands to the app task's mailbox |

- **QR / provisioning payload** is produced from the *real* lock PK
  (`s_lock_pk`, cached in `AppModule_Init()`).
- **M2 signature** (`Sig_L`) is produced by the session from its *snapshot*
  of `local_sk` — captured at `comm_module_init()` time.
- Both now derive from the **same on-device keypair**, because
  `AppModule_GetCommConfig()` guarantees the identity exists before the
  snapshot.

---

## 8. The fix, summarized

| What changed | Where | Why |
|--------------|-------|-----|
| `key_store_identity_init()` now runs in `AppModule_GetCommConfig()` | `app_module.c` | Runs *before* `comm_module_init()` snapshots `local_sk`/`local_pk` |
| `key_store_identity_init()` is **idempotent** | `key_store.c` | Safe to call from both `AppModule_GetCommConfig()` and `AppModule_Init()` |
| `AppModule_Init()` keeps a safety-net call | `app_module.c` | Guards any init path that skips the config assembly |
| Comments + `docs/storage.md` boot-flow diagram updated | — | The documented order now matches the enforced order |

**Expected boot log** (correct order):

```
I (306) STORAGE_NVS: NVS storage backend ready
I (316) KEY_STORE: cache warmed: 0 keys
I (316) INTENT_LOG: no persisted intent — starting clean
I (316) DISPLAY_CONSOLE: console backend init (...)
I (326) AAI_STUB: stub backend init (...)
I (326) INTEGRITY_STUB: stub backend init (...)
I (336) KEY_STORE: identity loaded from NVS   ← BEFORE comm_module_init
I (336) COMM_MOD: initialised
I (346) APP_MOD: boot recovery: physical=1 intent=0
...
```

> The **identity line now prints before `COMM_MOD: initialised`**. If you ever
> see it print *after* (or not at all), the comm module is running with a
> zeroed identity and every handshake will fail with "invalid lock signature".
