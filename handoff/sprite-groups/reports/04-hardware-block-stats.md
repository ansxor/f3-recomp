## Result: hardware sprite-block statistics, commandw and landmakrj

Counts come from per-frame sprite RAM dumps walked by a Python port of `decode.cpp`. Nothing in the repo changed. `git status --short` shows only the three pre-existing user changes: `M docs/notes/raw/docs/taito-f3`, `M runtime/frontend/state.hpp`, `M runtime/rom.cpp`.

### Frames sampled

| Game | Frames run | Full range | Gameplay subset (frames with drawn > 0) |
|---|---|---|---|
| commandw | 1000 | 1..1000 (every frame) | 586..967 (382 frames) |
| landmakrj | 2000 | 1..2000 (every frame) | 1035..2000 (947 frames) |

### Per-game statistics

| Metric | commandw, all frames | commandw, gameplay | landmakrj, all frames | landmakrj, gameplay |
|---|---|---|---|---|
| Drawn entries, mean / max | 46.7 / 140 | 122.4 / 140 | 114.1 / 367 | 241.0 / 367 |
| Blocks, mean / max | 3.6 / 10 | 9.5 / 10 | 114.1 / 367 | 241.0 / 367 |
| Blocks with ≥2 drawn members, mean / max | 3.6 / 10 | 9.5 / 10 | 0 / 0 | 0 / 0 |
| Singleton blocks, mean / max | 0 / 0 | 0 / 0 | 114.1 / 367 | 241.0 / 367 |
| Share of drawn entries in multi-member blocks | 100.0% | 100.0% | 0.0% | 0.0% |

Histogram of drawn members per block, summed over all sampled frames:

| Game | 1 | 2–4 | 5–16 | 17+ |
|---|---|---|---|---|
| commandw | 0 | 924 | 936 | 1778 |
| landmakrj | 228249 | 0 | 0 | 0 |

The commandw totals (924, 936, 1778) are block-frame instances: they sum to 3638 over 1000 frames, which matches the 3.6 mean. The landmakrj total of 228249 matches its 114.1 mean over 2000 frames.

Entry-level flags: commandw has 1768 drawn origin entries, all with ctrl 0/0. Its other drawn entries are members with spritecont 0x7d (X ctrl 1, Y ctrl 3) or 0x75, or with spritecont 0xcd (X ctrl 3, Y ctrl 0). Landmakrj has zero multi bits (spritecont bit 3 clear) on every drawn entry, so every drawn entry is its own origin.

### Commands
- commandw: `./build-commandw/f3rt-run --rom-dir /Users/darien/Downloads/commandw --set commandw --headless --no-audio --unthrottled --frames 1000 --inputs /tmp/f3bs/inputs/commandw.txt --dump-dir /tmp/f3bs/cw-dump --dump-start 1 --dump-every 1`
- landmakrj: `./build-landmakrj/f3rt-run --rom-dir /Users/darien/Workspace/f3-stuff/f3-recomp/roms/landmakrj --set landmakrj --renderer accurate --headless --no-audio --unthrottled --frames 2000 --inputs /tmp/f3bs/inputs/landmakrj.txt --dump-dir /tmp/f3bs/lm-dump --dump-start 1 --dump-every 1`
- Processing: `python3 /tmp/f3bs/blockstats.py cw-dump 32 224 cw.jsonl` and `python3 /tmp/f3bs/blockstats.py lm-dump 24 232 lm.jsonl`, then `python3 /tmp/f3bs/summarize.py cw.jsonl commandw` and `... lm.jsonl landmakrj`.
- The landmakrj default renderer (`enhanced`) refuses to run without strict native execution, and `--translated` alone is not enough. I used `--renderer accurate`. The dump is the raw sprite RAM written by the emulated CPU, so the renderer should not change it. I did not verify this across renderers.

### Scripts and outputs
- `/tmp/f3bs/blockstats.py`: port of `decode_sprite_list`, applied per frame.
- `/tmp/f3bs/summarize.py`: produces the tables above.
- `/tmp/f3bs/cw.jsonl` and `/tmp/f3bs/lm.jsonl`: per-frame records, including every drawn entry's raw bytes, flags, and block membership.
- Dump directories deleted. Run logs are in `/tmp/f3bs/*-run.log`.

### Assumptions
- **Block origin:** an entry is an origin when the previous walked entry had multi = 0 and its X or Y block control is 0 (where `decode.cpp` sets `block_pos`). Members are the drawn entries after an origin, until the next origin in walk order.
- **Bank:** state is chained across consecutive frames, starting from bank 0 and flipscreen off at frame 1. Mid-walk bank switches are followed, as `decode.cpp` does.
- **tile == 0:** the entry advances the state machine and resets multi, but is not a drawn member.
- **Culling and cap:** culled against the scanout crop (visible_y/height from config.toml). Drawn entries are capped at 1024, as in `decode.cpp`.
- **Walk:** no splices, since the game path does not use them. A self-jump ends the list.
- **Not computed:** an alternative block segmentation that ignores the control bits. The entry-level flags show that every drawn entry with multi = 0 has ctrl 0/0, but I did not compute the resulting block counts.

### Not done
- The optional alignment of block boundaries with emit-unit invocations (`sprite-check`). I skipped it.