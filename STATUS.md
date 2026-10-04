DONE

# Recompiler acceptance

The static recompiler, native main-CPU execution, SDL playability, and sampled MAME frame-parity milestones are complete. **Integrated audio waveform parity remains open with the runtime owner; this is not an overall audio-parity sign-off.**

Target: **Land Maker Japan 2.01J (`landmakrj`)**, as approved after identifying the supplied ROMs. World `landmakr` remains configuration-only because its program lanes were not supplied.

## Implemented

- Verified four-lane ROM loading and Capstone 68020 decoding; vector, branch, jump-table, actor-bytecode, and register-staged callback discovery.
- Native C emission with lazy flags, guest-stack calls, dispatch entries for block interiors, and strict rejection of unresolved execution in `landmakr`. Diagnostic interpreter fallback remains explicit opt-in.
- ABI v2 event deadlines: multi-instruction blocks yield at instruction boundaries for scheduled events and IRQ-mask changes. Generated sources reject incompatible runtime headers.
- Source- and trace-backed MOVEM, rotate, and software-TRAP timing corrections, without frame offsets, RNG overrides, or fitted delays.
- Native sound-mailbox/reset bus synchronization: device time catches up before access, without changing CPU cycle costs, ABI layout, lazy flags, or boundary-only main IRQ entry.
- Sound-68000 timing corrections: positive-source MULS includes its terminal Booth transition; STOP exposing a pending IRQ retains entry debt and the reference's four-clock polling iteration. These do not change the main EC020 compiler or introduce a sound-scheduler patch.

## Observed verification

| Check | Result |
| --- | --- |
| Discovery | 43,986 instructions; 163,286 bytes / 2 MiB (7.786%); 2,040 function candidates; 94 unresolved static transfers |
| Lowering | All 43,986 discovered instructions lowered natively; zero lowering fallbacks |
| Instruction differential | 5,000 passed, zero failed, zero unsupported; independent corrected Musashi EC020 reference, including cycles and exception state |
| Discovery/deadline regressions | Six passed; synthetic generated C executes deadline equality/overshoot, canonical flags, and interior-PC resumption |
| Strict native cold boot | 3,600 frames; 52,342,915 native blocks; zero fallback instructions |
| Cold-boot audio output | 1,817,655 PCM frames, peak 1289 after both sound-68000 MULS and pending-IRQ STOP corrections |
| Direct native vs MAME pixels | **25/25 frames RGB-exact**, frames 600–3480 at intervals of 120; zero mismatched pixels out of 1,856,000 |
| Main state | Frame 600 matches all 131,072 main-RAM bytes; sampled later RNG states match |
| Actual SDL gameplay | **8,400 frames**, 120,080,217 native blocks, **zero fallback instructions**; real foreground coin/start/selection/movement/Z/X/C input; visible character selection and scored combat |
| Gameplay audio output | 4,241,196 PCM frames, peak 1303; nonzero output, not a waveform-parity claim |
| SDL smoke at `a7c8606` | 900 frames on Cocoa/Metal, 15,407,878 native blocks, zero fallback; actual SDL framebuffer shows the Japan-only boot notice; this short boot segment is silent |
| Integrated native audio, seconds 20–54, MULS + STOP (`5fffd0e`) | L/R correlation **0.981626343/0.981147700**, RMS error **38.598326/38.716739 PCM LSB**, fixed analysis lag **−1 sample (−0.033601 ms)**; **not parity** |
| Preceding native audio, seconds 20–54, MULS only (`14957fe`) | L/R correlation **0.990095102/0.988963063**, RMS error **28.338496/29.623994 PCM LSB**, lag **−1 sample**; better aggregate correlation than the combined native run, still not parity |
| Main-to-sound event comparison at `2762bfe` | All 1,316 native/interpreter writes match PC/address/width/value/CPU tick; effective device-delivery mismatches reduced from 64 to zero |
| Sound-bus temporal regression at `2762bfe` | Real sound-CPU fixture fails against the old runtime on retroactive reset release; passes with the ABI synchronization fix |

Latest cold-boot/frame/audio verification: `5fffd0e` (runtime `91f24eb`), following `14957fe` (runtime `01911a6`), with native ABI bus synchronization `2762bfe` retained. Separate fresh strict-native captures verify both MULS-only and combined MULS/STOP execution; both preserve frame/RAM parity and zero fallback. The latest SDL window smoke remains from `a7c8606`; full SDL gameplay/input verification remains from `d31b3aa`. Neither SDL scenario was repeated for these sound-core changes. Generated main code, main EC020 opcode costs, ABI layout/version, and audio-scheduler production code remain unchanged.

## Reproduce

From the repository-root `integration` worktree, after the dependency setup in `README.md`:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DF3_ROM_DIR=../roms/landmakr
cmake --build build --target landmakr -j 4
./build/landmakr
```

Controls: 5/6 coin, 1/2 start, arrows, Z/X/C. The executable uses generated main-CPU code by default and fails on untranslated execution; do not add `--allow-fallback` for acceptance.

```sh
PYTHONPATH=build/python python3 -m unittest discover -s tools -p 'test_*.py'
PYTHONPATH=build/python python3 tools/differential/run.py \
  --musashi runtime/third_party/musashi --output build/differential --cases 5000
./build/landmakr --headless --no-audio --frames 3600 \
  --dump-dir build/captures/native-muls-stop --dump-start 600 --dump-every 120 \
  --wav build/native-muls-stop.wav
python3 tools/compare_frames.py \
  wt/runtime/build/captures/mame-aligned build/captures/native-muls-stop --json
cmp wt/runtime/build/captures/mame-aligned/frame_0600/mainram.bin \
  build/captures/native-muls-stop/frame_0600/mainram.bin
PYTHONPATH=wt/runtime/build/python python3 tools/compare_audio.py \
  wt/runtime/build/mame-audio-reset-v2.wav build/native-muls-stop.wav \
  --start 20 --end 54 --json build/native-muls-stop-audio.json
```

The frame comparison uses the existing local format-2 MAME baseline. Baseline regeneration is documented in `tools/mame/README.md`. The SDL smoke at `a7c8606` used `./build/landmakr --frames 900 --surface build/native-duart-tx-window.bmp`; its framebuffer was also converted to PNG for inspection. Earlier full gameplay evidence remains in `build/captures/native-play-deadline`, `build/native-play-deadline-grid.png`, and `build/native-play-deadline.wav`. ROM bytes, generated C, captures, WAVs, and raw traces remain ignored; completed temporary probes were removed.

The current waveform report is root `build/native-muls-stop-audio.json`, using the unchanged reset-aware MAME baseline at 29,761 Hz without resampling. MULS-only native evidence is separately retained in `build/native-muls-audio.json`, `build/native-muls.wav`, and `build/captures/native-muls`. The native rows above must not be replaced by the runtime peer's independent-interpreter metrics: MULS-only correlation 0.990128760/0.989298899, RMS 28.290878/29.169621; combined correlation 0.994958428/0.994527392, RMS 20.216328/20.858453 PCM LSB. The STOP correction improves that interpreter result but worsens the measured native result.

## Main-to-sound timing diagnosis at `2762bfe`

The complete 3,600-frame captures have **no mailbox/reset writes during seconds 20–54**. Surrounding writes occur at main ticks 283674747 (17.7296716875 s) and 916142598 (57.258912375 s). Across the entire run, all 1,316 events already matched PC/address/width/value and absolute CPU tick before the fix. The defect was earlier **effective device delivery**, not lowering-cycle drift:

| Event | PC / write address | CPU tick, both paths | Old native device tick | Interpreter and corrected device tick |
| --- | --- | --- | --- | --- |
| First reset assertion | `002f88` / `c80100` | 31831348 | 31831299 (−49) | 31831348 |
| First reset release | `002fd0` / `c80000` | 31836522 | 31836513 (−9) | 31836522 |

The preceding MOVEM and CLR.B correctly charge 49 and 9 cycles inside their native blocks, but device time previously remained at block entry. In total, 64 writes took effect 2–49 main ticks early. Native ABI callbacks now synchronize devices before sound-mailbox/reset access; all 64 delivery mismatches disappear with no change to event CPU timestamps. This is not deadline quantization, a fitted offset, or an opcode-cost correction.

A diagnostic one-instruction native dispatcher produces the entire interpreter WAV byte-for-byte. A write-synchronization-only control produces the entire corrected native WAV byte-for-byte. Production retains multi-instruction blocks. The residual native/interpreter audio difference is sensitive to sound-slice partitioning; the runtime owner retains that separate investigation.

Ignored captures are under `wt/recomp/build/`: `events-native.csv`, `events-interpreter.csv`, `events-native-fixed.csv`, and the `events-native-sync`/`events-native-one` CSV/WAV controls. Standard native acceptance artifacts are root `build/captures/native-main-sound-sync` and `build/native-main-sound-sync.wav`. Temporary probe source/executables were removed; the real sound-core temporal regression remains in `runtime/check.cpp`.

## Remaining limitations

- **Integrated sound timing/waveforms:** positive-source MULS correction raises native correlation from the ABI-sync run's 0.904081582/0.899085769 to **0.990095102/0.988963063**. Adding pending-IRQ STOP accounting instead lowers native correlation to **0.981626343/0.981147700**, despite improving the peer's interpreter result to 0.994958428/0.994527392. The post-STOP native/interpreter gap is not yet causally explained. The prior block/slice sensitivity is not proof of its cause. Audio parity remains unresolved; no fitted delay, sample masking, waveform shift, output compensation, or production audio-scheduler change was introduced.
- **Retained STOP correction:** the runtime peer's executed MAME microprogram measures marker totals of 20 clocks without STOP/IRQ and 72 with it, isolating 52 clocks (4 instruction + 4 poll + 44 IRQ entry). The imported correction matches that evidence. Omitting the poll misses the marker by four clocks and was rejected despite higher aggregate correlation. This is explicitly reference compatibility, not independent hardware timing proof; the worsened native waveform metric is not a reason to substitute the unsupported timing.
- **Counter reset compatibility:** preserving the queued counter expiration and output phase across board reset follows MAME's executed behavior, despite its contradictory reset comment. The reference warm test begins with ISR `$08` already latched; discarding that event previously added roughly 160,000 main ticks. This is explicitly **MAME compatibility, not a physical DUART RESET claim**. Restart now clears old divider residue, and source/preset writes do not rescale an already scheduled event.
- **Retained IRQ/DIVU correction:** `ca08bf9` (local `59f3362`) remains on qualified primary evidence, not improved audio metrics. Motorola documents nominal no-wait IRQ entry at 44 clocks. Its DIVU maximum of 140 clocks does not establish the exact dynamic formula; its “less than 10%” variation statement conflicts with Cwik's published range. Cwik's detailed 10/76/136-cycle timing account explicitly reports partial assembler testing on real 68000 hardware, not exhaustive physical proof or published raw traces. Source links and provenance are preserved in `NOTES.md`.
- Static discovery counts are not proof that every possible game path has been exercised. The 94 unresolved transfers remain reported; strict runtime execution prevents them from being silently accepted as native coverage.
- Pixel equivalence covers the stated attract samples, not every gameplay scene or physical FDP behavior. Disputed clipping behavior and the ROM evidence actually exercised are recorded in `NOTES.md`.
- World ROM execution remains untested and was not substituted for the approved Japanese target.
