---
source_url: docs/developer/DECISIONS.md + docs/SOUND-DRIVER.md
ingested: 2026-10-11
sha256: c973fbf20463cd8c62313c457b89b8681dcb3e3bb8f512bffc6546a65d1e09e4
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "repository record of runs (secondhand: command output and artifacts under ignored build/ were not retained); source doc deleted after ingest; F3SND1/F3SND2 trace, tools/compare_sound.py"
---
<!-- excerpt: docs/developer/DECISIONS.md lines 164-173 -->
- Isolated `wt/audio` / branch `audio` starts at `checkpoint-2-video` (`4200b04`). The sound CPU remains interpreted and ES5505/ES5510 plus SDL3 output are unchanged. `F3SND1` observation records effective device ticks, emitted-sample ordinals, actual bus results and writer PCs, without extra device reads.
- Initial seed-5 capture: 6000 frames, 80,338,232 native main blocks, zero fallback, 3,029,425 stereo PCM frames. Traced/untraced WAVs are byte-identical. Raw trace contains 9,643 main writes, 10,489,728 sound writes, 1,420,314 sound reads and one warm board reset.
- Fresh 3600-frame attract verification after capture integration: native/interpreter complete WAVs byte-identical (SHA-256 `62bac3a4d1baef4e7c2343ce987e44eb654a71f90d653b5b88c1d12d15eff05b`), 1,817,655 stereo frames, native blocks 51,507,335 and zero fallback. MAME comparison over seconds 20–54 exactly retains correlation 0.9956761849580716/0.9952171577492129, RMS 18.722660457703864/19.499802079856668 LSB, lag -1 sample. `runtime tests` passes. Evidence is local ignored `build/oracle-*` and `build/seed5*`.
- Decoded-log milestone `c6115dd`: seed 5 ×6000 has 852 packet publications, consumptions and task dispatches; 1,639 allocated note nodes; all 2,202 non-startup ES5505 starts have a RAM-linked source command (277 direct, 1,925 sequenced). The augmented observer remains WAV-identical to the untraced run. Submission time alone is not causal ownership: the ROM queues ring offsets and the voice's +$0e points to an allocated/reused note node.
- ROM evidence corrections: sound ring is 1024 bytes with doubled producer/consumer indices, not 2048 command bytes; 8e operands are sequence/track/key/velocity; MB87078 addresses are control and data latches, not left/right channels. Voice stride is $aa; allocation scans priority classes at $c17678 and may fail, rather than unconditionally stealing the oldest voice. $d85a is also a rotating software-update list. $c17b32 is a startup register-template copy, not a recurring envelope routine.
- Actual DUART IRQ reads in the seeded trace have modal spacing 16,000 main ticks; initialization uses counter $07d0 and ACR $60. ROM sequence accumulation subtracts 625 at threshold ($c12e3e), giving 192 ticks/sec for rate 120 under the observed 1 kHz interrupt cadence. This establishes driver timing arithmetic, not physical bus-cycle accuracy. Existing intra-instruction read/write versus sample-edge residual at $c17814 remains intentionally unchanged.
- Native driver gate: literal static ROM recompilation, opt-in `--sound-driver native`, independent CPU state and no Musashi sound execution/fallback. Seed 5 ×6000 compares 12,258,121 complete F3SND2 records exactly, including all reads/writes/timestamps and 3,768 ownership contexts; complete WAVs match both the interpreted driver and the untraced baseline. Native/interpreted main attract audio also remains identical, and MAME correlation retains the exact prior values.
- Probe correction from the strict comparison: IRQ stack writes can carry the interrupted instruction's PPC. PC-only voice probes invented metadata at tick 492,122,198; qualifying the actual destination address and width removes those false snapshots without altering bus behavior. Legato ownership is observed after `$c1762c`, and F3SND2 orders RAM snapshots before completed context markers.
- Compiler correction: Capstone rejects `ANDI` to CCR when its architecturally ignored upper immediate byte is nonzero (reachable `$c139d6`). Normalize that byte only for CCR/bit immediate decoding; preserve the effective operand and pinned 68000 timing. Native trace addresses use the physical 24-bit bus mask.
- Extraction boot evidence: main `$3400/$3404/$340a/$3410` publishes gain requests at `$c007f8..$c007fb` at 13.2338 seconds. Freezing at 660 frames retains startup attenuation. Default 900-frame boot reaches an event origin of 15.268770 seconds without hidden setup packets. Music and SFX examples have exact oracle/native traces and WAVs (peaks 414 and 530 respectively).

<!-- excerpt: docs/SOUND-DRIVER.md lines 369-391 -->

`compare_sound.py` compares every field in order, including reads, repeated
writes, timestamps, PCM ordinals, PCs and RAM ownership metadata. No lag fitting,
sorting, tolerance or write deduplication. A mismatch reports the first record
and recent submitted packets; truncated/incomplete captures fail.

Final seed-5 gate: **12,258,121 identical records**, 3,768 voice contexts,
1,639 note allocations, 852 commands and byte-identical 3,029,425-frame WAVs.
The native main CPU executes 80,338,232 blocks with zero fallback in both modes.
Fresh 3600-frame attract WAVs are also identical for native-main/oracle-sound,
interpreted-main/oracle-sound and native-main/native-sound. MAME correlation
retains exactly the observation-gate values above; it does not improve or
regress the separate hardware-model residual.

## Extracting music and SFX events

`f3rt-tool sound-extract` runs the interpreted main game for 900 frames by default,
then freezes it and advances only the sound subsystem. The game publishes its
startup packets itself. Main `$3400/$3404/$340a/$3410` writes the four gain
requests at main `$c007f8..$c007fb` around 13.2338 seconds; the old 660-frame
freeze point preceded these writes and produced highly attenuated output.
Default 900-frame event origin is tick 244,300,326 / PCM frame 454,413
(15.268770 seconds) with the supplied ROM and cold default state.
