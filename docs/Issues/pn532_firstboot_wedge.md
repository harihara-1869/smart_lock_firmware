I would write this like an engineering issue rather than a few notes. Six months from now you should be able to read it and immediately understand the context, what you've already ruled out, and where to start investigating.

---

# Issue: PN532 occasionally wedges after first unlock following boot

**ID:** FW-PN532-001

**Status:** Deferred

**Priority:** Medium (Prototype Phase)

**Discovered:** During Communication Module + stub Application integration

**Current Phase:** Protocol validation / architecture bring-up

---

# Summary

A rare failure occurs during the first unlock after boot where the PN532 fails to recover cleanly after the NFC session ends.

Originally the failure occurred during the teardown sequence while sending `InRelease (0x52)`.

After switching to a "lazy teardown" (removing `InRelease`), the failure moved to the next session's initialization, specifically `SAMConfiguration (0x14)`.

This indicates progress: the original teardown path is no longer the point of failure, but the PN532 is occasionally left in an unexpected internal state before the next session begins.

The issue is currently rare and does not significantly impact protocol development.

---

# Current Firmware Context

At the time this issue was observed the firmware architecture is incomplete.

Current implementation:

* Communication Module: mostly complete
* Transport / Session / LLI: implemented
* Application Module: scaffold
* AAI: stub only
* Integrity Module: stub only
* Display Module: not implemented
* Motor: simulated using delays
* No RMT
* No limit switches
* No production actuator backend

Therefore all timing observed today is based on temporary scaffolding and is expected to change substantially.

---

# Original Failure

Earlier implementation:

```text
Session End

↓

lli_abort()

↓

InRelease (0x52)

↓

Transport timeout

↓

Recovery
```

Failure:

```
0x52 occasionally failed
```

This resulted in repeated transport retries.

---

# Architectural Change

To support asynchronous "Tap-and-Go" unlocking:

Old behaviour:

```
Unlock

↓

AAI_Open()

↓

Return response
```

New behaviour:

```
Unlock

↓

Return response

↓

SESSION_ENDED

↓

AAI_Open()
```

The Application no longer performs physical actuation while the Communication Module is still completing the NFC transaction.

This removed one possible race between motor activity and response transmission.

---

# Current Failure

After removing `InRelease`, teardown became:

```
Session End

↓

No PN532 commands

↓

Idle

↓

Next session

↓

SAMConfiguration (0x14)

↓

Occasional timeout
```

Failure moved from

```
0x52
```

to

```
0x14
```

This suggests the original problem was not completely eliminated but shifted to the next command in the initialization sequence.

---

# Frequency

Observed characteristics:

* Rare
* Most commonly during the first unlock after boot
* Difficult to reproduce consistently
* Subsequent unlocks generally operate correctly
* Does not permanently wedge the firmware
* Recovery mechanisms usually restore operation

---

# Experiments Already Performed

## Experiment 1 — Lazy teardown

Changed:

```
lli_abort()

↓

Do not send InRelease
```

Result:

* Removed `0x52` failures.
* New occasional failure appears at `0x14`.

---

## Experiment 2 — Immediate phone removal

Procedure:

```
Wait until

Transport -> Active

appears

↓

Immediately remove phone
```

Repeated multiple sessions.

Observed:

* Communication stack reports Link Lost.
* Recovery succeeds.
* New session generally starts normally.
* No consistent wedge observed.

This weakens the hypothesis that every abrupt RF loss leaves unread responses in the PN532.

---

## Experiment 3 — Deferred physical actuation

Changed architecture so that

```
Application

↓

complete_response()

↓

SESSION_ENDED

↓

AAI_Open()
```

instead of

```
Application

↓

AAI_Open()

↓

complete_response()
```

Purpose:

Prevent physical actuation from occurring while the Communication Module is still completing APDU transmission.

This change matches the intended "Tap-and-Go" architecture and should remain regardless of whether it resolves this issue.

---

# Current Hypotheses

## Hypothesis 1 — PN532 internal state not fully reset

Confidence: Medium

The PN532 occasionally remains in an unexpected state after target mode exits.

Evidence:

* Failure shifted from teardown to next initialization.
* Mostly affects first unlock.

Not yet proven.

---

## Hypothesis 2 — Unread response ("Ghost Buffer")

Confidence: Low to Medium

Theory:

```
RF Lost

↓

PN532 creates Target Released response

↓

Host never reads it

↓

Output buffer remains occupied

↓

Next command reads stale frame
```

Evidence:

* Explains why first command may fail.

Counter-evidence:

* Immediate phone removal generally recovers correctly.
* Issue is rare rather than deterministic.

Not yet verified.

---

## Hypothesis 3 — First-session initialization timing

Confidence: Medium to High

The failure is strongly correlated with:

```
Boot

↓

First unlock
```

rather than

```
Session N

↓

Session N+1
```

This suggests a possible initialization race involving:

* SAMConfiguration
* Target mode activation
* Internal PN532 startup timing
* Driver state

---

# Why This Is Deferred

This issue is intentionally **not** being investigated further during the prototype phase.

Reason:

The execution model of the firmware will change substantially before production.

Upcoming architectural changes include:

* Real AAI
* RMT-based motor control
* Limit switches
* Removal of artificial delays
* Real Integrity Module
* Display Module
* Full Application orchestration

These changes will alter task scheduling, timing, and session teardown behaviour.

Fixing a race against temporary stub implementations risks solving the wrong problem.

---

# Conditions Before Reopening

Do **not** investigate further until:

* AAI production backend exists.
* Motor uses RMT.
* Limit switches implemented.
* Stub delays removed.
* Integrity module implemented.
* Full Application architecture stabilized.
* End-to-end protocol considered feature complete.

---

# Investigation Plan

When returning to this issue:

## 1.

Instrument complete session timeline.

Log timestamps for:

```
Boot

↓

SAMConfiguration

↓

Target Activate

↓

M1

↓

M2

↓

M3

↓

Secure Session

↓

Command

↓

Response Complete

↓

SESSION_ENDED

↓

Next SAMConfiguration
```

---

## 2.

Log every PN532 command.

Record:

* command opcode
* timestamp
* return code
* transport status

---

## 3.

Instrument IRQ behaviour.

Record:

* IRQ before command
* IRQ after command
* IRQ after session end
* IRQ before next initialization

---

## 4.

Capture any pending PN532 frame after RF loss.

Determine whether unread responses actually remain inside the chip.

This is the key experiment required to validate or reject the "ghost buffer" hypothesis.

---

## 5.

Compare first session after boot against subsequent sessions.

Specifically compare:

* initialization timing
* command ordering
* IRQ behaviour
* transport timing

---

## 6.

Only after gathering the above evidence should teardown behaviour be reconsidered.

Possible solutions to evaluate at that point include:

* ACK-frame abort
* explicit response draining
* revised teardown sequence
* different initialization sequence

No solution should be adopted without confirming the actual root cause.

---

# Architectural Notes

Regardless of the eventual fix, preserve the following architecture:

* Communication Task remains highest priority.
* Application Task remains lower priority.
* Physical actuation starts **after** the digital transaction completes.
* Application never waits for Communication internals.
* No additional semaphore/yield synchronization between Application and Communication.
* Maintain the single mailbox handoff architecture.

These are architectural invariants and should not be compromised to work around this issue.

---

# Related Changes

* Removal of `InRelease (0x52)` during teardown.
* Deferred actuation until `COMM_APP_EVENT_SESSION_ENDED`.
* Communication/Application mailbox architecture.
* Asynchronous "Tap-and-Go" protocol.
* Planned AAI RMT backend.