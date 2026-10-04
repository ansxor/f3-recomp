DONE — coverage fix verified on 16 seeds × 40,000 strict-native frames; decoder/ISA and sound-bus limits documented below

# Integrated Land Maker acceptance

The user's first-match strict-native crash at `0xed198` invalidated the previous coverage acceptance. The fix replaces Japan's observed-PC/table seeds with independent decoding at every word-aligned ROM offset; it also covers computed destinations with no pointer literal, such as the handoff's `0xf449e`. The old short SDL sessions were insufficient coverage evidence. **MAME waveform output remains non-byte-exact**, with the separately accepted sound-bus timing limit below unchanged.

Target: **Land Maker Japan 2.01J (`landmakrj`)**, as approved after identifying the supplied ROMs. World `landmakr` remains configuration-only because its program lanes were not supplied.

## Implemented

- Verified four-lane ROM loading and independent Capstone 68020 decoding at all 1,048,576 even offsets in the 2 MiB Japan ROM. No observed-PC, script-table or callback seeds are needed for Japan coverage.
- Native C emission with lazy flags, guest-stack calls, overlapping dispatch entries, length-correct successors and bounded 64-byte ROM-page blocks. Known illegal/A-line/F-line words use architectural exception handlers. Unsupported execution still fails strictly; diagnostic interpreter fallback remains explicit opt-in.
- ABI v2 event deadlines: multi-instruction blocks yield at instruction boundaries for scheduled events and IRQ-mask changes. Generated sources reject incompatible runtime headers.
- Source- and trace-backed MOVEM, rotate, and software-TRAP timing corrections, without frame offsets, RNG overrides, or fitted delays.
- Native sound-mailbox/reset bus synchronization: device time catches up before access, without changing CPU cycle costs, ABI layout, lazy flags, or boundary-only main IRQ entry.
- Sound-68000 timing corrections: positive-source MULS includes its terminal Booth transition; STOP exposing a pending IRQ retains entry debt and the reference's four-clock polling iteration.
- Device-first sound scheduling uses fractional/debt-based instruction deadlines, independent of main-block partitioning. Main execution retains multi-instruction native blocks; no fitted phase, delay, sample masking, or waveform compensation is present.

## Observed verification

| Check | Result |
| --- | --- |
| Discovery | 1,048,576 aligned offsets inspected; 464,523 decoded starts, including data/overlaps; 584,053 decoder rejections |
| Emission | 460,668 native decoded entries plus 568,753 shared illegal/A-line/F-line exception entries; 17,534 page blocks |
| Explicit coverage limits | 3,855 decoded-but-unsupported entries; 15,300 rejected entries with otherwise recognized primary words; neither category is asserted unreachable |
| Instruction differential | Fresh 5,000 passed, zero failed, zero unsupported; registers, flags, cycles, exceptions and ordered writes, including BCD, CHK, MOVEP/MOVES, CAS/CAS2, traps and alias/empty-mask boundaries |
| Discovery/deadline regressions | Fresh 13 passed; synthetic executable C covers overlapping starts, extension-word skipping, deadline equality/overshoot and canonical flags |
| Runtime devices | Fresh `f3rt-check` passed, including reset and main/sound synchronization |
| Strict native cold boot | 3,600 frames; 51,507,335 native blocks; 977,201,324 cycles; zero fallback |
| Cold-boot audio output | 1,817,655 stereo PCM frames, peak 1268; complete native WAV byte-identical to the frozen interpreter WAV |
| Direct native vs MAME pixels | **25/25 frames RGB-exact**, frames 600–3480 at intervals of 120; zero mismatched pixels out of 1,856,000 |
| Main state | Frame 600 matches all 131,072 main-RAM bytes |
| Reproducer gameplay capture | Seed 5 reaches frame 3,500 with zero fallback, 47,504,223 native blocks; inspected active first-match framebuffer, 1P score 30 |
| Long strict-native gameplay | **Seeds 1–16 × 40,000 frames = 640,000 frames**, 8,551,456,140 native blocks, **zero fallback**, zero failures; includes handoff failures 5–7 |
| Native audio vs MAME, seconds 20–54 | L/R correlation **0.9956761849580716/0.9952171577492129**, RMS error **18.722660457703864/19.499802079856668 PCM LSB**, analysis lag **−1 sample (−0.033601 ms)** |
| Clean build, AppleClang 21 arm64 Release, `-j4` | Configure/discovery/emission 12.72 s; compile/link 62.96 s; measured while gameplay jobs ran |
| Binary size | `landmakr` 64,950,520 bytes versus previous 7,294,872; native archive 77,686,440 versus 8,591,784 bytes |

The coverage implementation is `7e0bd43`, following regression-tool commit `7d7f6ef`, with unchanged runtime `b3f5578` (imported from frozen `5699a0b`). Acceptance was completed in isolated `wt/recomp/build/coverage` before updating integration. Exhaustive candidate discovery is an overapproximation, not a proof of reachability or universal decoder/ISA support.

## Seeded strict-native gameplay

Every row completed 40,000 frames with fallback disabled and zero fallback
instructions. Two independent runner batches exercised seeds 1–8 and 9–16;
elapsed times were 1,928.451 s and 1,922.876 s while sharing the host. The
final framebuffer CRCs record deterministic run outcomes, not MAME gameplay
pixel-equivalence claims.

| Seed | Native blocks | Final framebuffer CRC |
| --- | ---: | --- |
| 1 | 536,824,912 | `131034a3` |
| 2 | 533,870,651 | `5ec55dda` |
| 3 | 535,596,406 | `ced0390e` |
| 4 | 535,618,377 | `05bd2b0d` |
| 5 | 534,492,746 | `1101a39b` |
| 6 | 529,768,165 | `54a2ed76` |
| 7 | 536,649,307 | `e9a0299a` |
| 8 | 536,771,773 | `5892f0dd` |
| 9 | 537,594,246 | `7f5cf230` |
| 10 | 534,119,872 | `28774d0f` |
| 11 | 534,256,119 | `5470ff88` |
| 12 | 529,419,728 | `dd16329c` |
| 13 | 535,629,043 | `f126eae1` |
| 14 | 534,004,931 | `1a3906b0` |
| 15 | 533,779,909 | `12c0848e` |
| 16 | 533,059,955 | `dbc48e33` |

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
cmake --build build --target f3rt-gameplay-regression -j 4
python3 tools/run_gameplay_regression.py --rom-dir ../roms/landmakr \
  --frames 40000 --seeds 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16
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

The frame comparison uses the existing local format-2 MAME baseline; regeneration is documented in `tools/mame/README.md`. Current candidate artifacts are `wt/recomp/build/captures/coverage-native`, `coverage-native.wav`, `coverage-audio-comparison.json`, and `coverage-seed5-3500.bmp`/`.png`. The latter is a real headless machine framebuffer, not a new SDL-input claim. Previous SDL evidence on `b3f5578` remains historical and did not prevent the subsequently reported crash. ROM bytes, generated C, captures and WAVs remain ignored.

The current waveform report uses the unchanged 29,761 Hz reset-aware MAME baseline without resampling. It is measured from standard strict-native execution and matches the frozen interpreter result because the **entire WAV is byte-identical**, not because interpreter metrics were substituted. Earlier MULS-only and STOP-only evidence remains separately recorded in `NOTES.md`.

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
- All-aligned discovery removes dependence on identifying indirect destinations: every even ROM PC is examined, irrespective of pointer alignment, preceding data or routine length. Capstone rejections, reserved/malformed forms, unsupported CALLM/RTM and RAM/trace execution remain explicit limits. The reports do not label unexecuted candidates as unreachable data, and strict runtime rejection remains enabled. Seeded runs cannot prove every possible game state.
- Pixel equivalence covers the stated attract samples, not every gameplay scene or physical FDP behavior. Disputed clipping behavior and the ROM evidence actually exercised are recorded in `NOTES.md`.
- World ROM execution remains untested and was not substituted for the approved Japanese target.
