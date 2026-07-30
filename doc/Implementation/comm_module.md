# Comm Module Facade — Implementation

The Comm Module is the topmost layer of the Communication Module and the
**sole header the Application Module is permitted to depend on**. It wires
LLI → Transport → Session into one running stack, owns exactly one FreeRTOS
task (the *comm task*) that drives `transport_run_session`, and hands commands
and session lifecycle events to the Application through a mailbox +
task-notification handoff — so that the Communication Module never executes
application code on its own task.

It is a singleton: this firmware has exactly one PN532 and one lock, so the
facade exposes plain functions rather than a handle. There is exactly one
mailbox, shared between the comm task and the Application task.

## Position in the Stack

```
┌──────────────────────────┐
│   Application Module     │  dispatch / authorization / AAI (external peer)
├──────────────────────────┤
│   Comm Module Facade     │  ← this component (comm_module.h)
├──────────────────────────┤
│      Session Layer       │  mutual-auth handshake, AES-256-GCM, secure erase
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

`comm_module.c` imports `session.h`, `transport.h`, and `lli.h` and constructs
all three handles internally. None of those types appear in `comm_module.h`,
so the Application never needs — and must not — include any of them. This is
enforced at the build-system level: `session`, `transport`, and `lli` are
declared `PRIV_REQUIRES` (implementation-only), so their include directories
do not propagate to anything that depends on `comm_module`. The only types
`comm_module.h` exposes are ESP-IDF/FreeRTOS primitives (`i2c_port_t`,
`TaskHandle_t`) and the facade's own types.

> **The one rule.** No Application source file may `#include` `session.h`,
> `transport.h`, `lli.h`, or any `pn532_*.h`. Every need is met by
> `comm_module.h`. If a task ever needs something this header does not
> expose, that is a signal to extend the facade deliberately, not to reach
> around it.

---

## Lifecycle and Ordering

The facade is a singleton driven by a strict call order:

```
comm_module_init(cfg)            ── build lli/transport/session, no task yet
comm_module_register_app_task(h) ── tell the facade where to notify the app
comm_module_start()              ── spawn the comm task, returns immediately
...                               ── the comm task runs transport_run_session
comm_module_stop()               ── request the comm task to exit
comm_module_deinit()             ── tear down transport/session/lli
```

- **`comm_module_init`** constructs the `lli_config_t`, `session_config_t`
  (wiring the internal mailbox trampolines into `app_handler` and the
  `on_established`/`on_terminated` event seam), and `transport_config_t`, then
  calls `transport_init`. It does **not** spawn a task and does **not** install
  any GPIO or ISR — the single PN532 IRQ is already owned by `pn532_i2c_create`
  as a side effect of `lli_init` (see the master reference §8.0). It is safe to
  discard `cfg` after return; the facade copies it.

- **`comm_module_register_app_task`** must be called exactly once, after the
  Application task exists and before `comm_module_start`. A second call, or a
  `NULL` handle, returns `COMM_ERR_INVALID_ARG`. The facade needs this handle
  because every command and lifecycle event is delivered as a
  `xTaskNotifyGive(g_app_task)`; nothing before this point can deliver either.

- **`comm_module_start`** spawns the comm task. If `register_app_task` was not
  called first, it logs an error and returns without spawning (it is declared
  `void`, so the error is logged, not returned). Stack/priority/core defaults:
  `task_stack_size == 0` → 8192 bytes (crypto + I2C both run on this stack);
  `task_priority == 0` → 5; `task_core_id == tskNO_AFFINITY` → `xTaskCreate`
  (unpinned), any other value → `xTaskCreatePinnedToCore` with that core id.

- **`comm_module_deinit`** deinits transport and session, zeroes the mailbox,
  and clears the task-handle and flag state so a subsequent
  `init → register_app_task → start` cycle begins from a clean slate. It does
  not itself stop a still-running comm task — call `comm_module_stop` first and
  let the task reach the top of its loop before deinit.

Calling out of order is rejected, not silently tolerated: `start` before
`register_app_task` is a no-op (logged); `register_app_task` twice returns
`COMM_ERR_INVALID_ARG`.

---

## The Mailbox Model (application-facing)

The Application never sees the mailbox struct. It interacts only through four
accessors — `comm_module_poll_event`, `comm_module_has_command`,
`comm_module_get_command`, and `comm_module_complete_response` — plus
`comm_module_session_active`. The struct lives entirely inside `comm_module.c`.

The handoff is a **strict ping-pong**, enforced by notification ordering alone
(no mutex, no queue, no dynamic allocation):

- The comm task writes the command/event half and then must not touch the
  mailbox again until the Application has answered.
- The Application task writes the response half, and only after it has been
  notified that something is waiting.

The two directions are **not symmetric**:

**Commands are a bounded, synchronous round-trip.** A phone is sitting over
NFC waiting for an R-APDU, so the comm task must actually wait for the
Application's answer before it can encrypt and send one. The comm task
(populating the mailbox from inside the session's `app_handler` trampoline)
notifies the Application task, then blocks on its *own* task-notification slot
for up to `app_response_timeout_ms`. When the Application calls
`comm_module_complete_response`, that copies the response into the mailbox and
notifies the **comm task** (`xTaskNotifyGive(g_comm_task)`) — which is the only
thing that can wake the blocked `ulTaskNotifyTake` on the comm task's slot.

> **As-built note (diverges from the §8 plan's framing).** §8.4's pseudocode
> writes the comm task's `ulTaskNotifyTake` inside code that runs on the comm
> task, but left the *target* of the matching `xTaskNotifyGive` implicit. The
> as-built `comm_module_complete_response` notifies `g_comm_task`, not
> `g_app_task`. This is not a deviation — it is the only correct target.
> FreeRTOS task notifications are per-task: a task only wakes on a notify aimed
> at *its own* handle. The comm task is the one blocked in `ulTaskNotifyTake`,
> so the Application's `complete_response` must give to `g_comm_task`. Giving
> to `g_app_task` there would land on a slot nobody is waiting on and the comm
> task would never wake.

**Session lifecycle events are fire-and-forget.** No APDU is tied to "a session
started," so the comm task does not wait for an acknowledgement. The
`on_established`/`on_terminated` trampolines set `pending_event` and notify
`g_app_task` (`xTaskNotifyGive(g_app_task)`) without blocking.

**Coalescing.** Both producers notify the *same* Application-task slot, which
behaves as a counting value under Give/Take. A single wake can therefore carry
both an event and a command. The Application's loop must drain **both**
`pending_event` (via `poll_event`) and `command_valid` (via `has_command`) on
every wake, not assume exactly one item per notification. Because
`pending_event` is a single field, two session events that land before the
Application wakes are coalesced to the later one (e.g. a rapid STARTED then
ENDED shows only ENDED) — this is inherent to the single-slot design, which is
why the Application must keep its wake latency short (see the worked example
and §10 of the master reference).

**Timeout honesty.** If the Application does not call `complete_response`
within `app_response_timeout_ms` — stuck in a long integrity check, deadlocked,
crashed — the comm task gives up: it clears `command_valid`, returns an
internal-error status, and Session replies to the phone with an empty
successful R-APDU. The phone sees a normal-looking response for a command that
was never processed. This is a real failure mode worth monitoring for, not one
the facade eliminates. As a hardening measure, `complete_response` is a **no-op
if no command is pending**: a late call after the timeout already cleared
`command_valid` writes nothing and, critically, does not notify — otherwise the
stale `xTaskNotifyGive(g_comm_task)` would prematurely satisfy the *next*
command's `ulTaskNotifyTake` and deliver these bytes as its reply.

---

## Worked Example — Application Event/Command Loop

Adapted from §10 of the master reference, using the as-built function names.
The Application owns exactly one task that both runs its recurring integrity
work and services the mailbox, with a **bounded** wait:

```c
#include "comm_module.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define INTEGRITY_PERIOD_MS   100   /* must be << app_response_timeout_ms */

static void app_task(void *arg)
{
    (void)arg;

    /* The facade needs our handle before it can notify us. */
    comm_module_register_app_task(xTaskGetCurrentTaskHandle());
    comm_module_start();

    const TickType_t period = pdMS_TO_TICKS(INTEGRITY_PERIOD_MS);

    while (1) {
        perform_integrity_checks();   /* application-owned; fixed cadence */

        /* Bounded wait — wakes on any xTaskNotifyGive(app_task), or times out
         * and runs integrity checks again regardless of NFC activity. */
        if (ulTaskNotifyTake(pdTRUE, period)) {
            comm_app_event_t ev = comm_module_poll_event();
            if (ev == COMM_APP_EVENT_SESSION_STARTED) { /* arm a UI indicator */ }
            if (ev == COMM_APP_EVENT_SESSION_ENDED)   { /* clear it */ }

            if (comm_module_has_command()) {
                uint8_t cmd[227];
                size_t  cmd_len = 0;
                /* get_command can fail if the comm task's bounded wait
                 * timed out between has_command() and here — check it. */
                if (comm_module_get_command(cmd, sizeof(cmd), &cmd_len) == COMM_OK) {
                    uint8_t resp[227];
                    size_t  resp_len = build_response(cmd, cmd_len,
                                                      resp, sizeof(resp));
                    comm_module_complete_response(resp, resp_len);
                }
            }
        }
    }
}
```

Two rules this loop honors, both load-bearing:

1. **Pick `INTEGRITY_PERIOD_MS` first, then set `app_response_timeout_ms`
   comfortably above it.** Otherwise integrity checks slip whenever a command
   is being processed. Integrity/tamper monitoring must not depend on NFC
   activity — the bounded wait is what guarantees it runs on cadence whether
   or not a phone is present or the comm task is mid-handshake.

2. **Encode every application-level outcome inside the response bytes.**
   `complete_response` has no "this failed" signal — by design. Access denied,
   wrong lock state, actuator fault, invalid command: all are the
   Application's own status byte inside `resp[]`, delivered through a normal
   non-empty `complete_response`. Authentication (`SESSION_STARTED`) is not
   authorization — gate every real action on the specific command received.

---

## As-Built Divergences from the §8 Plan

The implementation is the source of truth where it differs from §8's design.
Two divergences are worth recording for a future maintainer:

1. **Local `comm_peer_key_provider_t` typedef.** `comm_module.h` declares its
   own `comm_peer_key_provider_t` rather than reusing
   `session_peer_key_provider_t`, so that `session.h` does not leak into the
   Application's include path. The two signatures are byte-for-byte identical
   (`bool (*)(size_t, uint8_t[32], void *)`), so casting one to the other in
   `comm_module_init` is well-defined — typedefs of structurally identical
   function-pointer types are the same type in C. This is the only cast in the
   facade and it is safe only as long as the two typedefs stay identical; if
   `session_peer_key_provider_t` ever changes, `comm_peer_key_provider_t` must
   change to match.

2. **`comm_module_complete_response` notifies `g_comm_task`.** Discussed above:
   required by FreeRTOS per-task notification semantics, not a true deviation.
   §8's pseudocode left the target implicit; the as-built code makes it
   explicit and correct.

The `comm_module_stop` and `comm_module_force_abort` semantics also match §8.5
exactly, including their limitation (see Known Limitations).

---

## API Reference

### Types

#### `comm_err_t`

| Value | Description |
|-------|-------------|
| `COMM_OK` | Success |
| `COMM_ERR_TIMEOUT` | (Reserved) operation timed out |
| `COMM_ERR_INVALID_ARG` | NULL argument, out-of-order call, or buffer too small |
| `COMM_ERR_INTERNAL` | Internal failure (init/transport/session) |

#### `comm_app_event_t`

| Value | Description |
|-------|-------------|
| `COMM_APP_EVENT_NONE` | No event pending |
| `COMM_APP_EVENT_SESSION_STARTED` | Session reached ESTABLISHED (M3 verified) |
| `COMM_APP_EVENT_SESSION_ENDED` | An ESTABLISHED session was erased |

#### `comm_peer_key_provider_t`

```c
typedef bool (*comm_peer_key_provider_t)(
        size_t index, uint8_t pubkey_out[32], void *provider_ctx);
```

Enumerates candidate long-term Ed25519 public keys for M3 verification. M1
carries no identity hint, so the peer's identity is resolved by enumeration:
the session calls this with `index = 0, 1, 2, …` and verifies the M3 signature
against each returned key. Return `true` when `pubkey_out` was filled, `false`
when the list is exhausted (authentication failure). Signature-identical to
`session_peer_key_provider_t`; the value flows into the session config via a
cast in `comm_module_init`.

#### `comm_module_config_t`

Configuration passed to `comm_module_init`. All fields are value-copied; the
caller may discard the struct after return.

| Field | Type | Description |
|-------|------|-------------|
| `sda_gpio` | `int` | I2C SDA pin (→ `lli_config_t`) |
| `scl_gpio` | `int` | I2C SCL pin |
| `irq_gpio` | `int` | PN532 IRQ pin; `-1` for polling mode |
| `rst_gpio` | `int` | PN532 RST pin; `-1` if not wired |
| `i2c_port` | `i2c_port_t` | I2C port (e.g. `I2C_NUM_0`) |
| `i2c_clk_hz` | `uint32_t` | I2C clock; `0` = 400 kHz default |
| `sens_res[2]` | `uint8_t` | ATQA, LSB first |
| `nfcid1[3]` | `uint8_t` | 3-byte NFCID1 |
| `sel_res` | `uint8_t` | SAK byte (`0x20` = ISO14443-4) |
| `nfcid2[8]` | `uint8_t` | 8-byte NFCID2 (FeliCa) |
| `pad[8]` | `uint8_t` | FeliCa padding |
| `system_code[2]` | `uint8_t` | FeliCa system code |
| `nfcid3t[10]` | `uint8_t` | NFCID3t |
| `gt[47]` | `uint8_t` | General bytes (Gt) |
| `gt_len` | `uint8_t` | Length of Gt (0–47) |
| `tk[47]` | `uint8_t` | Historical bytes (Tk / ATS) |
| `tk_len` | `uint8_t` | Length of Tk (0–47) |
| `handshake_timeout_ms` | `uint32_t` | THS — recommended 2000 ms (→ transport) |
| `activate_timeout_ms` | `uint32_t` | How long to wait for a reader tap |
| `apdu_timeout_ms` | `uint32_t` | Per-APDU receive timeout |
| `local_sk[64]` | `uint8_t` | Lock's Ed25519 secret key (seed 32 ‖ public 32) |
| `local_pk[32]` | `uint8_t` | Lock's Ed25519 public key |
| `peer_key_provider` | `comm_peer_key_provider_t` | Required. Candidate enumerator for M3 |
| `peer_key_provider_ctx` | `void *` | Passed to the provider |
| `app_response_timeout_ms` | `uint32_t` | Bounded wait for the Application to answer a command; must exceed the integrity-check period |
| `task_stack_size` | `uint32_t` | `0` = facade default (8192) |
| `task_priority` | `UBaseType_t` | `0` = facade default (5) |
| `task_core_id` | `BaseType_t` | `tskNO_AFFINITY` = unpinned (`xTaskCreate`); otherwise pinned to that core |

### Functions

#### `comm_module_init`

```c
comm_err_t comm_module_init(const comm_module_config_t *cfg);
```

Build the LLI/Transport/Session handles and call `transport_init` internally.
Does not spawn a task. Returns `COMM_ERR_INVALID_ARG` if `cfg` is NULL;
`COMM_ERR_INTERNAL` if session or transport init fails.

#### `comm_module_deinit`

```c
comm_err_t comm_module_deinit(void);
```

Tear down transport and session, zero the mailbox, and clear task-handle/flag
state so a later `init → register_app_task → start` is clean. Always returns
`COMM_OK`. Stop the comm task first.

#### `comm_module_register_app_task`

```c
comm_err_t comm_module_register_app_task(TaskHandle_t app_task);
```

Register the Application task handle. Must be called exactly once, after the
Application task exists and before `comm_module_start`. Returns
`COMM_ERR_INVALID_ARG` for a NULL handle or a second call.

#### `comm_module_start`

```c
void comm_module_start(void);
```

Spawn the comm task (returns immediately). If `register_app_task` was not
called first, logs an error and returns without spawning.

#### `comm_module_stop`

```c
void comm_module_stop(void);
```

Request the comm task to exit at the top of its **next** loop iteration. Does
not block, and does not interrupt a session mid-flight (see Known Limitations).

#### `comm_module_force_abort`

```c
comm_err_t comm_module_force_abort(void);
```

Best-effort abort that sets the comm task's stop flag. Currently functionally
equivalent to `comm_module_stop`: it cannot interrupt a blocking `lli_activate`
or a session in flight (see Known Limitations). Always returns `COMM_OK`.

#### `comm_module_session_active`

```c
bool comm_module_session_active(void);
```

Returns `true` while a session is in the ESTABLISHED stage (set by
`on_established`, cleared by `on_terminated`).

#### `comm_module_poll_event`

```c
comm_app_event_t comm_module_poll_event(void);
```

Return and clear the pending lifecycle event. Call from the Application task
after being notified. Returns `COMM_APP_EVENT_NONE` if none.

#### `comm_module_has_command`

```c
bool comm_module_has_command(void);
```

Returns `true` if an undelivered command is waiting for a response. Does not
consume the command.

#### `comm_module_get_command`

```c
comm_err_t comm_module_get_command(uint8_t *buf, size_t buf_cap,
                                   size_t *len_out);
```

Copy the pending command into `buf`. Returns `COMM_ERR_INVALID_ARG` if `buf` or
`len_out` is NULL, if no command is pending, or if `buf_cap` is smaller than
the command length. On success, `*len_out` receives the command length.

#### `comm_module_complete_response`

```c
void comm_module_complete_response(const uint8_t *response,
                                   size_t response_length);
```

Supply the response to the pending command and notify the comm task in one
call. Bounds-checks `response_length` against the 227-byte mailbox buffer
(silently clamping an over-length response). If no command is pending (e.g. a
late call after the bounded wait timed out), this is a no-op: it writes nothing
and does not notify, so a stale notification cannot corrupt the next command's
response. A `NULL`/zero-length response is delivered as an empty reply.

---

## Known Limitations

**`comm_module_stop` takes effect only at the top of the comm task's loop.**
The comm task's body is `while (!g_stop_requested) transport_run_session(...);`
— the flag is consulted once per *session*, not per APDU. A session already in
flight runs to completion (RELEASED) before the loop checks the flag and exits.
`stop` does not block waiting for the task to exit; the caller that wants a
guaranteed-stopped state before `deinit` must wait for the task to clear its
own handle by other means.

**`comm_module_force_abort` cannot interrupt `lli_activate`.** During the long
blocking wait inside `lli_activate(activate_timeout_ms)` — tens of seconds
waiting for a reader tap — there is no cancellation checkpoint, and the LLI
handle is owned by the transport layer and is not directly reachable from the
facade. `force_abort` therefore only sets the same stop flag as `stop` and
takes effect once a reader taps or that timeout elapses. Closing this gap is
real, scoped future work (a cancel-safe primitive below LLI), not something
this facade already does. It is kept as a distinct entry point so that
future work can strengthen it without changing the public API.
