# pwr_profile_mgr

Custom Zephyr module implementing a two-state power-profile manager
(`Active` / `Sleep`) for the **STM32F407 Discovery** board, with a
generic SoC-agnostic core and a board-specific backend.

Portfolio project — see `Requirements/`, `Design/`, and `Architecture/`
for the full design record before any implementation code is written.
Source (`pwr_profile_mgr.c`, backend, Kconfig, CMakeLists.txt, sample
app) is added once Phase 0 (board/toolchain confirmation) and the open
points below are resolved.

## Docs in this folder

- [`Requirements/REQUIREMENTS.md`](Requirements/REQUIREMENTS.md) — what the module must do, naming rules, explicitly deferred scope.
- [`Design/DESIGN.md`](Design/DESIGN.md) — state machine, public/internal API, failure-handling (fail-stuck) design.
- [`Architecture/ARCHITECTURE.md`](Architecture/ARCHITECTURE.md) — core/backend split, ops-struct, file layout, implementation phases.

## Implementation phase sequencing

0. Setup/confirmation — confirm board revision and accelerometer part
   (physical inspection), confirm toolchain builds/flashes a known-good
   baseline sample.
1. State machine defined conceptually (done — see `Design/DESIGN.md`).
2. Baseline functional mode — GPIO/LED and UART shell working standalone,
   no PM involved. Reference point for later comparison.
3. Basic suspend capability (system-level) — reliably enter/exit Stop
   mode on a simple trigger (button) before wrapping in the module.
4. Build `pwr_profile_mgr` around it.
5. Integrate peripherals one at a time — LED, then UART, then
   accelerometer last (most complex). Re-validate after each addition.
6. Add RTC wakeup timer as a second, independent wake source.
7. Validation — repeated suspend/resume cycles, confirm state survives
   every time; produce evidence (logs, demo video, before/after).
8. Documentation and packaging.

## Status

Phase 0 (board/toolchain confirmation) complete: board is the STM32F407
Discovery, accelerometer confirmed as the LIS3DSH by physical inspection,
toolchain builds/flashes verified. Generic core (`pwr_profile_mgr.c/.h`,
`pwr_profile_backend.h`, Kconfig, CMakeLists) is implemented and covered
by a passing `native_sim` ztest suite (`tests/core/`) with a
fault-injecting stub backend. Not yet started: the STM32F407 backend
(`pwr_profile_mgr_stm32f4.c` — real Stop-mode entry/exit and the LED/
UART/accelerometer driver callbacks), the button ISR/`k_work` wiring,
the `state_manager` sample app, Devicetree overlay work, and RTC
integration (phases 5–8 below).

## Open points to resolve before/during coding

Tracked in full in `Design/DESIGN.md` §Open Points and
`Architecture/ARCHITECTURE.md` §Open Points. Summary:

| # | Open point | Status |
|---|---|---|
| 1 | Accelerometer part (LIS302DL vs LIS3DSH) — physical board check | Resolved: LIS3DSH |
| 2 | `pwr_profile_backend_ops` struct signatures | Resolved: `pwr_profile_backend.h` |
| 3 | `pwr_profile_set_state()` exact signature/bookkeeping | Resolved: `pwr_profile_mgr.c` |
| 4 | Recoverable vs. unrecoverable error-code contract | Resolved: `pwr_profile_backend.h` comment block |
| 5 | `max_retries` value | Resolved: `CONFIG_PWR_PROFILE_MGR_MAX_RETRIES`, default 3 |
| 6 | Per-step timeout value | Resolved: `CONFIG_PWR_PROFILE_MGR_STEP_TIMEOUT_MS`, default 100ms |
| 7 | Thread-safety: state locking (core) | Resolved: `transition_lock` + `ctx_lock` |
| 7b | Thread-safety: ISR → `k_work` flow (button) | Open — needs the STM32F4 backend/sample-app slice |
| 8 | Kconfig / Devicetree overlay structure | Kconfig resolved; DT overlay (accel/RTC) still open |
| 9 | File/directory layout | Resolved (see Architecture doc) |
| 10 | CLI binary/app name (`state_manager` working name) | Non-blocking default, kept |
| 11 | `status` shell subcommand spec | Open — see Design/DESIGN.md open point 2 |

Also open, not in the original table: the REQ-5 (CLI-triggered resume)
vs. real STM32 Stop-mode tension — Stop mode halts the CPU, so a UART
shell command cannot be typed or processed while actually asleep. This
needs a user decision before the backend's `enter_sleep()`/resume path
is finalized; see the project-level hard-stop list.

## Next step

Implement `pwr_profile_mgr_stm32f4.c`: real Stop-mode entry/exit and the
LED/UART/accelerometer backend driver callbacks, integrated one
peripheral at a time per the phase sequencing below, plus the button
ISR/`k_work` wiring (REQ-15) and the `state_manager` sample app.
