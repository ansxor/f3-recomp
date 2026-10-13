# Discovery log triage

The discovery log only observes: cycles, native blocks and CRCs don't change. Log frames are 1-based;
`f3rt-tool sprite-check` prints 0-based `Machine::frame`. For screens attract mode never shows, play
the game binary interactively with its `--discovery-log FILE` flag and analyse that file with
`g.discover(FILE, ...)`.

```python
import re
from collections import Counter

aw = g.run(frames=3000, all_writers=True, out="build/f3a/writers")  # logs known writers too (known=1)
aw.summary["video_write"], aw.summary["sprite_stray"], aw.summary["video_fallback"]

aw.discover(ranges=True, unknown_only=True)       # (layer, routine) groups with unlisted PCs
# g.discover(frames=3000) runs the game itself, logging only writers missing from games/<id>/video/
# g.discover("path/to/discovery.log", ranges=True) analyses an existing log

new = [dict(re.findall(r"(\S+)=(\S+)", line), category=line.split()[1])
       for line in (aw.path / "discovery.log").read_text().splitlines() if line.startswith("NEW ")]
todo = [e for e in new if e.get("known") != "1"]  # e: category, layer, pc, frame, addr, ...
Counter(e["category"] for e in todo)
```

Triage each entry in `todo` by `category`:

- `sprite-stray`: a sprite-RAM write outside every emit unit, from a PC not in
  `[video.frame_writers]`. If the PC does frame setup/clear, add it to `frame_writers` with evidence;
  otherwise extend or add a `[[video.emit_units]]` entry. Only appears with strict native execution
  and in games that declare emit units.
- `video-write` (`layer=...`): a store from a PC that `video_writer_known` in
  `games/<id>/video/video.cpp` doesn't list. Find out what it writes (`g.dis`, `g.writes(e["pc"])`)
  before you add it to the list.
- `video-fallback`: the game renderer used the FDP oracle (the entry gives component and reason).
  Only appears with the game binary's `--renderer enhanced` or a developer game/compare renderer.
- `unit-replay-abort`, `unit-stats`: summary lines only. Replay mismatches need
  `f3rt-tool sprite-check`.
