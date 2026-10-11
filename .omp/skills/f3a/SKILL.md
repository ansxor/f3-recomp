---
name: f3a
description: "Analyse a Taito F3 game's 68EC020 program with uv run f3 analyze (CLI or Python library): find routines, globals, record layouts, video writers, frame order, dispatch targets; run the game headless with scripted input, RAM pokes and execution watches to verify; file findings in docs/notes. Use for any reverse-engineering question about a game in games/<id>/."
---

# f3a: game analysis for Taito F3

`uv run f3 analyze` gives static answers about the game's program, which come from the recompiler's own
discovery, and checks them with headless runs of the real runtime. Static output shows what the code
can do. Runs show what it does. A claim is not finished until a run supports it, or it is marked
`[INFERENCE]`.

Reference: `docs/developer/WORKFLOWS.md`, sections "Game analysis (`uv run f3 analyze`)" and "Scripted input".
Shared findings: `docs/notes/`, through the `re-wiki` skill (`skill://re-wiki`).

## Start

1. Read `docs/notes/index.md` and the game's page `docs/notes/games/<id>.md`, then grep `docs/notes`
   for your addresses. Labels like `round_task?` in f3a output come from those notes. A trailing `?`
   means the note is still a hypothesis.
   The hardware side is already written up; use it before guessing what a store means.
   - `hardware/` has one page per component: `memory-map`, `fdp`, `sprites`, `tilemaps`,
     `line-ram`, `line-ram-registers`, `priority-and-blend`, `shadow-mode`, `interrupts`,
     `video-timing`, `sound` and more.
   - `quirks/` has single testable behaviours, and `comparisons/` compares the hardware with MAME.
   - The source documents are in `raw/docs/taito-f3/` and `raw/emu-source/`.
   - Check a page's `status` and `contradictions` before relying on it. When the game's code settles
     an open contradiction (for example, the value it writes and what then shows on screen), report
     that with the run as evidence. It is a finding in its own right.
2. Pick the interface.
   - **CLI** (`uv run f3 analyze <cmd> --game <id>`) for one-off lookups. Shells without a terminal must pass
     `--game` or set `F3A_GAME`; `f3a use` only works in an interactive terminal.
   - **Library** for checking a guess across many frames or rows:
     ```python
     from tools.analysis import f3a    # run with `uv run python` from repo root
     g = f3a.game("landmakrj")          # analysed once per kernel
     help(g.xref)                       # each method's docstring is the CLI help
     ```
     Every command is a method taking positional arguments plus long options as keywords (`from_=` for
     `--from`). Results are lists of rows with raw fields (`.where(...)`, `.column(...)`). Printing
     shows at most 60 lines; use `.text` or `.show(n)` for everything. Errors raise `F3AError`.
   - In round 4, the library arm found more and better-verified facts. The CLI arm was faster on short
     questions.
3. Get your bearings: `vectors`, then `flow irq2 --tree 2` (the frame interrupt and what it calls,
   with hit counts).

## Evidence columns

- `xN`: executed N times in `profiles/<game>.profile`.
- `rooted`: statically reachable, never seen executing.
- `decoded?`: may not be code at all.

Without a profile there are no `xN` counts and `?dispatch` hints are weaker. Make one first with
`f3a profile --game <id> --build`.

## Recipes

**What does a global mean?**
- `xref a5-0x60ac --sort hits` lists every read, write and bit test, with the routine doing it.
- `dis <routine> --from A --to B` shows the code around the hottest writer.
- Check the meaning against runs: `run(...).series("a5-0x60ac", 2)`.

**Record or structure layout**
- `struct ROUTINE REG [--depth N]` lists accessed fields (offset, size, reads, writes, rmw, bits).
- `xref --field 0x1.7 --role write` finds who writes byte +1 bit 7 of any record.
- `--within START-END` limits it to one array and also lists immediate stores.
- From dumps: `records DIR --base B --stride S --count N --active 0.7`.
- Test a guess on every record of every frame: `--check '+0x2.w == +0x2a.w + 4'`. Report the
  mismatch count, not "it looks right".

**Frame order.** Use `flow irqN --tree 2` for each interrupt. An interrupt that only syncs is not the
one doing the frame work; check the hit counts.

**Who writes this video RAM?**
- Static: `writes --region sprites --ranges`, or `--region 0x660000-0x66001f`. Stack-argument stores
  in routines with no static callers are reported as `stack-arg` (`--unresolved` / `--ranges` note).
- Observed: `run --all-writers --out D`, then `discover D --ranges`. Filter with `--unknown-only`
  to highlight video.cpp coverage gaps.
- The two disagree when the static pass cannot place a pointer (`unknown`/`mixed` rows) or when a
  writer runs only in gameplay (see "Reaching code" below).

**Computed jumps, hooks and interpreters**
- `flow '?dispatch'` lists unresolved sites with their candidate sets. Slot candidates
  over-approximate.
- Observed targets: record with `run --indirect` (or `profile`), check with `flow '?dispatch'
  --observed DIR` (auto-loads `profiles/<game>.indirect`). Targets marked `(not a static candidate)`
  mean real runtime targets the static pass missed.
- `xref --field 0xN --role write --code` shows who stores code addresses into a slot.
- `dis TABLE --data offsets` decodes a `jmp table(pc,d0.w)` table.
- Confirm live targets with `--watch` on the candidates (below).

**Sprites**: `sprites DIR --frames A:B` walks the hardware list as the chip does; shows which routine drew each sprite (`← 0xPC (routine)`) with optional `--writer ROUTINE|PC` filter.

## Reaching code that attract mode never runs

Headless runs show attract mode unless the run presses buttons.

- `run --inputs SCRIPT` presses buttons from a script. Rule lines are `700+20 coin`,
  `1200-end mash seed=3`, `2000-2100 p2 right+b1` or `3000-end poke 0x401f54.w=10`. Frames are
  1-based, the same numbers as dumps and logs.
- `--until sub_a,sub_b` stops once the targets have run. Without `--watch` it only sees routines that
  write video or control RAM.
- `--watch PC,...` logs the frames in which any instruction, such as a branch target, executes. It
  uses the instrumented build (`build-<id>-instrument/`), and f3a prints the cmake commands if that
  build is missing.
- Task routines (`task` kind, e.g. cooperative yield traps) run their entry once and resume inside;
  `dis` annotates yield traps and `flow --tree` lists resume points. Watch a resume point instead of
  the entry (`run --watch` on an entry that ran in ≤1 frame suggests resume points).
- `--dump-every N --keep rendered.png,mainram.bin` gives screenshots, so you can see which screen you
  are on.

**Hand the search to a small-model subagent.** Finding a script is trial and error. Give the helper
the game, the target routines or branch PCs, the script format above, a run budget (about 25 runs),
and the instruction to save working scripts as `build/f3a/<game>-inputs/<scene>.txt` with a comment
line saying what they reach and when. Ask for a report per target: reached or not, the frame, the
script path and run directory, and, when unreached, the gate it found (the branch, the RAM value it
needs) with evidence. In practice:
- 2P versus and results screens took about 16 runs with buttons alone.
- Rare gates (10 round wins, a 1-in-128 random draw) needed `poke`, and then took 3–4 runs.

A poke run shows the code is reachable, not that normal play reaches it. Say so wherever you use it.

## Traps

- Dumps are taken at the end of each emulated frame.
- `--field` excludes `a5`/`a7`; a5 globals go through `xref a5-0xNNNN`. Write negative offsets as
  `--field=-0x10`.
- A header like "0x3ee6 is shared by 35 routines" means the code is shared; do not credit the first
  owner.
- `writes` rows marked `?` or `unknown` are stores the static pass could not place. Confirm them with
  `--all-writers`.
- Stack-argument stores in routines with no callers show up as `stack-arg` (not missing coverage of
  executed code).

## Finishing

- Report each finding with its address, the command, and the output line that proves it. Mark
  anything unseen `[INFERENCE]`.
- File findings that are worth keeping, following `skill://re-wiki`, in `docs/notes/`:
  - save the exact f3a commands and outputs (plus `inputs.txt`) under `raw/tests/`, `raw/traces/` or
    `raw/disasm/` (evidence kinds: `docs/notes/SCHEMA.md`);
  - a routine page `routines/<game>-<8hex>-<name>.md` names that routine in every later f3a run;
  - new pages are `hypothesis`, and only the user confirms;
  - update `index.md` and `log.md`.
- Tool problems (wrong output, missing feature) go in your report with the exact command, what you
  expected and what you got.
