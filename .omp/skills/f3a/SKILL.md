---
name: f3a
description: "Analyse a Taito F3 game's 68EC020 program with the f3a Python library (tools.analysis.f3a) in the eval kernel: find routines, globals, record layouts, video writers, frame order, dispatch targets; run the game headless with scripted input, RAM pokes and execution watches to verify; file findings in docs/notes. Use for any reverse-engineering question about a game in games/<id>/."
---

# f3a: game analysis for Taito F3

Static output shows what the code *can* do; headless runs show what it *does*. A claim is finished
only when a run supports it; otherwise mark it `[INFERENCE]`.

## Setup: library in the eval kernel

Use the `eval` tool (`language: "py"`):

```python
from tools.analysis import f3a
g = f3a.game("landmakrj")        # analysed once, then cached in the kernel
```

API (`help(g.<method>)` lists each method's options):

```python
def game(game_id, rom_dir=None, profile=None, symbols=None, a5=None) -> Game: ...
def open_run(path, game_id=None) -> Run: ...      # reopen an earlier g.run(out=...) directory
class F3AError(Exception): ...                    # every tool error

class Game:
    id: str
    core: analysis.game.Game                      # underlying analysis, for what methods don't expose
    # positional args as named; options as keywords: from_= for `from`, True = flag on, ints → hex
    def vectors(**o) -> Result; def dis(target, **o) -> Result; def xref(*targets, **o) -> Result
    def flow(target, **o) -> Result; def writes(*targets, **o) -> Result
    def struct(routine, reg, **o) -> Result; def graph(**o) -> Result; def profile(**o) -> Result
    def discover(log=None, **o) -> Result; def mametap(**o) -> Result
    def run(out=None, **o) -> Run                 # headless run; frames=1200 by default
    def address(text) -> int                      # "a5-0x60ac", "irq2", "sub_009b32", symbol, hex
    def label(address: int) -> str
    def rom(address, size) -> bytes; def words(address, count, signed=False) -> list[int]
    def longs(address, count) -> list[int]

class Result(list[Row]):                          # rows plus header notes, in output order
    notes: list[str]                              # counts, caveats, check= verdicts
    text: str                                     # everything; print/repr caps at 60 lines
    def where(predicate=None, **equal) -> Result  # r.where(lambda row: ..., role="write")
    def column(name) -> list
    def show(lines=None) -> None

class Row:                                        # repr = the printed line
    line: str
    fields: dict                                  # raw values (PCs/addresses as int); row.pc == row["pc"]
    def get(name, default=None)

class Run:
    path: Path; game: Game
    log: Result | None                            # the run's report (until/watch rows, notes)
    frames: list[int]                             # dumped frames, 1-based
    output: str; summary: dict[str, str]          # run.txt and its key=value pairs (strings)
    def records(frame=None, **o) -> Result; def sprites(frame=None, **o) -> Result
    def discover(**o) -> Result                   # on this run's discovery.log
    def mem(frame, address, size=1) -> bytes      # address: int, hex text or "a5-0x60ac"
    def b(frame, address) -> int; def w(frame, address) -> int; def l(frame, address) -> int
    def series(address, size=1, frames=None) -> list[tuple[int, int]]   # (frame, value)
    def table(base, stride, count, frame, size=None) -> list[Record]

class Record:
    frame: int; index: int; address: int; bytes: bytes
    def b(offset) -> int; def w(offset) -> int; def l(offset) -> int
```

## Start

1. Orient in `docs/notes/` per `skill://re-wiki`, then read `games/<id>.md` and grep your addresses.
   f3a labels come from those notes (`routines/<game>-<8hex>-<name>.md`, overridden by
   `games/<id>/analysis/symbols.toml`); a trailing `?` (`round_task?`) means the page is a hypothesis.
   If the game's code settles a page's `contradictions`, that is a finding: report it with the run.
2. Map the frame: `g.vectors()`, then `g.flow("irq2", tree=2)` (frame interrupt, callees, hit counts). An
   interrupt that only syncs is not the frame worker; compare hit counts across `irqN`.

## Evidence columns

`xN` executed N times in `profiles/<game>.profile`; `rooted` statically reachable, never seen
executing; `decoded?` may not be code. Without a profile there are no counts and `?dispatch` hints are
weaker: `g.profile(build=True)` first.

## Recipes

**Meaning of a global**
```python
r = g.xref("a5-0x60ac", sort="hits")                 # every read/write/bit test + routine
writers = r.where(role="write")                      # role: read | write | rmw
g.dis(writers[0].routine, from_=writers[0].pc - 0x20, to=writers[0].pc + 0x20)
run = g.run(frames=2000, dump_start=1000, dump_every=1, keep="mainram.bin", out="build/f3a/g60ac")
[(n, v) for n, v in run.series("a5-0x60ac", 2) if v]  # confirm the meaning against a run
```

**Record layout**
```python
g.struct("sprite_list_writers?", "a0", depth=2)      # offset, size, reads, writes, rmw, bits, pcs
g.xref(field="0x1.7", role="write")                  # who writes byte +1 bit 7 of any record
g.xref(field="0x1.7", role="write", within="0x410000-0x4107ff")  # one array, + immediate stores
rec = run.records(base="0x410000", stride=0x100, count=8, active=0.7,
                  check="+0x2.w == +0x2a.w + 4")      # every record of every dumped frame
rec.notes                                            # "holds for N/M active records": report N/M
[r.w(0x2) for r in run.table(0x410000, 0x100, 8, frame=1200)]   # raw values in one frame
```

**Who writes this video RAM?**
```python
g.writes(region="sprites", ranges=True)              # static; or region="0x660000-0x66001f"
aw = g.run(frames=3000, all_writers=True, out="build/f3a/writers")
obs = aw.discover(ranges=True)                       # observed writers per (layer, routine)
obs.where(lambda r: r.unknown_pcs)                   # PCs video.cpp does not list yet
```
Static and observed disagree when the static pass cannot place a pointer (`unknown`/`mixed`/`?`) or
the writer only runs in gameplay (next section). To triage discovery-log `NEW` lines or extend
`video_writer_known` / `[[video.emit_units]]`, read `references/discovery-log.md`.

**Computed jumps, hooks, interpreters**
```python
sites = g.flow("?dispatch")                          # unresolved sites; slot candidates over-approximate
obs = g.flow("?dispatch", observed="build/f3a/ind")  # after g.run(indirect=True, out=...); default: profile .indirect
[(hex(s.site), s.observed) for s in obs if s.observed]   # "(not a static candidate)" = static pass missed it
g.xref(field="0x10", role="write", code=True)        # who stores code addresses into slot +0x10
g.dis(0x2ff42, data="offsets")                       # decode a jmp table(pc,d0.w) table
```
Confirm live targets with `watch=` (below).

**Sprites**
```python
sr = g.run(frames=1210, dump_start=1200, dump_every=1, keep="mainram.bin,graphics.bin", out="build/f3a/spr")
sr.sprites(frames="1200:1210", writer="sprite_list_writers?")   # needs graphics.bin; walks the list as the chip does
```

## Reaching code attract mode never runs

Headless runs stay in attract mode unless the run presses buttons. Script grammar: header comment of
`runtime/debug/input_script.hpp`; read it, do not restate it from memory. Script frames are 1-based,
same as dumps and logs.

```python
from pathlib import Path
script = Path("build/f3a/landmakrj-inputs/<scene>.txt")
script.parent.mkdir(parents=True, exist_ok=True)
script.write_text("# <scene>: <target> reached at frame <N>\n"   # fill in once a run shows it
                  "700+20 coin\n1200-end mash seed=3\n2000-2100 p2 right+b1\n"
                  "3000-end poke 0x401f54.w=10\n")
run = g.run(frames=8000, inputs=str(script), until=["sub_09d72a"],
            dump_every=100, keep="rendered.png,mainram.bin", out="build/f3a/scene")
run.log.where(reached=False)          # targets not hit (row: target, reached, frame, pc, via)
run = g.run(frames=8000, inputs=str(script), watch=[0x08e414], out="build/f3a/scene-w")
run.log                                # frames in which each watched PC executed
```

- `until` without `watch` only sees routines that write video/control RAM.
- `watch` needs `build-<id>-instrument/`; the error prints the cmake commands if missing.
- Task routines (cooperative yield traps) run their entry once and resume inside: watch a resume point
  (`dis` annotates yields, `flow(tree=...)` lists resume points).
- `rendered.png` dumps show which screen you are on.

Finding a script is trial and error: hand it to a subagent with the game, target routines/PCs, the
grammar file path, a ~25-run budget, and the save convention above. Ask per target: reached or not,
frame, script and run paths, and when unreached the gate (branch, required RAM value) with evidence.
Buttons alone reached 2P versus/results in ~16 runs; rare gates (10 round wins, 1-in-128 draws)
needed `poke`, then 3–4 runs.

## Gotchas

- A poke run proves reachability, not that normal play gets there; say so wherever you use it.
- Pokes write main RAM only (`0x400000..0x41ffff`; `xref` prints an a5 global's absolute address). A
  range re-pokes at the start of each frame; the game can change it within the frame. Netplay rejects
  poke scripts. Any `inputs` script disables fast boot, so frame numbers are cold-boot frames.
- Dumps are taken at the end of each emulated frame.
- `field=` excludes `a5`/`a7`; use `xref("a5-0xNNNN")` for globals. Negative offsets work as
  `field="-0x10"`.
- "0x3ee6 is shared by 35 routines" means shared code; do not credit the first owner.
- `stack-arg` rows are stack-argument stores in routines without static callers, not missing coverage.
- `profiles/landmakrj.profile` (+ `.indirect`) is frozen (feeds codegen tiers): profile it with
  `out=FILE`. Other games' profiles merge new runs unless `replace=True`.
- `g.mametap(write="LO:HI", frames=N, run=True)` logs MAME accesses with PC (landmakrj ROMs only).

## Finishing

- Report each finding with address, the call, and the output line that proves it.
- File keepers per `skill://re-wiki`; the evidence is the exact calls and their output (plus the
  input script). A routine page names that routine in all later f3a output.
- Tool problems go in the report: exact call, expected, actual. Tool regressions:
  `uv run pytest tests/tools/test_f3a.py`.
