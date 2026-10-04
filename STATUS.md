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
| Cold-boot audio output | 1,817,655 PCM frames, peak 1289 after the sound-68000 timing corrections |
| Direct native vs MAME pixels | **25/25 frames RGB-exact**, frames 600–3480 at intervals of 120; zero mismatched pixels out of 1,856,000 |
| Main state | Frame 600 matches all 131,072 main-RAM bytes; sampled later RNG states match |
| Actual SDL gameplay | **8,400 frames**, 120,080,217 native blocks, **zero fallback instructions**; real foreground coin/start/selection/movement/Z/X/C input; visible character selection and scored combat |
| Gameplay audio output | 4,241,196 PCM frames, peak 1303; nonzero output, not a waveform-parity claim |

Latest cold-boot/frame/audio verification: `bfa140b`, cherry-picked from runtime peer `b456211`. Compiler/deadline and SDL gameplay verification remains from `d31b3aa`; the SDL input scenario was not repeated for this sound-only change. Runtime ABI/TRAP changes were cherry-picked from peer `36c8555` as `f74f66b`.

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
  --dump-dir build/captures/native-sound-timing --dump-start 600 --dump-every 120 \
  --wav build/native-sound-timing.wav
python3 tools/compare_frames.py \
  wt/runtime/build/captures/mame-aligned build/captures/native-sound-timing --json
```

The last command uses the existing local format-2 MAME baseline. Baseline regeneration is documented in `tools/mame/README.md`. Gameplay evidence is in root `build/captures/native-play-deadline`, `build/native-play-deadline-grid.png`, and `build/native-play-deadline.wav`. ROM bytes, generated C, captures, WAVs, and raw traces remain ignored; completed temporary probes were removed.

The refreshed waveform report is root `build/native-sound-timing-audio.json`. Its measured native result differs slightly from the peer's independent interpreter run; the figures below are from strict native execution.

## Remaining limitations

- **Integrated sound timing/waveforms:** after the scoped sound-68000 corrections, seconds 20–54 against the same reset-aware MAME baseline give fixed lag **−4 samples (−0.134404 ms)**, native L/R correlation **0.844473/0.847484**, and RMS error **112.314/110.057 PCM LSB**. The smaller lag does not establish waveform parity. The runtime owner continues post-initialization sound arithmetic/MMIO event investigation; renderer parity and sound-register replay do not establish integrated audio parity.
- Static discovery counts are not proof that every possible game path has been exercised. The 94 unresolved transfers remain reported; strict runtime execution prevents them from being silently accepted as native coverage.
- Pixel equivalence covers the stated attract samples, not every gameplay scene or physical FDP behavior. Disputed clipping behavior and the ROM evidence actually exercised are recorded in `NOTES.md`.
- World ROM execution remains untested and was not substituted for the approved Japanese target.
