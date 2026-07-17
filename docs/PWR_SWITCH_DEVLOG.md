# Dev log: host power-switch trigger feature

Session record for the `pwr_switch.{h,cpp}` feature (2026-07-16), kept here so the design context
survives across machines (this repo is worked on from both a desktop and a laptop) even though the
durable reference for the feature itself is `ARCHITECTURE.md` + `CODE_MAP.md`. This file is a log,
not a spec -- if it and `ARCHITECTURE.md` ever disagree, `ARCHITECTURE.md` is correct.

## The ask

> This project is to add a feature to OpenPuck's software: add a GPIO trigger on the NRF board that
> will be physically connected to the power switch on a motherboard. The trigger is to come from the
> Steam button short press of a connected steam controller, and it is ONLY to fire when the system is
> in an off state and USB has not connected. It is assumed the Steam Controller is already paired with
> the receiver, and that the USB port will have power while the motherboard is in the off state -- so
> the receiver should keep listening for a connection and a Steam-button press throughout. Changes
> should follow the style/format/rules in AGENTS.md, ARCHITECTURE.md, and CODE_MAP.md (read first).
> Open questions raised up front: whether the GPIO toggling should be its own .cpp/.h file (matching
> the rest of the codebase's one-file-per-feature layout), and how to make the feature optional for
> people without the extra hardware.

## Research (before planning)

Read `AGENTS.md`, `ARCHITECTURE.md`, `CODE_MAP.md` in full, plus the following code, to ground the
plan in what already exists rather than inventing new patterns:

- **`rf_link.cpp:460-495`** -- the existing "Steam-button short press" detector (down+up within 1s,
  per-bond-slot static state to survive round-robin polling), used today to fire USB remote wakeup
  while the host is *suspended*. This is the gesture to reuse; the new feature is its off-state
  complement.
- **`rf_link.cpp:498-562`** -- the Steam+Y 2s-hold power-OFF chord (controller power-off, not host
  power-off) -- a second precedent for time-based, per-slot, latch-once gesture detection.
- **`haptics.cpp:725-750`** -- the existing VBUS/suspend debounce (`SUSPEND_OFF_MS`, `USBREGSTATUS`)
  used to autonomously power the *controller* off after a genuine (not selective-suspend) host sleep.
  Closest existing "gate behavior on host power state" precedent.
- **`haptics.cpp:700-722`** (`hapticOnReconnect`) -- an independent, second edge-detector for the same
  link-up signal `rf_link.cpp` already tracks, rather than sharing state with it. This became the
  justification for giving the new feature its own independent Steam-press detector too, instead of
  threading a hook through `rf_link.cpp`.
- **`status_led.h`/`status_led.cpp`** -- the template for "a small, self-contained GPIO module":
  overridable pin/polarity macros, `xxxInit()`/`xxxTask()`, non-blocking pulse-then-release via a
  stamped `millis()` and a `PULSE_MS` timeout. Used as the direct template for `pwr_switch.{h,cpp}`.
- **`config.h`'s `OPK_FACTORY_RESET`** -- the precedent for an opt-in, compile-time-gated feature
  (`#ifndef OPK_X / #define OPK_X 0 / #endif`, guarded call sites in `OpenPuck.ino`). Used as the
  precedent for `OPK_PWR_SWITCH`.
- **`OpenPuck.ino`'s `setup()`/`loop()`** -- confirmed there's no task scheduler, just an ordered list
  of non-blocking `*Init()`/`*Task()` calls; a new feature hooks in the same way `status_led` does.
- Confirmed via grep: **no existing pins.h/board file**, no `tud_mount_cb`/`tud_umount_cb` usage
  anywhere (`USBDevice.mounted()`/`.suspended()` are polled, not callback-driven), and the whole GPIO
  surface in this codebase before this change was just `status_led.cpp`'s two LED pins.

## Design decisions

Two were surfaced to the user explicitly (via a clarifying-question step) before finalizing the plan:

1. **Enable/disable mechanism** -- chose **compile-time flag only** (`OPK_PWR_SWITCH`, default 0),
   not a persisted runtime toggle. Reasoning offered: matches the existing `OPK_FACTORY_RESET`
   precedent exactly, keeps the change minimal, and a runtime toggle can be layered on later the same
   way `g_padHaptics` etc. already are, if ever wanted. User picked this (the recommended option).
2. **Trigger pin** -- chose to **pick a sensible free default** or, rather than ask the user to
   pre-commit to real wiring, expose it as an overridable macro (`#ifndef PWR_SWITCH_PIN`) following
   `WAKE_LED_PIN_A`'s pattern, so it's a one-line override once the real relay wiring is decided.
   User picked this (the recommended option) over supplying a specific pin.

Other decisions made without asking (judgment calls, not preference calls):
- **Gating signal for "host off"**: `USBDevice.mounted() == false`, debounced ~1.5s continuous. This
  is a direct, literal read of the user's own stated requirement ("USB has not connected"), and the
  user's own stated assumption (USB port stays powered through host-off) means `VBUS` alone can't be
  used to distinguish on/off -- unlike the existing `haptics.cpp` VBUS-gated logic, which solves a
  different problem (genuine sleep vs. selective suspend on a host that IS attached).
- **Independent short-press detector** (reading `g_in[]`/`g_connReplyMs[]` directly from the new
  module) rather than adding a hook to `rf_link.cpp`. Justified by the `hapticOnReconnect` precedent
  above -- this codebase already tolerates a second consumer re-deriving the same edge independently
  rather than centralizing it, and it keeps `rf_link.cpp` (a protocol-decode file, per `AGENTS.md`'s
  own framing) untouched.
- **Per-slot link-up guard before trusting `g_in[s]`**: found during design, not in the original
  ask -- a mid-press link drop forces `g_in[s]` to zero elsewhere in `rf_link.cpp`, which a naive
  poll of `g_in[s].buttons` would misread as a button release. Guarded with the same 300ms threshold
  `anySlotLinkUp()` already uses.
- **300ms pulse / 5s retrigger cooldown**: reasonable defaults for a momentary front-panel switch
  pulse and to prevent rapid double-fire; called out as adjustable constants, not load-bearing design.

## The approved plan

<details>
<summary>Full plan as written and approved (click to expand)</summary>

# Host power-switch trigger (Steam-button short press, host-off gated)

## Context

OpenPuck currently only ever *listens* to a paired Steam Controller and forwards its input over USB — it has
no way to act on the physical world. The goal here is a new capability: while the connected PC's motherboard is
fully off, a short press of the controller's STEAM button should pulse a GPIO pin wired (through an external
relay/transistor/optocoupler — never a bare GPIO-to-header short) to the motherboard's front-panel power-switch
header, powering the PC on. It must only fire in that specific state — host off, USB not enumerated — never
while the PC is up, so a stray Steam-button tap during normal use can't accidentally power-cycle anything. The
receiver is assumed to keep running throughout (USB port keeps standby power while the host is off) and the
controller is assumed already bonded, so no pairing UI is needed.

This directly extends the existing "Steam-button short-press" gesture already recognized in `rf_link.cpp` for
USB remote wakeup — that logic wakes a *sleeping-but-still-USB-attached* host; this feature covers the
complementary case where the host is genuinely powered off (never mounted). The two stay independent: neither
should change the other's behavior.

Per this repo's own conventions (`AGENTS.md`/`ARCHITECTURE.md`/`CODE_MAP.md`), the design record for this
feature lives in-repo (`ARCHITECTURE.md` + `CODE_MAP.md` updates below) rather than in any local tool memory —
that's what makes it available from any machine that clones the repo, matching the multi-host (desktop/laptop)
workflow.

## Design decisions (confirmed with user)

- **Enable/disable**: compile-time only, new `OPK_PWR_SWITCH` flag in `config.h` (default `0`), following the
  exact `OPK_FACTORY_RESET` precedent. When off, the feature's call sites simply aren't compiled in — zero
  behavior/risk change for every existing build. No persisted runtime toggle for v1 (can be added later the
  same way `g_padHaptics` etc. are, if wanted).
- **Trigger pin**: pick a sensible free default (not `LED_BUILTIN`/P1.15 or pin 24/P0.15, both already claimed
  by `status_led.cpp`), exposed as an overridable `#ifndef PWR_SWITCH_PIN` macro — same pattern as
  `WAKE_LED_PIN_A` in `status_led.h` — so it can be repointed via `-DPWR_SWITCH_PIN=<n>` or a header edit once
  the real wiring is decided, without touching the logic.

## New files: `OpenPuck/pwr_switch.h` / `OpenPuck/pwr_switch.cpp`

Following the `status_led.h/.cpp` template (the codebase's existing "small, self-contained GPIO module" model)
and this repo's file-per-feature layout documented in `CODE_MAP.md`.

**Header** (`pwr_switch.h`) declares:
```cpp
#ifndef PWR_SWITCH_PIN
#define PWR_SWITCH_PIN 5          // free on Feather/SuperMini; override per your wiring
#endif
#ifndef PWR_SWITCH_ACTIVE
#define PWR_SWITCH_ACTIVE HIGH    // level that closes the switch; LOW if your driver is active-low
#endif

void pwrSwitchInit();   // call once from setup(): pin to OUTPUT, released level
void pwrSwitchTask();   // call every loop(): edge-detect + gate + non-blocking pulse
```

**Implementation** (`pwr_switch.cpp`) — all state is loop-task-only (no ISR/usbd-task involvement, no new
cross-task shared state):

1. **Host-off debounce**: track how long `USBDevice.mounted()` has continuously read `false`
   (`s_hostOffSinceMs`, reset to 0 the instant it reads `true`). `hostOff` is only asserted once that's held for
   `HOST_OFF_DEBOUNCE_MS` (~1.5s), so a brief re-enumeration blip (e.g. a `modeSwitchReboot()` on this same
   device, or a cable reseat) can't false-trigger. This directly implements "USB has not connected."

2. **Steam short-press edge detection, per bond slot** — reuses the exact down→up-within-1000ms pattern from
   `rf_link.cpp:465-494`, but reads the shared `g_in[s].buttons & TB_STEAM` (triton.h) directly from this
   module's own `pwrSwitchTask()` instead of touching `rf_link.cpp`. This mirrors an existing precedent in this
   codebase: `haptics.cpp`'s `hapticOnReconnect` edge-detector independently re-derives the same link-up signal
   `rf_link.cpp` already tracks, rather than sharing state — so a second, independent Steam-press detector here
   is consistent with how this codebase already handles "another subsystem needs its own edge-detect on a signal
   someone else also watches."
   - **Correctness detail**: must gate per-slot processing on that slot actually being link-up
     (`millis() - g_connReplyMs[s] < 300`, the same threshold `anySlotLinkUp()` uses). Without this, a
     mid-press link drop forces `g_in[s]` to zero (`rf_link.cpp`'s own down-edge handling), which would look
     like a same-frame "release" to a naive reader of `g_in[s].buttons` and could false-fire. On a not-linked-up
     slot, reset `steamWasDown[s] = false` and skip it.

3. **Fire gate**: a completed short press only pulses the pin if `hostOff`, no pulse is currently in flight, and
   `RETRIGGER_COOLDOWN_MS` (~5s) has elapsed since the last pulse (prevents rapid re-trigger from a fast double
   tap or bounce).

4. **Non-blocking pulse**: `firePulse()` drives `PWR_SWITCH_PIN` to `PWR_SWITCH_ACTIVE` and stamps the start
   time; `pwrSwitchTask()` releases it back to the inactive level after `PULSE_MS` (~300ms) on a later call —
   same non-blocking timeout idiom as `status_led.cpp`'s `ledTask()`/`PULSE_MS`. Never uses `delay()`, matching
   the "every subsystem hook must be non-blocking" rule in `ARCHITECTURE.md`.

## Changes to existing files

- **`OpenPuck/config.h`**: add, next to `OPK_FACTORY_RESET`:
  ```cpp
  #ifndef OPK_PWR_SWITCH
  #define OPK_PWR_SWITCH 0
  #endif
  ```
  with a comment explaining the feature, the default-off/opt-in rationale, and pointing at `pwr_switch.h` for
  the pin/timing knobs.

- **`OpenPuck/OpenPuck.ino`**:
  - `#include "pwr_switch.h"` alongside the other module includes.
  - In `setup()`, next to `ledInit();`: `#if OPK_PWR_SWITCH` / `pwrSwitchInit();` / `#endif`.
  - In `loop()`, next to both `ledTask();` call sites (the `#if OPK_LOG` timed branch and the plain `#else`
    branch): `#if OPK_PWR_SWITCH` / `pwrSwitchTask();` / `#endif`. Left out of the `acc[]` per-section timing
    array in the `OPK_LOG` branch, matching how `usbMountTask()`/`usbTxPump()`/`puckCmdLogDrain()` are already
    called untimed at the tail of both branches.

- **`ARCHITECTURE.md`**: new subsection (near "Wake from sleep") documenting the feature: what it does, the
  exact gating conditions, the "USB stays powered in G3/S5" assumption, the pre-paired-controller assumption,
  how to enable it (`OPK_PWR_SWITCH`), and the hardware note that the pin must drive an isolating relay/
  transistor/optocoupler, never short the header directly to the nRF's GPIO. Add `pwr_switch.{h,cpp}` to the
  module-layout table.

- **`CODE_MAP.md`**: add `pwr_switch.cpp/h` to the file map and to the "loop task" execution-context list
  (it's a plain `*Task()`/`*Init()` pair called from `loop()`/`setup()`, same context as `ledTask`/`hapticTask`).
  Note explicitly that it introduces no new cross-task shared state (all its statics are loop-task-only), so no
  new row is needed in the cross-task shared-state table.

## Verification

Hardware-in-the-loop testing (real relay wiring + a motherboard) is outside what I can do — flag that
explicitly rather than claiming it's tested. What I *can* verify:

1. `make build` (default `OPK_PWR_SWITCH=0`) — must still succeed unchanged, confirming the feature is fully
   inert for anyone who doesn't opt in.
2. `make build EXTRA_FLAGS="-DOPK_PWR_SWITCH=1"` — compile-check with the feature compiled in.
3. `make format && make check` — the repo's required pre-push formatting/trailing-comment gate (`AGENTS.md`).
4. Manual review of the gating logic against the stated requirement (host-off debounce, per-slot link-up guard,
   short-press window, cooldown) — re-read against `rf_link.cpp`'s existing short-press detector to confirm no
   drift in the 1000ms/edge-detection semantics.

For you, once flashed on real hardware: confirm (a) Steam short press while PC is off pulses the pin, (b) the
same gesture while the PC is on/USB-mounted does nothing, (c) the existing suspend-wakeup short-press behavior
in `rf_link.cpp` is unaffected, (d) rapid repeated presses respect the cooldown, (e) a long (>1s) Steam hold
does not fire.

</details>

## What actually landed

Matches the plan as written. Files touched:
- **New**: `OpenPuck/pwr_switch.h`, `OpenPuck/pwr_switch.cpp`.
- **Edited**: `OpenPuck/config.h` (`OPK_PWR_SWITCH` flag), `OpenPuck/OpenPuck.ino` (include + 3 guarded call
  sites: `setup()` once, `loop()` twice for the `OPK_LOG`/non-`OPK_LOG` branches), `ARCHITECTURE.md` (new "Host
  power-switch trigger" section + module-table row), `CODE_MAP.md` (new `pwr_switch.cpp/h` entry in §13 + two
  loop-task call-list mentions).

## Verification status (as of landing)

- `make format` / `make check` (format-check + trailing-comment lint, the actual CI gate per `AGENTS.md`): ran
  clean, no findings.
- `make build` (compile-check, either with or without `-DOPK_PWR_SWITCH=1`): **not run** -- `arduino-cli` isn't
  installed in the environment this was built in. Run it yourself before relying on this compiling.
- clang-format version note: `AGENTS.md` pins clang-format **18** ("other versions reformat differently and CI
  will reject them"); the environment this was built in only had **22.1.8** available. Running `make format`
  with v22 did not change any pre-existing file in the repo (only the new/edited ones), which is reassuring but
  not a guarantee of byte-identical output to v18 -- if CI flags a format diff, that's why.
- Hardware-in-the-loop (actual relay + motherboard header + paired controller): not done, not possible from
  here. See the "For you" checklist at the end of the plan above.
