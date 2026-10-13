# Discovery log triage

The discovery log only observes: cycles, native blocks and CRCs don't change. Log frames are 1-based;
`f3rt-tool sprite-check` prints 0-based `Machine::frame`. For screens attract mode never shows, play
the game binary interactively with its `--discovery-log FILE` flag and analyse that file with
`uv run f3 analyze discover FILE --ranges`.

```sh
uv run f3 analyze run --game <id> --frames 3000 --all-writers --out build/f3a/writers  # known writers too (known=1)
uv run f3 analyze discover build/f3a/writers --game <id> --ranges --unknown-only       # groups with unlisted PCs
# `discover --game <id> --frames 3000` (no log) runs the game itself, logging only writers missing from games/<id>/video/
```

The run prints `DISCOVERY sprite_stray=.. video_write=.. video_fallback=..` counts. To list the
unknown entries by category, script it:

```python
import re
from collections import Counter
from pathlib import Path

log = Path("build/f3a/writers/discovery.log").read_text().splitlines()
new = [dict(re.findall(r"(\S+)=(\S+)", line), category=line.split()[1])
       for line in log if line.startswith("NEW ")]
todo = [e for e in new if e.get("known") != "1"]  # e: category, layer, pc, frame, addr, ...
print(Counter(e["category"] for e in todo))
for e in todo:
    print(e["category"], e.get("layer", ""), e["pc"], "frame", e["frame"])
```

Triage each entry by `category`:

- `sprite-stray`: a sprite-RAM write outside every emit unit, from a PC not in
  `[video.frame_writers]`. If the PC does frame setup/clear, add it to `frame_writers` with evidence;
  otherwise extend or add a `[[video.emit_units]]` entry. Only appears with strict native execution
  and in games that declare emit units.
- `video-write` (`layer=...`): a store from a PC that `video_writer_known` in
  `games/<id>/video/video.cpp` doesn't list. Find out what it writes (`dis`, `writes <pc>`) before you
  add it to the list.
- `video-fallback`: the game renderer used the FDP oracle (the entry gives component and reason).
  Only appears with the game binary's `--renderer enhanced` or a developer game/compare renderer.
- `unit-replay-abort`, `unit-stats`: summary lines only. Replay mismatches need
  `f3rt-tool sprite-check`.
