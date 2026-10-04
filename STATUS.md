DONE

# Recompiler acceptance

The static recompiler, native main-CPU execution, SDL playability, and sampled MAME frame-parity milestones are complete. **Integrated audio waveform parity remains open with the runtime owner; this is not an overall audio-parity sign-off.**

Target: **Land Maker Japan 2.01J (`landmakrj`)**, as approved after identifying the supplied ROMs. World `landmakr` remains configuration-only because its program lanes were not supplied.

## Implemented

- Verified four-lane ROM loading and Capstone 68020 decoding; vector, branch, jump-table, actor-bytecode, and register-staged callback discovery.
- Native C emission with lazy flags, guest-stack calls, dispatch entries for block interiors, and strict rejection of unresolved execution in `landmakr`. Diagnostic interpreter fallback remains explicit opt-in.
- ABI v2 event deadlines: multi-instruction blocks yield at instruction boundaries for scheduled events and IRQ-mask changes. Generated sources reject incompatible runtime headers.
- Source- and trace-backed MOVEM, rotate, and software-TRAP timing corrections, without frame offsets, RNG overrides, or fitted delays.

## Observed verification

| Check | Result |
| --- | --- |
| Discovery | 43,986 instructions; 163,286 bytes / 2 MiB (7.786%); 2,040 function candidates; 94 unresolved static transfers |
| Lowering | All 43,986 discovered instructions lowered natively; zero lowering fallbacks |
| Instruction differential | 5,000 passed, zero failed, zero unsupported; independent corrected Musashi EC020 reference, including cycles and exception state |
| Discovery/deadline regressions | Six passed; synthetic generated C executes deadline equality/overshoot, canonical flags, and interior-PC resumption |
| Strict native cold boot | 3,600 frames; 52,342,915 native blocks; zero fallback instructions |
| Cold-boot audio output | 1,817,655 PCM frames, peak 1244 after the event-based DUART counter correction |
| Direct native vs MAME pixels | **25/25 frames RGB-exact**, frames 600–3480 at intervals of 120; zero mismatched pixels out of 1,856,000 |
| Main state | Frame 600 matches all 131,072 main-RAM bytes; sampled later RNG states match |
| Actual SDL gameplay | **8,400 frames**, 120,080,217 native blocks, **zero fallback instructions**; real foreground coin/start/selection/movement/Z/X/C input; visible character selection and scored combat |
| Gameplay audio output | 4,241,196 PCM frames, peak 1303; nonzero output, not a waveform-parity claim |
| SDL smoke at `a7c8606` | 900 frames on Cocoa/Metal, 15,407,878 native blocks, zero fallback; actual SDL framebuffer shows the Japan-only boot notice; this short boot segment is silent |
| Integrated native audio, seconds 20–54 | L/R correlation **0.886013889/0.882232851**, RMS error **96.173675/96.749467 PCM LSB**, fixed analysis lag **−2 samples (−0.067202 ms)**; **not parity** |

Latest cold-boot/frame/audio verification: `2fc5df5`, cherry-picked from runtime peer `c31066c`. The latest SDL window smoke remains from `a7c8606`; full SDL gameplay/input verification remains from `d31b3aa`. Neither SDL scenario was repeated for this counter-only correction. No CPU, compiler, or ABI change was required.

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
  --dump-dir build/captures/native-duart-counter --dump-start 600 --dump-every 120 \
  --wav build/native-duart-counter.wav
python3 tools/compare_frames.py \
  wt/runtime/build/captures/mame-aligned build/captures/native-duart-counter --json
cmp wt/runtime/build/captures/mame-aligned/frame_0600/mainram.bin \
  build/captures/native-duart-counter/frame_0600/mainram.bin
PYTHONPATH=wt/runtime/build/python python3 tools/compare_audio.py \
  wt/runtime/build/mame-audio-reset-v2.wav build/native-duart-counter.wav \
  --start 20 --end 54 --json build/native-duart-counter-audio.json
```

The frame comparison uses the existing local format-2 MAME baseline. Baseline regeneration is documented in `tools/mame/README.md`. The SDL smoke at `a7c8606` used `./build/landmakr --frames 900 --surface build/native-duart-tx-window.bmp`; its framebuffer was also converted to PNG for inspection. Earlier full gameplay evidence remains in `build/captures/native-play-deadline`, `build/native-play-deadline-grid.png`, and `build/native-play-deadline.wav`. ROM bytes, generated C, captures, WAVs, and raw traces remain ignored; completed temporary probes were removed.

The refreshed waveform report is root `build/native-duart-counter-audio.json`, using the unchanged reset-aware MAME baseline at 29,761 Hz without resampling. The separate native-audio row above is measured from strict native execution, not the peer's independent-interpreter result (correlation 0.899887521/0.896598008, RMS error 90.141461/90.667383 PCM LSB).

## Remaining limitations

- **Integrated sound timing/waveforms:** the event-based DUART counter correction improves native correlation from the preceding TX-only run's 0.831907254/0.829002777 to **0.886013889/0.882232851**, but substantial waveform error remains, and native/interpreter results still differ. The fixed lag is an analysis measurement only; no fitted runtime delay, sample masking, waveform shift, or output compensation was introduced. Earlier controlled MAME-mailbox injection leaves similar residuals, locating that error downstream of main-side commands. Remaining bus/instruction timing is under investigation; no single cause of the full residual is established.
- **Counter reset compatibility:** preserving the queued counter expiration and output phase across board reset follows MAME's executed behavior, despite its contradictory reset comment. The reference warm test begins with ISR `$08` already latched; discarding that event previously added roughly 160,000 main ticks. This is explicitly **MAME compatibility, not a physical DUART RESET claim**. Restart now clears old divider residue, and source/preset writes do not rescale an already scheduled event.
- **Retained IRQ/DIVU correction:** `ca08bf9` (local `59f3362`) remains on qualified primary evidence, not improved audio metrics. Motorola documents nominal no-wait IRQ entry at 44 clocks. Its DIVU maximum of 140 clocks does not establish the exact dynamic formula; its “less than 10%” variation statement conflicts with Cwik's published range. Cwik's detailed 10/76/136-cycle timing account explicitly reports partial assembler testing on real 68000 hardware, not exhaustive physical proof or published raw traces. Source links and provenance are preserved in `NOTES.md`.
- Static discovery counts are not proof that every possible game path has been exercised. The 94 unresolved transfers remain reported; strict runtime execution prevents them from being silently accepted as native coverage.
- Pixel equivalence covers the stated attract samples, not every gameplay scene or physical FDP behavior. Disputed clipping behavior and the ROM evidence actually exercised are recorded in `NOTES.md`.
- World ROM execution remains untested and was not substituted for the approved Japanese target.
