# SDL3 frontend

`runtime/frontend/frontend.cpp` is the host shell for `landmakr` and `f3rt-run`. It owns CLI parsing, preferences, SDL events/output, pacing and captures. Emulation stays in `Machine`.

## Startup and preferences

Configure ROMs, native blocks, sound and video before the machine starts running. Preferences from `settings.cfg` under `SDL_GetPrefPath("f3-recomp", "f3rt")` load before explicit CLI overrides; `--config FILE` changes that path. `states/` and `screenshots/` are sibling directories. Preferences are saved only through the explicit menu action.

The [CLI reference](/reference/cli) lists option values/defaults. Strict `landmakr` defaults to generated main execution, native sound and GameVideo. The Enhanced host audio backend is the default; Reference runs the cycle-accurate sound devices. Do not conflate the diagnostic sound-CPU driver with the host audio backend.

`--audio-backend enhanced|reference` selects the backend. The frontend has no sound-driver option: with Reference audio it runs the native sound driver when the build generated one (`F3RT_SOUND_GENERATED`) and the oracle otherwise. An explicit CLI backend overrides the saved backend preference. Explicit Enhanced with `--sound-trace` remains an error.

GPU/auto/interpolation options retain their own game/compare and GPU gates.

## Events and ImGui ownership

`FrontendUi` uses Dear ImGui with SDL renderer/GPU backends. `InputMapper` produces two offline active-high 11-bit profiles from keyboard/gamepad state; `apply_inputs` maps them to active-low board ports. There are no default gamepad bindings. Default keys and remapping instructions are in [Controls and options](/guide/running).

F1 opens/closes the menu. The simulation pauses while it is open. Escape closes an open menu, otherwise quits. F3 is service; F2 is test. F12 captures PNG. F11/Alt+Enter toggles fullscreen. Key repeat does not create extra input edges. Focus loss or capture closure clears input and suppresses held controls until release, preventing a captured button leaking into gameplay.

## Presentation and output

The window starts at `(320 + 2 * border) * 3` by 696 and preserves aspect on resize. Native cadence is `6671500 / (432 * 262)` Hz. Headless local runs require a finite frame count and run without a window/device; no-audio does not disable simulation audio.

GPU scale, filtering, interpolation (mode and fields) and motion interpolation apply live between frames; CPU scale, renderer/backend, video model and border are restart preferences. Shaders Off/CRT/User are GPU-only live presentation effects. User reload failure retains the last valid shader. Effects transform GPU captures, not native pixels/checksums or the overlay. See [shader ABI/examples](/guide/video#f1-shaders-and-live-controls).

Slots 0–9 use full machine state and require compatible build, ROMs and geometry.

Reference output drains the generated PCM, including while muted. Volume affects host output only. Enhanced is not PCM-equivalent to Reference/native mixing. [HLE design](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/HLE-AUDIO.md) documents that boundary.

## Exit

At normal exit the frontend saves an explicitly selected EEPROM file, writes requested diagnostics/captures and prints machine/video summaries. Captures/dumps can contain ROM-derived data and belong in ignored output directories.
