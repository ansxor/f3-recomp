---
name: f3mcp
description: "Analyse a Taito F3 game's 68EC020 program through the f3recomp MCP server (static analysis tools plus live f3rt-tool runtime sessions): find routines, globals, record layouts, video writers, frame order, dispatch targets; step, poke and screenshot a live machine to verify; file findings in docs/notes. Use for any reverse-engineering question about a game in games/<id>/."
---

# f3mcp: game analysis for Taito F3 over MCP

Static output shows what the code *can* do; a live machine shows what it *does*. A claim is finished only when a
run supports it; otherwise mark it `[INFERENCE]`.

## Setup

One MCP server covers both halves: `f3recomp mcp` (stdio). Run it from the repository root (it resolves
`games/<id>/config.toml`, `roms/<id>`, `profiles/<id>.profile` and `build/<id>/f3rt-tool` there; `--root DIR`
overrides).

```sh
nix run .#f3-mcp                      # packaged server (same as `nix run .#mcp`)
nix develop -c dune build --root recomp && recomp/_build/default/bin/main.exe mcp   # from a checkout
f3recomp mcp --game landmakrj         # also list the rt_* runtime tools from the start (see below)
```

Register it in an MCP client as a stdio server, for example `{"command": "nix", "args": ["run", ".#f3-mcp"]}` with the
repository root as the working directory.

The runtime needs a built `build/<id>/f3rt-tool` (`nix run .#f3-compile -- <id>` or
`nix develop -c cmake --build build/<id> --target f3rt-tool`).

## Static tools

`rom_read`, `disasm`, `inspect`, `xref`, `flow`, `coverage`, `check_irq`, `decisions`, `lower`, `profile_merge`.
Pass `game: "<id>"` (or explicit `config` + `rom_dir`). Results are the JSON of `recomp/JSON-API.md` in
`structuredContent` (snake_case fields; addresses are integers; address arguments accept integers or `"0x400"` /
`"$400"` strings). `inspect`, `xref`, `flow` and `decisions` attach hit counts from `profiles/<id>.profile` when it
exists (`no_profile: true` drops it). Without a profile there are no counts and `?dispatch` is weaker.

`f3recomp` names routines `sub_00xxxx`; the note labels (`routines/<game>-<8hex>-<name>.md`) are not applied to its
output, so map them by address yourself.

## Live runtime: the session lifecycle

1. `session_open {game}` spawns `build/<game>/f3rt-tool mcp`, boots the game and **dynamically registers the runtime's
   tools as `rt_<name>`**: `rt_step`, `rt_regs`, `rt_read`, `rt_write`, `rt_set_input`, `rt_screenshot`, `rt_sprites`,
   `rt_save_state`, `rt_load_state`, `rt_crc`, `rt_audio_stats`, `rt_reset`, `rt_video_diff`, `rt_init`, `rt_shutdown`.
   The server then sends `notifications/tools/list_changed`: **refresh your tool list** (re-run `tools/list`) after
   `session_open` and after `session_close`. A client that ignores `list_changed` should start the server with
   `--game <id>` so the `rt_*` tools are listed from the start (calls fail with a clear message until a session is open).
2. Every `rt_*` tool takes an optional `session_id` (default: the most recently opened session; `session_list` shows
   them all). `session_open` options: `rom_dir`, `set`, `game_video`, `sprite_writers` (record writer PCs per sprite
   slot), `video_diff` (FDP oracle for `rt_video_diff`), `eeprom`, `tool_path`, `session_id`.
3. `session_close` ends the process and frees the machine. Close sessions when done.
4. **Stale binaries.** Opening a session records the binary's mtime, inode and size. If `build/<id>/f3rt-tool` is rebuilt,
   replaced or removed while the session is open, every `rt_*` call returns an error (`... changed on disk ...`)
   instead of running old code. Recover with `session_close` and `session_open`. `session_list` shows `fresh`, `stale`
   or `missing` per session.

`rt_read` / `rt_write` accept `a5+0x...` / `a5-0x...` addresses, resolved with the game's inferred a5 (the `a5` field
of `inspect`). `rt_screenshot` returns a native `image/png` content block (and `path` also writes a file); look at it.

The same runtime also runs standalone: `build/<id>/f3rt-tool mcp [--rom-dir DIR] [--no-init]` is an MCP server by
itself (tools without the `rt_` prefix, `init` first when started with `--no-init`).

One-shot runs without a session: `gameplay`, `sprite_check`, `motion` (`f3rt-tool <cmd> --json`, fresh process per
call; `gameplay` defaults to 40000 frames and can take minutes; extra flags go in `args`).

## Start

1. Orient in `docs/notes/` per `skill://re-wiki`, then read `games/<id>.md` and grep your addresses.
2. Map the frame: `inspect {game, routines: false}` for vectors, then `flow {game, target: "irq2", depth: 2}` (frame
   interrupt, callees, hit counts). An interrupt that only syncs is not the frame worker; compare hit counts across `irqN`.

## Recipes

**Meaning of a global**: `xref {game, target: "a5-0x60ac"}` lists every read/write/bit test with its routine;
`disasm {game, pc: <writer pc - 0x20>, count: 24}` shows the context; then confirm in a session:
`rt_step {frames: 1200}`, `rt_read {addr: "a5-0x60ac", size: 2}`, step more, read again.

**Record layout**: `xref {target: "0x410000-0x4107ff", role: "write"}` for the writers of an array;
`rt_read {region: "ram", offset: 0x0000, length: 0x800}` for the raw records in one frame.

**Who writes this video RAM?** `xref {target: "sprites", role: "write", limit: 5000}` (also `control`, `pf0`,
`0x660000-0x66001f`). Static references cannot place every computed pointer (`indexed`, `target: null`); confirm a
candidate in a session opened with `sprite_writers: true`: `rt_sprites` returns each entry's `writer_pc`.

**Computed jumps, hooks, interpreters**: `flow {target: "?dispatch"}` lists unresolved sites; callees need a profile.

**Sprites**: `rt_sprites {}` walks the list as the chip does (entries, bank switches, jumps, end slot). `drawn_only:
false` lists every visited entry; `raw: true` adds the 64 KiB sprite RAM as hex.

**Checkpoint and replay**: `rt_save_state {slot: "before"}`, `rt_set_input {system_inputs: 0xfb}` (service/test/coin byte,
active low) or `{port, mask, pressed}` for buttons, `rt_step {frames: 30}`, `rt_load_state {slot: "before"}`.
`state_crc` reproduces after `rt_load_state`, so a diverging CRC means the input or code path differed. Inputs are
not part of a state: reset them yourself after loading.

## Gotchas

- `rt_regs` and region reads do not change `state_crc`; `rt_write` does (it is a real bus write, hardware side effects
  apply). Bus reads of I/O addresses have the bus's side effects.
- Checkpoint replays are not byte-identical in audio (the synth is not in the saved state); use a fresh session to
  compare audio.
- `rt_init` rebuilds the machine and clears the slots; `rt_reset` keeps the audio clock.
- `rt_video_diff` needs `session_open {video_diff: true}` and is meaningful from about frame 600.
- Attract mode never presses buttons; drive input with `rt_set_input` between `rt_step` calls. A poke run proves
  reachability, not that normal play gets there; say so wherever you use it. Pokes write main RAM
  (`0x400000..0x41ffff`; `xref("a5-0xNNNN").match.address` is the absolute address).
- Headless runs of the game binary with an input script (grammar in the header comment of
  `runtime/debug/input_script.hpp`) remain available on the command line; the MCP server does not wrap them.
- "0x3ee6 is shared by 35 routines" means shared code; do not credit the first owner.
- `profiles/landmakrj.profile` is frozen (feeds codegen tiers): do not overwrite it; write new profiles elsewhere
  and combine with `profile_merge`.

## Finishing

- Report each finding with address, the call, and the output that proves it.
- File keepers per `skill://re-wiki`; the evidence is the exact calls and their output.
- Tool problems go in the report: exact call, expected, actual. Regressions:
  `nix develop -c dune test --root recomp --force test/api` (MCP end-to-end tests).
