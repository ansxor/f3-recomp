# SDL3 frontend

`runtime/frontend/frontend.cpp` is the host shell for `landmakr` and `f3rt-run`. It owns CLI parsing, preferences, SDL events/output, pacing and captures. Emulation stays in `Machine`.

## Threads

A windowed run uses two threads. A headless run runs the same emulation loop inline on the main thread, with no emulation thread, no snapshots and no pacing, so its output stays bit-identical.

- **Video thread (main).** SDL events, `FrontendUi`, input sampling, GPU/SDL present, screenshots, automatic scale and motion interpolation (`capture_motion` per new snapshot, `present_motion` per display refresh). SDL requires window events and swapchain acquire/present on this thread. With motion interpolation it is paced by the display tick (`GpuVideo::pace_motion`); otherwise it presents when a new snapshot arrives. It never touches `Machine`.
- **Emulation thread.** Applies inputs, runs `Machine::run_frame`, entry watch, dumps and profiling ticks, drains PCM into the SDL audio stream (and WAV/statistics) and paces frames against a rolling `steady_clock` deadline. A stall of more than 50 ms resyncs the deadline instead of bursting. `--unthrottled` runs it flat out. Fast boot runs before the thread starts.

The SDL audio stream plays on SDL's own thread, and the Enhanced audio backend keeps its own worker. The frontend adds no audio thread.

### Hand-off

`Link` (in `frontend.cpp`) carries everything across the thread boundary under one mutex.

- **Snapshots.** After each frame the emulation thread publishes an immutable snapshot into a three-slot, latest-wins buffer: the `CapturedFrame` for the GPU path (the three capture buffers are handed to `GameVideo::set_capture_target`, so nothing is copied or allocated per frame) or the presentation pixels for the CPU/FDP path, plus the frame number, the scheduled frame start time, the GameVideo GPU scale and a discontinuity epoch. A slow presenter drops stale snapshots; a fast one repeats or interpolates. The epoch changes on state load, pause/resume and clock resync, and tells the presenter to reset motion history. The GPU only reads the snapshot and its own copy of the tile ROM, which is immutable after load.
- **Commands.** The video thread posts pause/resume (the menu pauses emulation, except during an active netplay match), save/load slot (refused during a match), GPU scale changes and the netplay actions (host, join, cancel, play locally, chat line). The emulation thread applies them between frames, answers with a message that appears in the menu (errors included), and bumps the epoch and clears the SDL audio stream on load. The presenter applies a new GPU scale only when a snapshot captured at that scale arrives. Anything that redraws the capture outside a normal frame (state load, netplay state sync, leaving the lobby) first redirects the capture to a buffer the presenter does not hold and republishes the result.
- **Netplay.** The lobby, `Session`, recorder, versus trigger and lockstep stepping all run on the emulation thread (the session callbacks run the machine). It publishes a `NetStatus` (lobby open, active, status/error text, room, peer, ping, stalls, desync bundle path, received chat) that the video thread copies into the UI. The video thread keeps the UI-only parts: lobby window visibility follows the status, the chat box, the overlay, and the frame-keyed test options (`--screenshot-frame`, `--open-f1-frame`, `--chat-open-frame`, `--chat-close-frame`), which fire on the first presented snapshot at or past their frame; `--chat-send-frame` runs on the emulation thread at the exact frame. Local input for a match frame is latched on the emulation thread from the published words, and withheld while the chat box has focus.
- **Input.** The video thread publishes the current input words; the emulation thread latches them once per emulated frame. It never waits for the video thread and reuses the last published words if none are fresh. The emulation thread also publishes its next frame deadline, and the video thread's idle wait wakes about 2 ms before it to pump events and publish input, so the latched words are ~2-4 ms old instead of up to one 8 ms idle wait. The margin covers timer wake lateness on both threads. With display-paced motion interpolation the video thread is paced by the display tick instead, and input age is up to one display period. On macOS the emulation thread runs at user-interactive QoS; at default QoS its frame-deadline sleep overshoots by 1-4 ms.
- **Exit.** The video thread requests stop and joins. An exception on the emulation thread (for example a CPU halt) is captured and rethrown on the main thread after the join. With `--frames N --surface`, the presenter draws and saves the snapshot of frame N after the emulation thread finishes, then exits.

## Startup and preferences

Configure ROMs, native blocks, sound and video before the machine starts running. Preferences from `settings.cfg` under `SDL_GetPrefPath("f3-recomp", "f3rt")` load before explicit CLI overrides; `--config FILE` changes that path. `states/` and `screenshots/` are sibling directories. Preferences are saved only through the explicit menu action.

The [CLI reference](/reference/cli) lists option values/defaults. Strict `landmakr` defaults to generated main execution, native sound and GameVideo. The Enhanced host audio backend is the default; Reference runs the cycle-accurate sound devices. Do not conflate the diagnostic sound-CPU driver with the host audio backend.

`--audio-backend enhanced|reference` selects the backend. The frontend has no sound-driver option: with Reference audio it runs the native sound driver when the build generated one (`F3RT_SOUND_GENERATED`) and the oracle otherwise. An explicit CLI backend overrides the saved backend preference.

GPU/auto/interpolation options retain their own game/compare and GPU gates.

## Events and ImGui ownership

`FrontendUi` uses Dear ImGui with SDL renderer/GPU backends. `InputMapper` produces two offline active-high 11-bit profiles from keyboard/gamepad state; `apply_inputs` maps them to active-low board ports. There are no default gamepad bindings. Default keys and remapping instructions are in [Controls and options](/guide/running).

F1 opens/closes the menu. The simulation pauses while it is open. Escape closes an open menu, otherwise quits. F3 is service; F2 is test. F12 captures PNG. F11/Alt+Enter toggles fullscreen. Key repeat does not create extra input edges. Focus loss or capture closure clears input and suppresses held controls until release, preventing a captured button leaking into gameplay.

## Presentation and output

The window starts at `(320 + 2 * border) * 3` by 696 and preserves aspect on resize. Native cadence is `6671500 / (432 * 262)` Hz, kept by the emulation thread. Headless local runs require a finite frame count and run without a window/device; no-audio does not disable simulation audio.

GPU scale, filtering, interpolation (mode and fields) and motion interpolation apply live between frames; CPU scale, renderer/backend, video model and border are restart preferences. Shaders Off/CRT/User are GPU-only live presentation effects. User reload failure retains the last valid shader. Effects transform GPU captures, not native pixels/checksums or the overlay. See [shader ABI/examples](/guide/video#f1-shaders-and-live-controls).

Slots 0–9 use full machine state and require compatible build, ROMs and geometry.

Reference output drains the generated PCM, including while muted. Volume affects host output only. Enhanced is not PCM-equivalent to Reference/native mixing. [HLE design](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/HLE-AUDIO.md) documents that boundary.

## Exit

At normal exit the frontend saves an explicitly selected EEPROM file, writes requested diagnostics/captures and prints machine/video summaries. Captures/dumps can contain ROM-derived data and belong in ignored output directories.
