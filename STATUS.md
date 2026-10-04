DONE (limits: sub-instruction bus timing of sound-68000 RMW vs sample edges; audio corr 0.995676185/0.995217158, RMS 18.722660458/19.499802080)

# Integrated Land Maker acceptance

The integrated product meets the orchestrator's accepted finish line: strict native execution, exact sampled reference frames/RAM, full native/interpreter audio equality, and fresh SDL gameplay with audio output. **MAME waveform output is not byte-exact.** The documented sub-instruction sound-bus limit below is explicitly accepted; no cycle-stepped bus model is being started.

Target: **Land Maker Japan 2.01J (`landmakrj`)**, as approved after identifying the supplied ROMs. World `landmakr` remains configuration-only because its program lanes were not supplied.

## Implemented

- Verified four-lane ROM loading and Capstone 68020 decoding; vector, branch, jump-table, actor-bytecode, and register-staged callback discovery.
- Native C emission with lazy flags, guest-stack calls, dispatch entries for block interiors, and strict rejection of unresolved execution in `landmakr`. Diagnostic interpreter fallback remains explicit opt-in.
- ABI v2 event deadlines: multi-instruction blocks yield at instruction boundaries for scheduled events and IRQ-mask changes. Generated sources reject incompatible runtime headers.
- Source- and trace-backed MOVEM, rotate, and software-TRAP timing corrections, without frame offsets, RNG overrides, or fitted delays.
- Native sound-mailbox/reset bus synchronization: device time catches up before access, without changing CPU cycle costs, ABI layout, lazy flags, or boundary-only main IRQ entry.
- Sound-68000 timing corrections: positive-source MULS includes its terminal Booth transition; STOP exposing a pending IRQ retains entry debt and the reference's four-clock polling iteration.
- Device-first sound scheduling uses fractional/debt-based instruction deadlines, independent of main-block partitioning. Main execution retains multi-instruction native blocks; no fitted phase, delay, sample masking, or waveform compensation is present.

## Observed verification

| Check | Result |
| --- | --- |
| Discovery | 43,986 instructions; 163,286 bytes / 2 MiB (7.786%); 2,040 function candidates; 94 unresolved static transfers |
| Lowering | All 43,986 discovered instructions lowered natively; zero lowering fallbacks |
| Instruction differential | 5,000 passed, zero failed, zero unsupported; independent corrected Musashi EC020 reference, including cycles and exception state |
| Discovery/deadline regressions | Six passed; synthetic generated C executes deadline equality/overshoot, canonical flags, and interior-PC resumption |
| Strict native cold boot | 3,600 frames; 52,342,915 native blocks; zero fallback instructions |
| Cold-boot audio output | 1,817,655 stereo PCM frames, peak 1268; complete standard-native WAV byte-identical to the frozen interpreter WAV |
| Direct native vs MAME pixels | **25/25 frames RGB-exact**, frames 600–3480 at intervals of 120; zero mismatched pixels out of 1,856,000 |
| Main state | Frame 600 matches all 131,072 main-RAM bytes; earlier sampled RNG verification is retained |
| Fresh SDL gameplay on `b3f5578` | **6,600 frames**, 93,893,755 native blocks, **zero fallback**, exit 0; real foreground coin/start/player-select/arrows/Z/X/C input; inspected player-select captures, live window and final rendered surface; 1P score progressed from 50 to 110 |
| Fresh SDL audio output | Default audio device opened/resumed and accepted queued audio; 3,332,368 PCM frames, peak 1234, 5,536,667 nonzero samples |
| Final native audio vs MAME, seconds 20–54 | L/R correlation **0.9956761849580716/0.9952171577492129**, RMS error **18.722660457703864/19.499802079856668 PCM LSB**, analysis lag **−1 sample (−0.033601 ms)** |
| Full device chronology | Corrected native/interpreter sound reads/writes, interrupt-mask changes, IACKs, DUART register/edge logical timestamps and sample boundaries match; 14,706,484 records per run over startup–54 s |
| Native-one diagnostic on preceding `5fffd0e` | Full WAV byte-identical to interpreter; 13.672437 s versus native-block 9.662622 s, **41.5% slower** in that control run; not used for production dispatch |

All final cold-boot, frame/RAM, waveform and SDL gates were exercised on **`b3f5578`**, the local import of frozen runtime **`5699a0b`**, following the MULS/STOP corrections. The native ABI bus fix `2762bfe` is retained. Compiler differential/discovery checks above are earlier evidence for unchanged lowering, not claimed as newly rerun. Runtime's separately owned status was not cherry-picked over this status.

## Reproduce

From the repository-root `integration` worktree, after the dependency setup in `README.md`:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DF3_ROM_DIR=../roms/landmakr && cmake --build build --target landmakr -j 4 && ./build/landmakr
```

Controls: 5/6 coin, 1/2 start, arrows, Z/X/C. The executable uses generated main-CPU code by default and fails on untranslated execution; do not add `--allow-fallback` for acceptance.

```sh
PYTHONPATH=build/python python3 -m unittest discover -s tools -p 'test_*.py'
PYTHONPATH=build/python python3 tools/differential/run.py \
  --musashi runtime/third_party/musashi --output build/differential --cases 5000
./build/landmakr --headless --no-audio --frames 3600 \
  --dump-dir build/captures/native-final --dump-start 600 --dump-every 120 \
  --wav build/native-final.wav
python3 tools/compare_frames.py \
  wt/runtime/build/captures/mame-aligned build/captures/native-final --json
cmp wt/runtime/build/captures/mame-aligned/frame_0600/mainram.bin \
  build/captures/native-final/frame_0600/mainram.bin
cmp wt/runtime/build/audio-partition.wav build/native-final.wav
PYTHONPATH=wt/runtime/build/python python3 tools/compare_audio.py \
  wt/runtime/build/mame-audio-reset-v2.wav build/native-final.wav \
  --start 20 --end 54 --json build/native-final-audio.json
./build/landmakr --frames 6600 \
  --dump-dir build/captures/native-final-play --dump-start 1200 --dump-every 300 \
  --wav build/native-final-play.wav --surface build/native-final-play-window.bmp
```

The frame comparison uses the existing local format-2 MAME baseline; regeneration is documented in `tools/mame/README.md`. Fresh SDL evidence is root `build/captures/native-final-play`, `build/native-final-play.wav`, and the final `SDL_RenderReadPixels` surface `build/native-final-play-window.bmp`/`.png`. Frame 3000 and 3300 captures show PLAYER SELECT; the final surface shows active gameplay with 1P score 110. An actual live-window screenshot at 1P score 50 is `wt/recomp/build/native-final-live.png`. ROM bytes, generated C, captures, WAVs and raw traces remain ignored; temporary trace/input source and executables were removed.

Final waveform report: root `build/native-final-audio.json`, using the unchanged 29,761 Hz reset-aware MAME baseline without resampling. This is measured from standard strict-native execution and matches the frozen interpreter result because the **entire WAV is byte-identical**, not because interpreter metrics were substituted. Earlier MULS-only and STOP-only native evidence remains separately recorded in `NOTES.md` and ignored `build/native-muls*` artifacts.

## Main-to-sound timing diagnosis at `2762bfe`

The complete 3,600-frame captures have **no mailbox/reset writes during seconds 20–54**. Surrounding writes occur at main ticks 283674747 (17.7296716875 s) and 916142598 (57.258912375 s). Across the entire run, all 1,316 events already matched PC/address/width/value and absolute CPU tick before the fix. The defect was earlier **effective device delivery**, not lowering-cycle drift:

| Event | PC / write address | CPU tick, both paths | Old native device tick | Interpreter and corrected device tick |
| --- | --- | --- | --- | --- |
| First reset assertion | `002f88` / `c80100` | 31831348 | 31831299 (−49) | 31831348 |
| First reset release | `002fd0` / `c80000` | 31836522 | 31836513 (−9) | 31836522 |

The preceding MOVEM and CLR.B correctly charge 49 and 9 cycles inside their native blocks, but device time previously remained at block entry. In total, 64 writes took effect 2–49 main ticks early. Native ABI callbacks now synchronize devices before sound-mailbox/reset access; all 64 delivery mismatches disappear with no change to event CPU timestamps. This is not deadline quantization, a fitted offset, or an opcode-cost correction.

At that earlier revision, a diagnostic one-instruction native dispatcher produced the entire interpreter WAV byte-for-byte, and the write-sync control produced the corrected native WAV byte-for-byte. The subsequent all-device investigation isolated the remaining host-partition-dependent sound scheduling; `b3f5578` removes that native/interpreter waveform gap while retaining production multi-instruction main blocks.

Ignored captures are under `wt/recomp/build/`: `events-native.csv`, `events-interpreter.csv`, `events-native-fixed.csv`, and the `events-native-sync`/`events-native-one` CSV/WAV controls. Standard native acceptance artifacts are root `build/captures/native-main-sound-sync` and `build/native-main-sound-sync.wav`. Temporary probe source/executables were removed; the real sound-core temporal regression remains in `runtime/check.cpp`.

## All-device chronology and scheduling cutover

Frozen native/interpreter probes preserve their complete uninstrumented WAVs. Main device values/instruction timestamps match, including **2,004 vblanks, 2,004 IRQ3 events, 4,008 IRQ entries/acknowledgments, 32,064 IO/EEPROM byte reads and 6,012 IO byte writes** over seconds 20–54. There are no main timer-control, mailbox or reset-port accesses in that window.

The first sound DUART write, PC `c19dde` to `28001f` with value `40`, has the same sound-instruction accounting tick **34831557**, but device cursors **34831552 native / 34831556 interpreter**. Sound execution previously ran before the corresponding device-time advance. In the requested window, the first matched OTIS page write (`c18f2a` → `20001e`, value 15) occurs at **320000013 / 320000048**; the first DUART half-period edge is **320002124 / 320002188**, delivered at DUART cursors **320002148 / 320002192**. Of 34,000 matched DUART writes in that window, 32,155 have differing old device timestamps. This is a sound scheduler defect, not main-lowering or main IRQ drift; indiscriminate main-ABI catchup would not fix it.

After device-first instruction deadlines, all **480,231 sound reads, 3,731,684 writes, 201,025 mask changes, 34,000 IACKs, 170,000 DUART counter events and 1,011,874 sample boundaries** in seconds 20–54 match in event identity and logical time. DUART register accesses, IRQ edges and TX events also match. Host callback endpoints can still differ between sound observation points: those host/debug cursors are not claimed identical, while the CPU-visible bus/IRQ/sample chronology and complete PCM are identical. Binary traces and their schema remain under `wt/recomp/build/{frozen,corrected}-*.trace` and `device-chronology-schema.json`.

Raw IRQ notifications are qualified separately: sound-domain stage-0 callbacks retain **3,380 differing endpoint timestamps** over startup–54 s (**2,914** in seconds 20–54), native later by 4–68 main ticks. Their PC/address/value/width match. These timestamps are DUART advance endpoints, not original edge clocks; the separate DUART edge records and CPU-visible bus/mask/IACK/sample observations match. The chronology claim does not erase or reinterpret those raw differences.

## Remaining limitations

- **Accepted sub-instruction sound-bus limit:** the runtime peer's concrete reference witness is `ANDI.W #$fffc,(A0)` at sound PC `c17814`: MAME reads at main tick **341008964** and writes at **341008972**, straddling sample 634298's edge at **341008971.472733**. The runtime performs both operations atomically at **341008949**. Instruction-level device ordering is now deterministic, but read-modify-write bus phases are not cycle-stepped. Eliminating this remaining class of discrepancy requires a **cycle-stepped 68000 bus model**, including intra-instruction access/prefetch/IRQ timing. That work is explicitly out of this accepted finish line. The witness establishes a real residual mechanism, not an exhaustive proof of every mismatched sample. No compensating delay or opcode-specific patch was added.
- **Retained STOP correction:** the runtime peer's executed MAME microprogram measures 20 clocks without STOP/IRQ and 72 with it, isolating 52 clocks (4 instruction + 4 poll + 44 IRQ entry). The correction matches that evidence. The unsupported no-poll variant was rejected despite higher aggregate correlation. This is reference compatibility, not independent hardware timing proof.
- **Counter reset compatibility:** preserving the queued counter expiration and output phase across board reset follows MAME's executed behavior, despite its contradictory reset comment. The reference warm test begins with ISR `$08` already latched; discarding that event previously added roughly 160,000 main ticks. This is explicitly **MAME compatibility, not a physical DUART RESET claim**. Restart now clears old divider residue, and source/preset writes do not rescale an already scheduled event.
- **Retained IRQ/DIVU correction:** `ca08bf9` (local `59f3362`) remains on qualified primary evidence, not improved audio metrics. Motorola documents nominal no-wait IRQ entry at 44 clocks. Its DIVU maximum of 140 clocks does not establish the exact dynamic formula; its “less than 10%” variation statement conflicts with Cwik's published range. Cwik's detailed 10/76/136-cycle timing account explicitly reports partial assembler testing on real 68000 hardware, not exhaustive physical proof or published raw traces. Source links and provenance are preserved in `NOTES.md`.
- Static discovery counts are not proof that every possible game path has been exercised. The 94 unresolved transfers remain reported; strict runtime execution prevents them from being silently accepted as native coverage.
- Pixel equivalence covers the stated attract samples, not every gameplay scene or physical FDP behavior. Disputed clipping behavior and the ROM evidence actually exercised are recorded in `NOTES.md`.
- World ROM execution remains untested and was not substituted for the approved Japanese target.
