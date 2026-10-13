---
name: f3a
description: "Analyse a Taito F3 game's 68EC020 program with f3a (`uv run f3 analyze` CLI, plus small Python scripts importing tools.analysis.f3a): find routines, globals, record layouts, video writers, frame order, dispatch targets; run the game headless with scripted input, RAM pokes and execution watches to verify; file findings in docs/notes. Use for any reverse-engineering question about a game in games/<id>/."
---

# f3a: game analysis for Taito F3

Static output shows what the code *can* do; headless runs show what it *does*. A claim is finished
only when a run supports it; otherwise mark it `[INFERENCE]`.

## Setup

**CLI for lookups**: `uv run f3 analyze <cmd> ... --game <id>` (or `export F3A_GAME=<id>`; `f3a use`
only works in an interactive terminal). `uv run f3 analyze <cmd> --help` lists options. Rows are
capped by `--limit`: pass `--limit 0` for all, `--json` for one object per row.

**Scripts for checks across many rows or frames**: when a guess must hold over every record, frame or
writer, write a short script under `build/f3a/scripts/` and run it from the repo root with
`uv run python build/f3a/scripts/<topic>.py`. Each script re-analyses the game (~1 s), so put related
checks in one script and print only the verdict lines.

```python
from tools.analysis import f3a
g = f3a.game("landmakrj")
run = f3a.open_run("build/f3a/g60ac")              # a directory made by `f3 analyze run --out`
nonzero = [(n, v) for n, v in run.series("a5-0x60ac", 2) if v]
print(f"{len(nonzero)}/{len(run.frames)} frames non-zero; first {nonzero[:5]}")
```

Library API (methods take the CLI's positional args, options as keywords: `dump_every=1`, `from_=`
for `--from`, `True` for flags, ints become hex):

```python
def game(game_id, rom_dir=None, profile=None, symbols=None, a5=None) -> Game: ...
def open_run(path, game_id=None) -> Run: ...
class F3AError(Exception): ...                    # every tool error

class Game:
    id: str
    core: analysis.game.Game                      # underlying analysis, for what methods don't expose
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

1. Orient in `docs/notes/` per `.omp/skills/re-wiki/SKILL.md`, then read `games/<id>.md` and grep
   your addresses. f3a labels come from those notes (`routines/<game>-<8hex>-<name>.md`, overridden by
   `games/<id>/analysis/symbols.toml`); a trailing `?` (`round_task?`) means the page is a hypothesis.
   If the game's code settles a page's `contradictions`, that is a finding: report it with the run.
2. Map the frame: `vectors`, then `flow irq2 --tree 2` (frame interrupt, callees, hit counts). An
   interrupt that only syncs is not the frame worker; compare hit counts across `irqN`.

## Evidence columns

`xN` executed N times in `profiles/<game>.profile`; `rooted` statically reachable, never seen
executing; `decoded?` may not be code. Without a profile there are no counts and `?dispatch` hints are
weaker: `uv run f3 analyze profile --build` first.

## Recipes

Commands below omit `uv run f3 analyze` and `--game <id>`. Quote names containing `?`.

**Meaning of a global**
```sh
xref a5-0x60ac --sort hits                        # every read/write/bit test + routine
dis 'round_task?' --from 0x8e3f4 --to 0x8e434     # around the hottest writer
run --frames 2000 --dump-start 1000 --dump-every 1 --keep mainram.bin --out build/f3a/g60ac
```
Then confirm the meaning over every dumped frame with the `series` script in Setup.

**Record layout**
```sh
struct 'sprite_list_writers?' a0 --depth 2        # offset, size, reads, writes, rmw, bits, pcs
xref --field 0x1.7 --role write                   # who writes byte +1 bit 7 of any record
xref --field 0x1.7 --role write --within 0x410000-0x4107ff   # one array, + immediate stores
records build/f3a/g60ac --base 0x410000 --stride 0x100 --count 8 --active 0.7 \
  --check '+0x2.w == +0x2a.w + 4'                 # "holds for N/M active records": report N/M
```
For checks `--check` cannot express, script it over `run.table(base, stride, count, frame=n)` for
every `n in run.frames` and print the mismatch count.

**Who writes this video RAM?**
```sh
writes --region sprites --ranges                  # static; or --region 0x660000-0x66001f
run --frames 3000 --all-writers --out build/f3a/writers
discover build/f3a/writers --ranges --unknown-only   # observed PCs video.cpp does not list yet
```
Static and observed disagree when the static pass cannot place a pointer (`unknown`/`mixed`/`?`) or
the writer only runs in gameplay (next section). To triage discovery-log `NEW` lines or extend
`video_writer_known` / `[[video.emit_units]]`, read `references/discovery-log.md`.

**Computed jumps, hooks, interpreters**
```sh
flow '?dispatch'                                  # unresolved sites; slot candidates over-approximate
run --frames 3000 --indirect --out build/f3a/ind
flow '?dispatch' --observed build/f3a/ind         # "(not a static candidate)" = static pass missed it
xref --field 0x10 --role write --code             # who stores code addresses into slot +0x10
dis 0x2ff42 --data offsets                        # decode a jmp table(pc,d0.w) table
```
Confirm live targets with `--watch` (below).

**Sprites**
```sh
run --frames 1210 --dump-start 1200 --dump-every 1 --keep mainram.bin,graphics.bin --out build/f3a/spr
sprites build/f3a/spr --frames 1200:1210 --writer 'sprite_list_writers?'   # needs graphics.bin
```

## Reaching code attract mode never runs

Headless runs stay in attract mode unless the run presses buttons. Script grammar: header comment of
`runtime/debug/input_script.hpp`; read it, do not restate it from memory. Script frames are 1-based,
same as dumps and logs. Save input scripts as `build/f3a/<game>-inputs/<scene>.txt`, first line a
comment saying what they reach and at which frame (fill in once a run shows it):

```text
# <scene>: <target> reached at frame <N>
700+20 coin
1200-end mash seed=3
2000-2100 p2 right+b1
3000-end poke 0x401f54.w=10
```

```sh
run --frames 8000 --inputs build/f3a/landmakrj-inputs/<scene>.txt --until sub_09d72a \
  --dump-every 100 --keep rendered.png,mainram.bin --out build/f3a/scene   # prints reached / NOT reached
run --frames 8000 --inputs build/f3a/landmakrj-inputs/<scene>.txt --watch 0x08e414 \
  --out build/f3a/scene-w                         # frames in which each watched PC executed
```

- `--until` without `--watch` only sees routines that write video/control RAM.
- `--watch` needs `build-<id>-instrument/`; the error prints the cmake commands if missing.
- Task routines (cooperative yield traps) run their entry once and resume inside: watch a resume point
  (`dis` annotates yields, `flow --tree` lists resume points).
- `rendered.png` dumps show which screen you are on.

Finding a script is trial and error: hand it to a subagent with the game, target routines/PCs, the
grammar file path, a ~25-run budget, and the save convention above. Ask per target: reached or not,
frame, script and run paths, and when unreached the gate (branch, required RAM value) with evidence.
Buttons alone reached 2P versus/results in ~16 runs; rare gates (10 round wins, 1-in-128 draws)
needed `poke`, then 3–4 runs.

## Gotchas

- Output stops at `--limit` rows with `… N more rows`; counting or filtering on truncated output is
  wrong. Use `--limit 0`, or a script.
- A poke run proves reachability, not that normal play gets there; say so wherever you use it.
- Pokes write main RAM only (`0x400000..0x41ffff`; `xref` prints an a5 global's absolute address). A
  range re-pokes at the start of each frame; the game can change it within the frame. Netplay rejects
  poke scripts. Any `--inputs` script disables fast boot, so frame numbers are cold-boot frames.
- Dumps are taken at the end of each emulated frame.
- `--field` excludes `a5`/`a7`; use `xref a5-0xNNNN` for globals. Write negative offsets as
  `--field=-0x10`.
- "0x3ee6 is shared by 35 routines" means shared code; do not credit the first owner.
- `stack-arg` rows are stack-argument stores in routines without static callers, not missing coverage.
- `profiles/landmakrj.profile` (+ `.indirect`) is frozen (feeds codegen tiers): profile it with
  `--out FILE`. Other games' profiles merge new runs unless `--replace`.
- `mametap --write LO:HI --frames N --run` logs MAME accesses with PC (landmakrj ROMs only).

## Finishing

- Report each finding with address, the command or script, and the output line that proves it.
- File keepers per `.omp/skills/re-wiki/SKILL.md`; the evidence is the exact commands/scripts and
  their output (plus the input script). A routine page names that routine in all later f3a output.
- Tool problems go in the report: exact command, expected, actual. Tool regressions:
  `uv run pytest tests/tools/test_f3a.py`.
