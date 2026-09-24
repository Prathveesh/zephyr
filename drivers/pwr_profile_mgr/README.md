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
| 10 | CLI binary/app name (`state_manager` working name) | Non-blocking default, kept; CLI itself deferred (REQ-20) |
| 11 | `status` shell subcommand spec | Deferred with CLI trigger (REQ-20) |

Also open, not in the original table:

- **REQ-3/REQ-5 (CLI trigger) deferred to a later release** — see
  Requirements/REQUIREMENTS.md REQ-20. v1 wires only the button
  (REQ-2/REQ-4). The RX-EXTI-wake design that would satisfy REQ-5
  against real Stop mode is written up in Design/DESIGN.md but parked,
  not implemented, until CLI trigger is back in scope. This also means
  the v1 sample app doesn't need `suspend`/`resume`/`status` shell
  commands yet — it can just observe state via LED/log output.
- **ST-LINK VCP is not wired to UART2 on this board revision** —
  confirmed against Zephyr's own `boards/st/stm32f4_disco` doc and by
  testing (`/dev/ttyACM0` produces zero bytes regardless of
  stty/pyserial config). REQ-10 (UART shell suspend/resume behavior)
  is still in v1 scope — it's not deferred, only the CLI trigger is —
  so reading real UART output for REQ-10 will need either an external
  USB-TTL adapter on PA2(TX)/PA3(RX)/GND, or another plan, decided
  before Phase 5's UART integration slice. Not blocking now since LED/
  button/Stop-mode work doesn't need it.
- **OpenOCD needs `reset_config connect_assert_srst`** to connect
  reliably on this setup — plain `west flash --runner openocd` /
  `west debug --runner openocd` intermittently fail to examine/halt the
  target because Zephyr's idle thread uses WFI and DBGMCU's
  debug-during-sleep bits aren't set by default. Equivalent to the
  `st-flash --connect-under-reset` workaround already noted for that
  tool.

## Next step

Implement `pwr_profile_mgr_stm32f4.c`: real Stop-mode entry/exit
(leveraging Zephyr's own STM32F4 PM subsystem —
`PM_STATE_SUSPEND_TO_IDLE` / `pm_state_force()` — rather than hand-rolled
PWR register writes) and the LED backend driver callback, plus the
button ISR/`k_work` wiring (REQ-15), integrated one peripheral at a time
per the phase sequencing above. UART and accelerometer follow once LED/
button/Stop-mode is validated on hardware.
