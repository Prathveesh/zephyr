# Design — pwr_profile_mgr

Unit-level design of the generic core: state machine, exposed types, and
API. For "what must this do" see
[../Requirements/REQUIREMENTS.md](../Requirements/REQUIREMENTS.md). For
"how the code is split and what interfaces exist between layers" see
[../Architecture/ARCHITECTURE.md](../Architecture/ARCHITECTURE.md).

Items marked **(proposed)** are not finalized and are tracked in
[Open points](#open-points) at the end of this file.

## 1. State machine

```
PWR_PROFILE_ACTIVE
        │  (PWR_PROFILE_CMD_SUSPEND)
        ▼
PWR_PROFILE_SLEEPING   ← internal/transient — NOT exposed to external callers
        │
        ▼
PWR_PROFILE_SLEEP
        │  (PWR_PROFILE_CMD_RESUME)
        ▼
PWR_PROFILE_WAKING     ← internal/transient — NOT exposed to external callers
        │
        ▼
PWR_PROFILE_ACTIVE     (cycle repeats)
```

**Why 4 states, not 2:** a 2-state model (`ACTIVE`/`SLEEP` only, guarded
by a busy boolean for re-entrancy) was seriously considered and nearly
adopted. It was reversed once the transient states were reframed not as
progress-indicators but as the **fail-stuck fault-latch mechanism**
(see `pwr_profile_set_state()` under [Functions](#functions-1)) — a
stronger justification than the original "peripherals take time to
suspend" reasoning. The 4-state model is final.

**Public exposure:** external callers only ever observe
`PWR_PROFILE_ACTIVE` or `PWR_PROFILE_SLEEP` via `pwr_profile_get_state()`.
Transient states are an internal implementation detail used for
fault-latching, never requested or observed directly by external code.

## Implementation phases

See [../README.md](../README.md#implementation-phase-sequencing)
for the full sequencing (phases 0–8).

## List

### Datatypes

- None. No `typedef`s are exposed; all exposed types are `enum`s and
  `struct`s listed below.

### Structs

- `struct pwr_profile_ctx` — internal state record: transient/current
  state, last stable state, fault-latch flag, retry count.

### Enums

- `enum pwr_profile_state` — the four profile states.
- `enum pwr_profile_cmd` — the two trigger commands.

### Functions

- `pwr_profile_get_state()` — public; query current stable state.
- `pwr_profile_suspend()` — public; request Active → Sleep.
- `pwr_profile_resume()` — public; request Sleep → Active.
- `pwr_profile_set_state()` — internal (`static`); single implementation
  of all transition logic.

## Explanation

### Datatypes

None exposed. See [Structs](#structs-1) and [Enums](#enums-1).

### Structs

#### `struct pwr_profile_ctx`

```c
struct pwr_profile_ctx {
	enum pwr_profile_state state;
	enum pwr_profile_state stable_state;
	bool transition_failed;
	uint8_t retry_count;
};
```

**Explanation:**
Holds the module's runtime state. The same `state` enum value
(`SLEEPING`/`WAKING`) is used both transiently during a normal fast
transition and as the permanent fault-latch value when a rollback fails
after retries; `transition_failed` and `retry_count` disambiguate these
so debuggers, logs and callers can tell them apart:

- `state` — direction of the in-flight or failed transition (suspend vs.
  resume) when in a transient value; equal to `stable_state` when idle.
- `stable_state` — the last state that was fully reached and confirmed
  stable (`ACTIVE` or `SLEEP` only). This is what
  `pwr_profile_get_state()` returns, including while a fault is latched
  in `state` — resolves the "what does `get_state()` return during a
  fault" question from the code, not by inventing a third externally
  visible value.
- `transition_failed` — severity/certainty: `false` = normal in-progress
  or stable; `true` = latched fault, retries exhausted.
- `retry_count` — attempt history: number of rollback retries made
  before giving up (0 if none were needed).

A heavier parallel fault-reason enum was proposed and explicitly
rejected in favor of this lightweight form.

Internal to the core, not exposed via the public header. Whether/how the
planned `status` shell subcommand reads `transition_failed` and
`retry_count` is still open — see [Open points](#open-points) item 6.

**Boundary values:**

| Field | Lower | Upper |
|---|---|---|
| `state` | `PWR_PROFILE_ACTIVE` | `PWR_PROFILE_WAKING` |
| `transition_failed` | `false` | `true` |
| `retry_count` | `0` | `CONFIG_PWR_PROFILE_MGR_MAX_RETRIES` (proposed default 3; type ceiling 255) |

**Valid values:**

- `transition_failed == true` is valid only when `state` is
  `PWR_PROFILE_SLEEPING` or `PWR_PROFILE_WAKING`.
- `retry_count > 0` is valid only when a rollback has been attempted.
- `transition_failed == false` with `retry_count == 0` is the only valid
  combination for `ACTIVE` and `SLEEP`.

### Enums

#### `enum pwr_profile_state`

```c
enum pwr_profile_state {
	PWR_PROFILE_ACTIVE,
	PWR_PROFILE_SLEEPING,
	PWR_PROFILE_SLEEP,
	PWR_PROFILE_WAKING,
};
```

**Explanation:**
The four power-profile states (see [State machine](#1-state-machine)).
`ACTIVE` is normal operation. `SLEEP` is the low-power, SRAM-retained
state and maps to STM32 **Stop mode**, not STM32's own lighter "Sleep
mode" (RM0090). `SLEEPING` and `WAKING` are internal transient states
that double as the fault-latch value when a transition cannot be rolled
back.

**Boundary values:**

- Lower: `PWR_PROFILE_ACTIVE`
- Upper: `PWR_PROFILE_WAKING`

Explicit integer values are not assigned in the design (proposed:
implicit `0`–`3` in the order shown).

**Valid values:**

- Internally: all four.
- As returned to external callers by `pwr_profile_get_state()`: only
  `PWR_PROFILE_ACTIVE` and `PWR_PROFILE_SLEEP`.
- As the `target` of `pwr_profile_set_state()`: only
  `PWR_PROFILE_ACTIVE` and `PWR_PROFILE_SLEEP` (proposed; transients are
  sequenced internally).

#### `enum pwr_profile_cmd`

```c
enum pwr_profile_cmd {
	PWR_PROFILE_CMD_SUSPEND,
	PWR_PROFILE_CMD_RESUME,
};
```

**Explanation:**
The two trigger commands. Every trigger source (button, shell) reduces
to one of these, so the state machine never knows which source fired
(REQ-6). Shell subcommands exposed to the user are plain verbs
(`suspend`, `resume`) and do not carry this prefix.

**Boundary values:**

- Lower: `PWR_PROFILE_CMD_SUSPEND`
- Upper: `PWR_PROFILE_CMD_RESUME`

**Valid values:** both.

### Functions

#### `pwr_profile_get_state()`

```c
enum pwr_profile_state pwr_profile_get_state(void);
```

**Description:**
Returns the current profile state. Named with the `_get_` accessor
suffix to match Zephyr convention. Never returns a transient value to
external callers: it reads `ctx.stable_state`, not `ctx.state`, so
while a transition is in flight, or after a fault latch, it returns the
last state that was fully reached — not the in-progress/latched
transient. It is a plain spinlock-guarded read, not itself a
hardware-touching path, so it is not subject to the per-step timeout.

**Arguments:**

| Name | Direction | Type | Boundary values |
|---|---|---|---|
| — | — | `void` | n/a |

**Return values:**

Returns an `enum pwr_profile_state` directly, not an errno.

| Value | Meaning |
|---|---|
| `PWR_PROFILE_ACTIVE` | Module is in normal operation. |
| `PWR_PROFILE_SLEEP` | Module is in the low-power state. |

**Thread-safe:** YES — reads `stable_state` under `ctx_lock`, the same
spinlock that guards writes.

#### `pwr_profile_suspend()`

```c
int pwr_profile_suspend(void);
```

**Description:**
Requests the Active → Sleep transition. A thin, intent-expressing
wrapper that calls `pwr_profile_set_state(PWR_PROFILE_SLEEP)`; it must
not duplicate any transition logic. Must be called from thread context
only — never from an ISR (Stop-mode entry and SPI/UART operations are
unsafe in interrupt context). The button ISR therefore submits a
`k_work` item, which calls this function from thread context.

**Arguments:**

| Name | Direction | Type | Boundary values |
|---|---|---|---|
| — | — | `void` | n/a |

**Return values:**

| Macro | Meaning |
|---|---|
| `0` | Suspended; state is now `PWR_PROFILE_SLEEP`. |
| `-EALREADY` | Already in `PWR_PROFILE_SLEEP`; nothing done. |
| `-EBUSY` | Another transition is in flight. |
| `-EAGAIN` | Recoverable failure (incl. timeout); all drivers rolled back, state reverted to `PWR_PROFILE_ACTIVE`. |
| `-EIO` | Unrecoverable failure or rollback failed after retries; state latched at `PWR_PROFILE_SLEEPING`, `transition_failed == true`. Target must be reset. |

**Thread-safe:** YES — serialized against `resume()` and concurrent
callers by `transition_lock`. **Not ISR-safe.**

#### `pwr_profile_resume()`

```c
int pwr_profile_resume(void);
```

**Description:**
Requests the Sleep → Active transition. Thin wrapper calling
`pwr_profile_set_state(PWR_PROFILE_ACTIVE)`, with the same thread-context
restriction as `suspend()`. The wake sources (button GPIO/EXTI, RTC
wakeup timer) reach it through deferred work, not directly from their
ISRs.

**Arguments:**

| Name | Direction | Type | Boundary values |
|---|---|---|---|
| — | — | `void` | n/a |

**Return values:**

| Macro | Meaning |
|---|---|
| `0` | Resumed; state is now `PWR_PROFILE_ACTIVE`. |
| `-EALREADY` | Already in `PWR_PROFILE_ACTIVE`; nothing done. |
| `-EBUSY` | Another transition is in flight. |
| `-EAGAIN` | Recoverable failure (incl. timeout); rolled back, state reverted to `PWR_PROFILE_SLEEP`. |
| `-EIO` | Unrecoverable failure or rollback failed after retries; state latched at `PWR_PROFILE_WAKING`, `transition_failed == true`. Target must be reset. |

**Thread-safe:** YES — same as `suspend()`. **Not ISR-safe.**

#### `pwr_profile_set_state()` (internal)

```c
static int pwr_profile_set_state(enum pwr_profile_state target);
```

**Description:**
The single source of truth for every transition; `suspend()` and
`resume()` are thin callers. This mirrors how Zephyr's own PM subsystem
is structured (a general transition function with named actions as
callers). The design is a deliberate **fail-stuck** model — not
fail-closed, not silent rollback-and-pretend — analogous to automotive
ECU graceful degradation and DTC-style fault reporting.

Forward path:

1. Enter the matching transient state (`SLEEPING` for target `SLEEP`,
   `WAKING` for target `ACTIVE`).
2. Make a **single** attempt, no retry on the forward path. Every
   hardware step (each driver's suspend/resume, and Stop-mode
   entry/exit) is bounded by a per-step timeout. Expiry is a failure
   and feeds the same rollback logic, not a special case.
3. On success, set the stable target state and return `0`.

On failure, classify the backend's error first:

- **Recoverable** (e.g. timeout, `-EAGAIN`-style): roll back.
- **Unrecoverable** (e.g. `-EIO`-style, bus lockup): rollback may itself
  be unsafe — skip straight to the fault latch.

Rollback:

- Each driver (LED, UART shell, accelerometer) owns its own rollback
  routine.
- **Walk every driver in the rollback set; do not stop at the first
  failure.** Stopping early would sacrifice diagnostic completeness —
  the goal is that the user can see which driver is causing the error.
  The cost is negligible with three lightweight onboard drivers.
- If a driver's rollback fails, retry **that driver only**, up to
  `max_retries`. Retry is per-driver; drivers that already rolled back
  are not restarted.
- Log each failing driver and its retry outcome.

Terminal outcomes:

- **All rollbacks succeed:** revert to the previous stable state and
  return the original failure to the caller. Hardware is known-good;
  the transition simply did not happen.
- **Any rollback still fails after retries:** do **not** revert. Latch
  `state` at the transient value already reached, set
  `transition_failed = true`, record `retry_count`, log full per-driver
  detail, and tell the caller the target must be reset. No further
  hardware operations are attempted, since hardware state is no longer
  trustworthy. Once latched, later calls return `-EIO` immediately.

**Arguments:**

| Name | Direction | Type | Boundary values |
|---|---|---|---|
| `target` | in | `enum pwr_profile_state` | `PWR_PROFILE_ACTIVE` or `PWR_PROFILE_SLEEP` only. Transient values return `-EINVAL`. |

**Return values:**

| Macro | Meaning |
|---|---|
| `0` | Transition succeeded. |
| `-EINVAL` | `target` is a transient or out-of-range value. |
| `-EALREADY` | Already in `target`. |
| `-EBUSY` | Another transition is in flight. |
| `-EAGAIN` | Recoverable failure; rolled back, prior state restored. |
| `-EIO` | Unrecoverable failure or fault latched. |

**Thread-safe:** YES — `transition_lock` (a `k_mutex`, non-blocking
`K_NO_WAIT` acquire so a concurrent caller gets `-EBUSY` instead of
blocking) is held across the whole transition; `ctx_lock` (a
`k_spinlock`) guards individual field reads/writes within it.
**Not ISR-safe.** Internal; not callable from outside the core.

## Open points

Resolved since the last pass (implemented in `pwr_profile_mgr.c`,
`pwr_profile_backend.h`, `Kconfig`):

- `pwr_profile_set_state()` signature/bookkeeping — `static int
  pwr_profile_set_state(enum pwr_profile_state target)`, backed by
  `struct pwr_profile_ctx` (documented above).
- Return-code contract — the errno tables above are no longer proposed;
  they match what the code returns.
- `max_retries` — `CONFIG_PWR_PROFILE_MGR_MAX_RETRIES`, default 3.
- Per-step timeout — `CONFIG_PWR_PROFILE_MGR_STEP_TIMEOUT_MS`, default
  100 ms.
- Thread-safety mechanism (core only) — `transition_lock` +
  `ctx_lock`, as described above.
- Button ISR → `k_work` flow (REQ-15) — implemented in
  `pwr_profile_mgr_stm32f4.c`: the GPIO/EXTI0 ISR only calls
  `k_work_submit()`; the work handler (thread context) reads
  `pwr_profile_get_state()` and calls `pwr_profile_suspend()` or
  `pwr_profile_resume()` accordingly. Validated on hardware.

Still open:

1. **`struct pwr_profile_ctx` exposure** — whether `transition_failed`
   and `retry_count` are visible to callers (e.g. an extended query or
   the `status` shell subcommand). `pwr_profile_get_state()` itself is
   resolved: it always returns `stable_state`, even while a fault is
   latched. Deferred with the CLI trigger (REQ-20) — no `status`
   subcommand exists yet to need it.
2. **`status` shell subcommand** — deferred with CLI trigger (REQ-20),
   not a v1 concern.

## STM32F4 backend: Stop-mode entry/exit and LED driver (implemented 2026-09-24)

**`enter_sleep()`/`exit_sleep()` build on Zephyr's own STM32F4 PM
subsystem, not hand-rolled PWR/RCC register writes.**
`soc/st/stm32/stm32f4x/power.c` already implements real Stop-mode entry
for `PM_STATE_SUSPEND_TO_IDLE` (`LL_PWR_SetPowerMode(LL_PWR_MODE_STOP_LPREGU)`
+ deep-sleep + WFI), tied to the "stop" state in
`dts/arm/st/f4/stm32f4.dtsi`'s `power-states` node. Reusing this instead
of writing new register-level code is both correct per the "no
hallucination, verify against real Zephyr source" rule and avoids
duplicating already-verified upstream logic.

`enter_sleep()` only calls `pm_state_force(0, stop_state)` — it does
**not** itself block until the CPU wakes. The actual WFI halt happens
later, naturally: once `forward()` finishes and the calling thread (the
button's `k_work` handler) has nothing left to do, it becomes idle, and
Zephyr's own idle thread — seeing the forced state — performs the real
Stop-mode entry. Wake, when it comes (button EXTI0), is handled by
Zephyr's own `pm_state_exit_post_ops()` (clock restoration) *before*
any wake-source ISR runs, so `exit_sleep()` has nothing left to do and
is a no-op. This means the actual CPU halt happens outside the
`pwr_profile_suspend()` call's own execution — a deliberate consequence
of Zephyr's idle-driven PM model, not a gap: `pwr_profile_suspend()`
still returns promptly (well within `STEP_TIMEOUT`) with `stable_state
== PWR_PROFILE_SLEEP`, and the CPU physically sleeps once nothing else
is runnable, which in this single-purpose module is immediately after.

**LED driver (REQ-9, REQ-21) preserves blink phase, not just on/off
state, and drives all four onboard LEDs as a single state indicator.**
A `k_timer` drives periodic toggling of the three "active" LEDs
(green/orange/blue — `led0`/`led1`/`led3`) together, in sync.
`suspend()` calls `k_timer_remaining_get()` *before* stopping the
timer, saving how far through the current half-period it was;
`resume()` restarts the timer with that saved remainder as a one-shot
first expiry, then falls back to the normal period — so the blink
continues from where it left off rather than restarting fresh. On
`suspend()`, the three active LEDs go dark and the red LED (`led2`)
turns solid on; on `resume()`, red turns off and the three active LEDs
are restored to their last on/off level immediately, before the timer
restarts. `suspend()`/`resume()` are both idempotent (guarded by a
`led_running` flag), so `rollback()` can call either one
unconditionally depending on which direction it's undoing. Validated
on hardware with the user for both the single-LED (REQ-9) and
four-LED (REQ-21) versions.

## Live state-transition logging over RTT (added 2026-09-24)

`pwr_profile_mgr.c`'s `set_state()` logs `LOG_INF("now %s", ...)` on
every successful transition (`ACTIVE`/`SLEEP`) — generic core code, not
backend-specific, since it's describing the same state change REQ-6
already unifies across trigger sources. The board-level question is
how the *user* sees it: this board's ST-LINK VCP has no UART bridge
(see README's "ST-LINK VCP is not wired" note), so `state_manager`'s
`prj.conf` routes logging over **RTT** instead
(`CONFIG_USE_SEGGER_RTT`, `CONFIG_LOG_BACKEND_RTT`) — a RAM ring buffer
the debug probe reads in the background over the same SWD connection
already used for flashing, no extra hardware. `USE_SEGGER_RTT` on
STM32 auto-selects `STM32_ENABLE_DEBUG_SLEEP_STOP`, which keeps DBGMCU
debug clocks alive during Sleep/Stop specifically so RTT keeps working
then too — this doesn't defeat real Stop mode (the core clock still
gates, execution still halts), only the debug-support clock domain
stays up. See README for the exact commands to view it live
(`openocd`'s `rtt server`, then `telnet`/`nc`).

## REQ-5 vs. real STM32 Stop mode (parked with REQ-5, 2026-09-24)

**Status:** REQ-5 (CLI-triggered resume) is deferred out of v1 scope
(Requirements/REQUIREMENTS.md REQ-20), so this design is parked, not
active. v1 wires only the button (REQ-2/REQ-4) as a trigger source, so
the RX-EXTI-wake mechanism below is not implemented for v1. Kept here
because it's still the intended design once REQ-3/REQ-5 come back into
scope — no rework needed then, just implementation.

**The tension:** real STM32 Stop mode halts the CPU clock entirely,
including the USART peripheral, so a UART byte cannot be received or
decoded while the board is actually asleep — REQ-5 ("CLI-triggered
resume") cannot mean "type `resume` in the shell and have it parsed
while in Stop mode," taken literally.

**Resolution:** the STM32F4 backend configures the debug
USART's RX pin as a plain GPIO/EXTI line — independent of the USART
peripheral itself, the same mechanism already used for the button
(EXTI0/PA0, REQ-2/REQ-4) — so any falling edge (the start bit of an
incoming byte) wakes the CPU from Stop mode. The byte that caused the
edge is lost, because the USART was unclocked and never captured it; on
wake, the shell backend's resume path prints a note that the triggering
keystroke was not parsed, and the user re-issues `resume` normally now
that the CPU is running and the USART is live again. This is a literal,
physically real "a CLI keystroke causes wake" path — REQ-5 is satisfied
by the RX-edge-wakes-CPU mechanism plus a genuine `resume` command
completing the transition once the CPU is back up, not by redefining
REQ-5 to only apply pre-Stop-mode-entry or only on `native_sim`.

Implementation implications for the STM32F4 backend (not yet written):
- The UART RX EXTI line and the button EXTI0 line both feed the same
  deferred `k_work` → `pwr_profile_resume()` path (REQ-6, REQ-15) —
  the core has no visibility into which one fired.
- `enter_sleep()` must configure the RX pin's EXTI trigger (and restore
  normal USART RX pin muxing) as part of Stop-mode entry/exit,
  verified against `/home/pns/DS_dm00037051.pdf`'s GPIO/EXTI and USART
  chapters and the STM32F4 Discovery board's actual USART/pin wiring —
  not assumed from general STM32 recollection.
