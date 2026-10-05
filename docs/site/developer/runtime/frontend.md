# SDL3 frontend

`runtime/frontend.cpp` is the host shell for `landmakr` and `f3rt-run`. It owns CLI parsing, preferences, SDL events/output, pacing, captures and session lifetime. Emulation stays in `Machine`; lifecycle stays in `netplay::Session`.

## Startup and preferences

Configure ROMs, native blocks, sound and video before snapshot ownership begins. Preferences from `settings.cfg` under `SDL_GetPrefPath("f3-recomp", "f3rt")` load before explicit CLI overrides; `--config FILE` changes that path. `states/` and `screenshots/` are sibling directories. Preferences are saved only through the explicit menu action.

The [CLI reference](/reference/cli) lists option values/defaults. Strict `landmakr` defaults to generated main execution, native sound and GameVideo. Accurate host audio is default; HLE is an optional backend with separate non-rewound reconciliation. Do not conflate the diagnostic sound-CPU driver with the host audio backend.

`--audio-backend accurate|hle` selects the backend; `--sound-driver native|oracle` selects sound-CPU execution within accurate only. Explicit CLI audio choices override the saved backend preference. Explicit HLE with `--sound-driver` or `--sound-trace` remains an error.

GPU/auto/interpolation options retain their own game/compare and GPU gates. Netplay requires strict-native main execution without fallback or sound tracing, server/room and a Host/Join role for CLI entry. It does not require erased EEPROM or fixed presentation geometry. Identity compares ROM/build/canonical simulation format; the host supplies match state.

## Events and ImGui ownership

`FrontendUi` uses Dear ImGui with SDL renderer/GPU backends. `InputMapper` produces two offline active-high 11-bit profiles from keyboard/gamepad state; `apply_inputs` maps them to active-low board ports. Network input uses local P1 for whichever slot the relay assigned. There are no default gamepad bindings. Default keys and remapping instructions are in [Controls and options](/guide/running).

F1 opens/closes the menu. Solo pauses while open; network simulation continues with local P1 neutral. Escape closes an open menu, otherwise quits. F3 is service; F2 is test. F12 captures PNG. F11/Alt+Enter toggles fullscreen. Key repeat does not create extra input edges. Focus loss or capture closure clears input and suppresses held controls until release, preventing a captured button leaking into gameplay.

## Presentation and output

The window starts at `(320 + 2 * border) * 3` by 696 and preserves aspect on resize. Native cadence is `6671500 / (432 * 262)` Hz. Headless local runs require a finite frame count and run without a window/device; no-audio does not disable simulation audio.

GPU scale/filtering apply live; CPU scale, renderer/backend, video model, border and interpolation are restart preferences. Shaders Off/CRT/User are GPU-only live presentation effects. User reload failure retains the last valid shader. Effects transform GPU captures, not native pixels/checksums or the overlay. See [shader ABI/examples](/guide/video#f1-shaders-and-live-controls).

Offline slots 0–9 use full machine state and require compatible build, ROMs and geometry. They are disabled during netplay. Rollback's local ring also retains full presentation state; canonical sync APIs omit expanded buffers only for handoff and network CRCs.

Accurate output drains generated PCM locally and confirmed-only PCM in rollback, including while stalled or muted. Volume affects host output only. HLE has separate reconciliation and is not PCM-equivalent to accurate/native mixing. [HLE design](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/HLE-AUDIO.md) documents that boundary.

## Session loop and return

Pump `Session` every pass even during stalls. It pairs clients, automatically prepares host versus using ordinary coin/start/confirmation inputs, transfers a canonical snapshot, waits for both-loaded acknowledgement, and constructs match-relative rollback. Sample scheduled inputs once, synchronize before stepping, drain checksums/audio and present only the latest corrected image.

The guest preserves its local full state until handoff succeeds. Natural exit is determined only at a confirmed boundary. Disconnect/exit restores confirmed state before local execution; a pre-barrier guest failure restores its prior local state. Host/Join again creates a fresh snapshot, including in the same room. See [frontend netplay integration](/developer/netplay/frontend-integration).

At normal exit the frontend saves an explicitly selected EEPROM file, writes requested diagnostics/captures and prints machine/video summaries. Captures/dumps can contain ROM-derived data and belong in ignored output directories.

For verified menu/slots/remaps/shaders and lifecycle details, see [IMGUI-NETPLAY.md](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/IMGUI-NETPLAY.md).
