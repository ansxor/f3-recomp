IN PROGRESS

# Phase 4 — Land Maker 1v1 rollback netplay

Worktree `wt/netplay`, branch `netplay`, based on `checkpoint-5-sound-default`. Local commits only; no pushes or edits to integration/other worktrees. Snapshot, rollback, UDP relay, client integration and impaired-network acceptance are not yet verified. Prior phase evidence follows; it is not netplay acceptance.

## Delivered

- Decoded interpreted-driver log landed before native implementation: timestamped mailbox publication/consumption/dispatch, note allocation, physical voice ownership, ES5505 sample/pitch/loop/volume/filter/bank/output pair, ES5510 programming, DUART and MB87078 writes. Full register stream and onset projection are available.
- `--sound-driver oracle|native` on both frontends and seeded gameplay. Oracle remains default. Native is an independent **literal statically recompiled ROM driver**, not a high-level sequencer rewrite or trace replay; no sound-interpreter fallback. ES5505/ES5510 and SDL3 are unchanged.
- `tools/compile_sound.py`, pinned 68000 timing metadata and native runtime; generated ROM code stays ignored. CMake generates the sound program with `F3_ROM_DIR`.
- `tools/compare_sound.py`: exact full-record comparison, including timestamps, reads and ownership metadata; no normalization or tolerance.
- `f3rt-sound-extract`: normal main-game boot, frozen-main packet scheduling, complete sound trace and optional full/event-window WAV. `tools/decode_sound.py` produces sequence/SFX events.
- `docs/SOUND-DRIVER.md`: packet grammar, main selectors, tables, allocation, sequencer/timer arithmetic, DSP/gain protocol, ROM addresses, evidence, reproduction commands and limits.

## Exercised evidence

- Seed 5 ×6000 frames: **12,258,121 identical oracle/native sound records**, including 9,643 main writes, 10,489,728 sound writes, 1,420,314 sound reads and one warm reset. Both main runs execute 80,338,232 native blocks with zero fallback.
- 852 published/consumed/dispatched commands, 1,639 allocated notes, 3,768 voice contexts. All 2,202 non-startup voice starts have note-origin commands: 277 direct and 1,925 sequenced; two additional starts are silent startup probes.
- Complete 3,029,425-frame seeded WAVs are byte-identical across both sound drivers and the original untraced oracle capture.
- Fresh ×3600 attract runs: native-main/oracle-sound, interpreted-main/oracle-sound and native-main/native-sound produce identical complete 1,817,655-frame WAVs. Native main: 51,507,335 blocks, zero fallback.
- MAME comparison, seconds 20–54: correlation **0.9956761849580716 / 0.9952171577492129**, RMS error 18.722660457703864 / 19.499802079856668 LSB, fixed lag -1 sample. Exact baseline retained.
- Extracted music: 1,697,152 identical records and identical 148,805-frame event WAVs, peak 414. Extracted SFX: 1,446,519 identical records and identical 89,283-frame event WAVs, peak 530. Default boot includes the game's output-gain initialization at 13.23 seconds; event origin is 15.268770 seconds.
- Runtime `f3rt-check` and five decoder boundary/ownership tests pass. Extractor rejects a packet scheduled exactly at the capture endpoint. Actual CLI runs exercised both driver modes and event-window WAV output.

## Limits

- Native mode targets sound ROM CRC32 `5a7e9117`. Its complete aligned map contains 944 unlowered candidates, including overlapping/data decodes; none is reached in the exercised runs. Executing one or work-RAM code fails with PC/opcode, never silently falls back. This is not a general-purpose 68000 emulator or exhaustive proof of every command/alias/error path.
- Timing matches the interpreted oracle at instruction boundaries, not physical bus cycles. Existing intra-instruction read/modify/write versus sample-edge residual remains; MAME correlation is not hardware waveform equality.
- Note-origin IDs are RAM-linked causal ownership, not a claim that every later control write has one exclusive originating command. Control commands and all subsequent register writes remain explicit in the full timeline. Unknown alias origins remain null; `--notes-only` intentionally omits later envelope/control writes.
- Extraction accepts raw driver packets, not a packaged named-song catalog. Earlier boot overrides may retain startup attenuation. No alternate audio backend or hardware-model changes were introduced.

Evidence remains local and ignored: `build/seed5-oracle-v2.*`, `build/seed5-native.*`, `build/seed5-parity.json`, `build/final-*`, `build/music-*`, `build/sfx-*`. Reproduce with README and docs/SOUND-DRIVER.md.
