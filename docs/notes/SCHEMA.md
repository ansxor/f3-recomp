# RE notes schema

These notes follow the `re-wiki` skill (`.omp/skills/re-wiki/SKILL.md`). This file states what is
specific to this project.

## System

Taito F3 package system:

- Main CPU: Motorola 68EC020 at 16 MHz, program ROM at `0x000000`, work RAM at `0x400000..0x41ffff`
  (mirrored to `0x43ffff`), with game globals usually addressed from `a5`.
- Video: TC0630FDP, which handles playfields PF1–PF4, text, line RAM, sprites and the palette.
- Sound: a 68000 sound CPU with an ES5505 sample player and an ES5510 DSP. It talks to the main CPU
  through shared RAM at `0xc00000`.
- Bus map and register names: [[hardware/memory-map]], `runtime/machine.cpp` and `runtime/renderer/`.

## Games

The hash is the SHA-256 of the main program image as `f3recomp inspect` reports it (`rom_sha256`,
the ROM files interleaved). The ROM file name is not a valid identity.

- landmakrj — Land Maker (Japan) — `aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc`
- commandw — Command War (prototype) — `09941ebd5dea6b128c3375560dbe3942e55bd0942e50e791c062d88a8cc5dab1`

Add a game here before writing pages about it.

## File names

- `routines/<game>-<addr>-<name>.md`. `<addr>` is the 8-digit hex address without `0x`; `<name>` is
  kebab-case. Example: `routines/landmakrj-0008e20c-round-task.md`.
- `quirks/<slug>.md`, `hardware/<component>.md`, `games/<game>.md`.

## Page frontmatter

```yaml
---
title:
created: YYYY-MM-DD
updated: YYYY-MM-DD
type: hardware | game | routine | quirk | comparison | query
tags: []
sources: [raw/...]
games: []
addresses: []            # hex, full bus width, e.g. 0x0008E20C
status: hypothesis | confirmed | refuted
evidence:                # required unless hypothesis
  - {kind: test|trace|emu-source|doc, ref: raw/..., note: ...}
contradictions: []
supersedes: []
---
```

## Evidence in this project

| Kind | What it is here | Where it goes in `raw/` |
|---|---|---|
| `test` | A run that checks a prediction: a live `f3recomp mcp` runtime session (`rt_step`, `rt_set_input`, `rt_read`, `rt_save_state`/`rt_load_state`), an instrumented game run with `--watch-entries`, the per-area runtime tests | `raw/tests/` (command, script and output) |
| `trace` | Observed execution: discovery logs, `entries.log`, frame dumps, `rt_sprites` and `rt_read` output from a live session, MAME taps | `raw/traces/` |
| `emu-source` | MAME or this runtime's device code | `raw/emu-source/` |
| `doc` | Datasheets, hardware notes, and static `f3recomp disasm`/`xref`/`flow` output (CLI or the `f3recomp mcp` tools) | `raw/disasm/` for static output, `raw/docs/` otherwise |

Static disassembly is `doc`-level evidence: it shows what the code can do, not what it does. A run
that uses `poke` lines shows that code is reachable, not that normal play reaches it; say so on the
page. Save the exact command and the `inputs.txt` with every run output stored in `raw/`.

Text copied from this repository's runtime documentation (`raw/docs/runtime-*.md`) describes the
MAME-derived device models and ROM analysis of this runtime. It is `doc` evidence about the runtime, not
about the board. Matching MAME output is compatibility evidence only, and none of it outranks a board
observation by 12 (see `.omp/rules/hardware-observation-over-mame.md`).

## raw/ frontmatter

```yaml
---
source_url:          # or the f3recomp / MCP call
ingested: YYYY-MM-DD
sha256:              # of the body below the frontmatter
game:
rom_sha256:
captured_with:       # e.g. "f3recomp mcp (git <short sha>)", "MAME 0.27x", "hardware"
---
```

## Tags

cpu, video, sprite, tilemap, text, line-ram, palette, blend, clip, audio, io, input, interrupt,
timing, vblank, reset, memory-map, pcb, game-loop, task-system, object-system, dispatch, rng,
scoring, emulator-bug, undocumented, rom-layout, eeprom, netplay, sound-driver, mailbox

`pcb` covers board wiring, chip pinouts and clocks. Third-party hardware notes ingested under
`raw/docs/taito-f3/` (12Me21/taito-f3) are `doc` evidence even where they report oscilloscope or
hardware-test observations: they are secondhand until reproduced here.

Add a tag here before using it.

## Submodule sources

`raw/docs/taito-f3/` is a git submodule (upstream 12Me21/taito-f3), so its files carry no raw
frontmatter. Its provenance lives in the sidecar `raw/docs/taito-f3.md`, whose `git_commit` replaces
the per-file `sha256`: drift means the submodule HEAD differs from `git_commit`. Never commit inside
the submodule; bumping it is a re-ingest.
