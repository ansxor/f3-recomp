# Unit checks

This page explains the tests that need no ROM or MAME.
It covers the split C++ runtime tests, and the Python tests.
You will learn what each test checks and how to add a check.

These tests check isolated behavior. Run the relevant tests before the whole-game checks.

| Test set | Language | Command | Needs ROM |
| --- | --- | --- | --- |
| `f3rt-test-<area>` (CTest name `runtime-<area>`) | C++ | `ctest --test-dir build -R runtime-` | No |
| `tools/test_*.py` | Python | `uv run python -m unittest discover` | No |
| Differential harness | Python and C | See [Differential testing](/developer/testing/differential) | No |

## C++ runtime tests

The C++ checks are split by area under `runtime/tests/`. CMake builds the `f3rt-test-support` static library and one executable per area (`f3rt-test-<area>`), discovering GoogleTest and RapidCheck test cases under `runtime-<area>` with CTest when `BUILD_TESTING` is on.

```sh
cmake -S . -B build -G Ninja -DF3RT_SDL=OFF -DF3RT_GPU=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build --target f3rt-test-video f3rt-test-input f3rt-test-eeprom f3rt-test-cpu f3rt-test-sprites f3rt-test-audio f3rt-test-motion_interp f3rt-test-state
ctest --test-dir build -R '^runtime-' --output-on-failure
```

Each area test runs its test suite using GoogleTest and RapidCheck property checks. Shared fixture helpers are in `runtime/tests/support.hpp` and `support.cpp`. The fixture uses zero-filled ROM data, an initial stack at `0x41fff0`, a small loop at `0x100`, and one sample word.

| Area | Source | Coverage |
| --- | --- | --- |
| video | [`runtime/tests/video.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/video.cpp) | FDP geometry and game-video decoding checks. |
| input | [`runtime/tests/input.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/input.cpp) | Local and dial inputs, scripts, coin edges and active-low start. |
| eeprom | [`runtime/tests/eeprom.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/eeprom.cpp) | EEPROM protocol and factory image. |
| cpu | [`runtime/tests/cpu.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/cpu.cpp) | CPU dispatch, memory, timing, exceptions, IRQ and watchdog. |
| sprites | [`runtime/tests/sprites.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/sprites.cpp) | Sprite unit sandbox. |
| audio | [`runtime/tests/audio.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/audio.cpp) | Sound ordering, audio timing, DUART, DSP, mixer and reset behavior. |
| motion_interp | [`runtime/tests/motion_interp.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/motion_interp.cpp) | Sprite presentation decoding, motion interpolation pairing, rigid object transforms, independent scroll axes, boundary thresholds and temporal history. |
| state | [`runtime/tests/state.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/state.cpp) | State serialization and validation guards for sound CPU contexts, audio schedulers, and ES5505 clock rates. |
| frontend | [`runtime/tests/frontend.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/frontend.cpp) | Frontend settings round-trip, validation, loader robustness, input mapper events, remapping and capture (requires `F3RT_SDL=ON`). |

Machine-backed checks use synthetic data and do not need game ROMs; standalone component checks use small in-memory inputs.

### What the tests cover

CTest discovers individual GoogleTest cases with names prefixed `runtime-<area>.`; the names below are GoogleTest suite and test names. Cases declared with `RC_GTEST_PROP` are marked property tests; RapidCheck generates and checks multiple inputs for each run.

| GoogleTest test | What it proves |
| --- | --- |
| `Video.RomPlaneLoading4BppAndRayForceVisibleScanline` | Loads 4-bpp graphics without fabricated high planes and honors RayForce's first visible scanline. |
| `Video.SpriteLagAndDelayedPositionSnapshot` | Sprite lag delays captured positions correctly, and save/load preserves the delayed sprite list. |
| `Video.RidingFightGeometryAndFdaPaletteBlur` | Checks Riding Fight geometry, latched palette mode, 15-bit color conversion and FDA forward blur. |
| `Video.SecondSpriteBankHighPlanes` | Second sprite bank uses its own high planes rather than wrapping to bank zero. |
| `Video.FdpGeometryAndNonPowerOfTwoAssets` | Checks independent/non-power-of-two asset wrapping, FDP maps, crops, alternate maps and extended layouts. |
| `Video.GameTileObservation` | Tile attributes decode palette base, pen mask and blend; flips mirror sampled texels; the palette low bit masks the extra plane. |
| `Video.GameTileRowSampling` | Wrapping, repeats, reverse X jumps and global screen flip sample the correct raw cell across four layers. |
| `Video.GameSpriteVramBlockChaining` | Checks chained game-sprite VRAM descriptors. |
| `Video.GameLineVramCarryForward` | Checks line VRAM state carry-forward. |
| `Video.GameTextVramDecode` | Checks decoding of game text-map VRAM. |
| `Video.GameTextFlipMirrorsTexels` (property test) | For generated flip combinations, text X/Y flips mirror sampled texel coordinates. |
| `Video.Color15BitUnpackingInvariant` (property test) | Generated 15-bit color words unpack to the expected ARGB channels. |
| `Input.LocalControlsActiveLowMapping` | Local control mapping uses active-low bits for the tested game layouts. |
| `Input.DialNibblePackingAndDirectionCancellation` | Dial values pack into nibbles and opposing directions cancel. |
| `Input.CoinCounterRisingEdge` | Coin counting responds to rising edges rather than repeated asserted writes. |
| `Input.StartButtonActiveLow` | Start input is active-low. |
| `InputScript.RangeAndMashTiming` | Script ranges, coin pulses, directional input and mash timing produce the expected words. |
| `InputScript.MalformedLinesRejected` | Malformed script lines and out-of-range pokes are rejected. |
| `InputScript.MemoryPokes` | Scripted memory pokes take effect only in their specified frame ranges. |
| `Input.LocalInputActiveLowBitPacking` (property test) | Generated local input words preserve active-low port-line behavior. |
| `Eeprom.FactoryDefaultsAndSerialWrap` | Factory EEPROM defaults load, and serial reads wrap. |
| `Eeprom.UserImageOverrideAndPersistence` | User image overrides and persisted EEPROM data are retained. |
| `Eeprom.WriteDisabledAtPowerOn` | EEPROM writes are disabled after power-on. |
| `Eeprom.WriteTimingAndBusyStatus` | Write busy status and timing match the protocol. |
| `Eeprom.SequentialReadWrap` | Sequential serial reads wrap at the device boundary. |
| `Eeprom.EwdsWriteProtection` | EWDS disables writes. |
| `Eeprom.SingleWordEraseTiming` | Single-word erase uses the expected busy time. |
| `Eeprom.EraseAllTiming` | Erase-all uses the expected busy time. |
| `Eeprom.WriteAllTiming` | Write-all uses the expected busy time. |
| `Eeprom.PowerResetPreservesContents` | Power reset preserves EEPROM contents. |
| `Eeprom.SerialWriteReadRoundTrip` (property test) | Generated addresses and values written under EWEN can be read back. |
| `Eeprom.WriteDisabledRejectsWrites` (property test) | EWDS prevents writes for generated addresses and values. |
| `Eeprom.StateSaveLoadRoundTrip` (property test) | Generated EEPROM word arrays survive state save/load. |
| `Eeprom.FileSaveLoadRoundTrip` (property test) | Generated EEPROM word arrays survive file save/load. |
| `Cpu.NativeLookup` | Native block lookup finds the registered instruction address. |
| `Cpu.WideBusBoundaries` | Wide accesses at RAM boundaries match their byte-lane behavior. |
| `Cpu.WorkRamMirrorRoundTrip` (property test) | Generated 32-bit writes are readable from the work-RAM mirror offset. |
| `Cpu.WideBusReadsMatchByteLanes` (property test) | Generated wide reads match concatenated byte-lane reads. |
| `CpuTest.MainRomBindingAndColdReset` | Validates main-ROM binding and cold-reset initialization. |
| `CpuTest.VblankSchedule` | Checks the first vblank epoch and raster deadlines on the frame grid, including IRQ2 and IRQ3 timing. |
| `CpuTest.Movem` | Checks EC020 MOVEM widths, sign extension, pre-decrement and stored/loaded cycle costs. |
| `CpuTest.RotateCycles` | Register shifts/rotates have no count surcharge; immediate counts 1 and 8 have equal timing. |
| `CpuTest.TrapCycles` | All 16 `TRAP #n` vectors take 24 cycles and stack a format-0 frame. |
| `CpuTest.WorkRamAndRomMemory` | Checks mirrored/wrapping work RAM, read-only ROM, shared-RAM byte lanes and stopped-voice readback. |
| `CpuTest.SrMaskAndWatchdogArming` | Checks stack switching on SR changes, deadline-cache invalidation when the interrupt mask drops, and watchdog arming. |
| `CpuTest.IrqRedirectAndFrame` | Checks IRQ redirection and 68020 exception-frame construction. |
| `CpuTest.ExceptionFramesAndRte` | Checks exception frames and RTE behavior. |
| `CpuTest.NativeDispatchFallbackAndExclusions` | Checks native dispatch, one-instruction fallback and duplicate-block rejection. |
| `CpuTest.WatchdogWholeBoardReset` | Watchdog expiry cannot be skipped by STOP; whole-board reset preserves sound work RAM, reloads boot vectors and clears DSP registers. |
| `SpriteUnits.BehaviourNameHyphenation` | Behaviour names replace underscores with hyphens. |
| `SpriteUnits.CompletedReplayLeavesMachineUntouched` | A completed replay leaves the machine unchanged. |
| `SpriteUnits.DeviceAccessAbortsReplay` | Device access aborts replay. |
| `SpriteUnits.UnpatchedReplayMatchesRealSpan` | Unpatched replay matches execution of the real span. |
| `SpriteUnits.BehaviourSplicesReplacementEntries` | A behaviour splices replacement entries into the sprite-unit replay. |
| `SpriteUnits.ReplayPipelineSplicesPatchedCounter` (property test) | Generated counter values are spliced into replacement entries while real RAM remains unmodified by the behaviour. |
| `Audio.SoundResetReleaseOrdering` | Sound reset release does not execute sound code in the past. |
| `Audio.MailboxWidthWrites` | Mailbox writes at widths 1, 2 and 4 run sound execution in the right order. |
| `Audio.MailboxWidthReads` | Mailbox reads at widths 1, 2 and 4 run sound execution in the right order. |
| `Audio.ResetInstructionPreservesPrecedingSoundExecution` | Reset instructions preserve sound execution that precedes them. |
| `Audio.SoundIrqPartitioningIndependence` | Sound IRQs and CPU state match across main-block partitions of 1, 7, 64, 511 and 4096 ticks. |
| `Audio.DuartCounterRestartAndExpiry` | Checks DUART counter restart and expiry. |
| `Audio.DuartTimerModeAndClockSource` | Checks DUART timer mode and clock source. |
| `Audio.DuartResetRetainsExpiration` | DUART reset retains the expiration state. |
| `Audio.DuartTxFramingAndInterrupts` | Checks transmit framing, holding-register overflow and ready interrupts on both channels. |
| `Audio.DuartCounterDerivedTxClock` | Checks transmit clock derived from the DUART counter. |
| `Audio.DspEndAndSafetyBudgetOrdering` | Checks DSP end-marker and safety-budget ordering. |
| `Audio.DspDelayAddressing` | Checks DSP delay addressing. |
| `Audio.AudioMixerBoardGainAndMute` | Checks board gain, signed PCM scaling, channel mute, gain stages, and state retained across CPU-line or board reset. |
| `Audio.AudioSampleRateNoClockDrift` | Ten seconds of audio produce exactly `sample_rate * 10` samples. |
| `AudioMachineTest.SoundInstructionCycleCharges` | Checks sound-CPU cycle costs for arithmetic, TAS, bit operations, DIVU.W and MULS.W instructions. |
| `AudioMachineTest.SoundVectoredIrqWake` | Checks sound-CPU wake-up on vectored IRQs. |
| `AudioMachineTest.SoundStopUnmaskBudget` | Checks STOP wake-up timing when an IRQ becomes unmasked for budgets from 1 to 52 cycles. |
| `AudioMachineTest.DpramOtisAndSoundReset` | Checks DPRAM byte lanes, OTIS stopped-voice readback and sound reset behavior. |
| `AudioMachineTest.CpuLineResetPreservesDspAndDuart` | CPU-line reset preserves DSP and DUART state. |
| `Audio.DspDelayAddressingModuloInvariant` (property test) | Generated delay addresses preserve the expected modulo addressing invariant. |
| `Frontend.Player1DefaultsAndLocalProfileIndependence` | Player 1 control indices and extra buttons default to expected lines, and other local profiles default to independent controls. |
| `Frontend.FourPlayerStartAndCoinDefaults` | Gamepad ordinals match local players, and start/coin pairs target only their corresponding player. |
| `Frontend.MenuClosureSuppressesHeldInputUntilPhysicalRelease` | Held inputs remain suppressed after menu closure and re-arm only on physical release. |
| `Frontend.ExtraButtonSuppressionReleaseAndFocusLoss` | Suppressed extra buttons stay blocked until release, re-arm independently, and window focus loss releases held extra buttons. |
| `Frontend.CaptureCancellationAndRemapIndependence` | Cancelling capture preserves gameplay keys, while completed bindings remap independently without input leakage. |
| `Frontend.ExtraButtonCaptureAndCancelForAdditionalPlayers` | Capture/cancel for players 3 and 4 preserves player and appended control indices with independent remaps. |
| `Frontend.CaptureEscapeAndBoundsValidation` | Escape cancels capture without leaking input, and captures outside player/control bounds are rejected. |
| `Frontend.LegacySettingsCompatibility` | Loads legacy two-player 11-control configurations, preserving mapped controls and default start/coin/extra buttons. |
| `Frontend.SettingsRoundTripAndReloadedInput` | Persisting and reloading four-player custom bindings preserves device slots, keyboard, button, and signed-axis bindings. |
| `Frontend.SettingsValidationRejectsCorruptInput` | Malformed device ordinals or binding indices are rejected while leaving existing valid settings unmodified. |
| `Frontend.SaveValidationRejectsReservedKeys` | Settings saving validates bindings and rejects reserved keys such as F12. |
| `FrontendSettings.SaveLoadRoundTrip` (property test) | Generated valid frontend settings survive save and load round-trips exactly. |
| `FrontendSettings.LoaderRobustnessOnArbitraryConfigText` (property test) | Arbitrary or mutated config text never crashes the parser and leaves existing settings intact on failure. |
| `FrontendInputMapper.ArbitraryKeySequencesTrackActiveControls` (property test) | Arbitrary key press and release sequences accurately track active control words, and releasing all keys returns to neutral. |
| `FrontendInputMapper.ReleaseBlocksHeldKeysUntilPhysicalRelease` (property test) | Menu closure and focus loss block held keys until physical release, re-arming only upon subsequent press. |
| `MotionDecode.BaselineAndEmptyPresentation` | Validates baseline sprite list decode and identity-neutral empty presentation behavior. |
| `MotionDecode.IdentityPropagation` | Propagates sprite identities to untouched presentation entries. |
| `MotionDecode.SplicedReplacementAndChaining` | Verifies chained zoomed splices, jump word ignoring, native RAM equivalence, stale splice rejection, and empty replacements. |
| `MotionInterp.SpriteCountShiftAndRemoval` | Verifies sprite count shifts and removals retain stable motion pairing. |
| `MotionInterp.AmbiguousDuplicatesSnap` | Equidistant duplicate sprite candidates snap rather than choosing arbitrary matches. |
| `MotionInterp.DenseUnambiguousDuplicateMovement` | Dense identically textured sprite groups pair with their uniquely nearest motion. |
| `MotionInterp.TransformAndJumpRejections` | Zoom, flip, tile change, and large jumps (>32px) reject motion interpolation. |
| `MotionInterp.StaticPairNoMovementOrRejections` | Identical frames produce zero motion and zero rejection statistics. |
| `MotionInterp.RigidAgreementAndPoseSwap` | Rigid agreement with an object interpolates tile changes, while pose swaps and out-of-tolerance changes snap. |
| `MotionInterp.ReindexedGridAndAppearanceTwin` | Re-indexed grids stay static and demoted pairs prefer static appearance twins. |
| `MotionInterp.IdentityMatchedMotionAndZoomRejection` | Identity-matched motion interpolates while transform changes reject. |
| `MotionInterp.RekeyedIdentityAndAppearanceFallback` | Appearance fallback pairs re-keyed identities within the displacement window, snapping equidistant ambiguities. |
| `MotionInterp.MixedIdentityVsNoIdentity` | Mixed identity and identity-less pairs reject, while identical geometry with new identity does not produce spurious motion. |
| `MotionInterp.PlayfieldScrollAndIndependentGuards` | Unrelated playfield controls allow valid scroll, while Y jumps, zooms, wraps, clips, and pen masks snap independently. |
| `MotionInterp.TextScrollAndIndependentWrap` | Fractional text scroll midpoints interpolate, while text clips and wraps snap independently. |
| `MotionInterp.HistoryLifecycleAndDiscontinuity` | Invalid alpha, duplicates, history gaps, fallback recovery, and resets correctly control temporal pairing lifecycle. |
| `MotionInterp.EndpointIdentity` (property test) | For arbitrary valid displacements, alpha=0.0 and alpha=1.0 match start and end coordinates exactly. |
| `MotionInterp.MonotonicInterpolationBounds` (property test) | For any alpha in [0, 1], interpolated positions remain within endpoint bounds. |
| `MotionInterp.JumpThresholdRejection` (property test) | Displacements strictly exceeding 32px are rejected as jumps. |
| `StateValidationTest.MusashiSoundContextUnsafeFieldsRejected` | Rejects malformed sound supervisor/master stack indices, opcode tables, interrupt levels, CPU models, and timing tables. |
| `StateValidationTest.AudioSchedulerSnapshotsRejected` | Rejects sample deadlines outside fractional range, unbounded generation, and arithmetic underflow/overflow. |
| `StateValidationTest.ES5505ClocksRejected` | Rejects zero OTIS rates and crystal/divider disagreements. |
| `State.AudioCoreRoundTrip` (property test) | Valid CanonicalAudioCore parameters survive state serialization round-trip. |
| `State.AudioCoreInvalidSampleAccumRejected` (property test) | Generated sample_accum values outside fractional range are rejected. |
| `State.ES5505RoundTrip` (property test) | Valid CanonicalES5505 clock and voice parameters survive state serialization round-trip. |
| `State.ES5505SampleRateDisagreementRejected` (property test) | Disagreeing ES5505 sample rates and master clocks are rejected. |

Some CPU checks compare interpreter and ABI execution to ensure the native and reference paths agree on timing.

`docs/developer/DECISIONS.md` records prior fixes protected by these tests, including pending-IRQ-at-STOP and DC-voice sound behavior.

### Running an individual test

Use GoogleTest's `--gtest_filter` with the executable, or select its discovered CTest name:

```sh
build/f3rt-test-cpu --gtest_filter='CpuTest.Movem'
ctest --test-dir build -R 'runtime-cpu.CpuTest.Movem' --output-on-failure
```

RapidCheck prints a seed for reproducibility when a property fails. Set it through `RC_PARAMS` to rerun with that seed (replace `<n>` with the printed value):

```sh
RC_PARAMS="seed=<n>" build/f3rt-test-cpu --gtest_filter='Cpu.WorkRamMirrorRoundTrip'
```

The equivalent discovered CTest can also be run with `RC_PARAMS="seed=<n>" ctest --test-dir build -R 'runtime-cpu.Cpu.WorkRamMirrorRoundTrip' --output-on-failure`.

The RapidCheck documentation also prints a `reproduce=...` value after a failed property; using that value with `RC_PARAMS="reproduce=<value>"` replays the exact minimized failure.

### How to add a check

Add a GoogleTest `TEST` or `TEST_F` for an example-based check, or `RC_GTEST_PROP` for a property-based check, in the relevant file under `runtime/tests/`. Use the shared fixture in `runtime/tests/support.hpp` when a machine is needed. Keep assertions focused on the behavior the test protects.

For sound code, write it with `m->audio->write16()` into sound RAM, as the `AudioMachineTest` cases do.


The runtime C++ tests also run in GitHub Actions for both a game with game-video sources and one using the generic video path. The workflow builds the eight `f3rt-test-*` targets and runs the `runtime-` CTest cases; it requires no ROMs.

### What the runtime tests does not prove

It tests single rules with small inputs. It does not run the game. It does not replace the gameplay gates. See [Machine, memory and scheduling](/developer/runtime/machine).


## Python unit tests

Three files in `tools/` hold the Python tests. They use `unittest`. They need dependencies managed by `uv` (`capstone==5.0.9`) and, for one file, a C compiler. They use synthetic data only.

```sh
uv run python -m unittest discover -s tools -p 'test_*.py'
```

Run the command from the repository root. A run of this command on the documented source ran 18 tests and passed.

### `test_discovery.py` (11 tests)

The tests build a small synthetic ROM (0x1000 bytes with the vectors SSP `0x410000` and PC `0x400`) and call `recomp.discovery.discover()`. They check which instruction addresses the discovery finds. See [Discovery](/developer/recompiler/discovery).

`ActorDiscoveryTests` check the recursive mode with actor scripts:

- A long callback address and cyclic script calls are followed, and script data is not decoded as code.
- Register-staged script records (`MOVE.L table(PC,Xn),An` followed by a store) give the callbacks, and the pointer-table strides are respected.
- A pointer table with an explicit `count` does not run into the data after it.
- Jump tables with backward destinations, staged through a register or `LEA`, are found.
- A signed full-format extension displacement locates a table correctly.

`AllAlignedDiscoveryTests` check the `all_aligned` coverage mode that the Japan set uses:

- A computed target with no pointer literal in the ROM is still found.
- A 24-bit pointer at an odd byte offset is found.
- A routine of more than 32 straight-line instructions is found without a filter.
- Instruction starts inside the extension words of another instruction are kept (overlapping starts).
- Odd PCs are never decoded. A truncated instruction at the end of the ROM is rejected and recorded in `invalid_pcs`. Odd entry points raise `ValueError`.
- The `recursive` mode still ignores code that nothing reaches.

### `test_generate.py` (2 tests)

These tests call `recomp.generate.generate()` on a tiny program. They write a driver in C, compile it with the C compiler (`CC` or `cc`, with `-std=c11 -O2 -Wall -Wextra -Werror`) and run it. They check the deadline behavior of the generated blocks:

- A block yields at the first instruction boundary that reaches `dispatch_deadline`, with flags written to SR (`cc_op == 0`) and the PC and cycles correct. It resumes inside the block and finishes with the right registers and flags.
- Overlapping entries skip extension words. A deadline stop and a restart inside the overlap give the right result.

See [Flags, timing and deadlines](/developer/recompiler/flags-and-timing).

### `test_decode_sound.py` (5 tests)

These tests check `decode_sound.py` with synthetic records. See [Sound traces and sound tools](/developer/testing/sound-tools).

## Where the other tests are

| Test | Page |
| --- | --- |
| Instruction-level differential harness | [Differential testing](/developer/testing/differential) |
| Seeded gameplay regression | [Seeded gameplay regression](/developer/testing/gameplay-regression) |

## Limits

- GitHub Actions runs the C++ runtime tests automatically; run Python tests locally before you commit.

