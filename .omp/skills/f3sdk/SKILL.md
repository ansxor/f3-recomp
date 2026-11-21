---
name: f3sdk
description: "Analyse a Taito F3 game's 68EC020 program with the TypeScript code-mode SDK (tools/sdk, over f3recomp and the game binary): find routines, globals, record layouts, video writers, frame order, dispatch targets; run the game headless with scripted input and frame dumps to verify; compare WAV/PNG captures; file findings in docs/notes. Use for any reverse-engineering question about a game in games/<id>/."
---

# f3sdk: game analysis for Taito F3

Static output shows what the code *can* do; headless runs show what it *does*. A claim is finished
only when a run supports it; otherwise mark it `[INFERENCE]`.

## Setup

Run TypeScript from the repository root with Node (22.18+/26), Bun or Deno; `tools/sdk/README.md` has the
full API. No install step. `f3recomp` must be built (`dune build --root recomp`); runs need
`build/<id>/<id>` (and `build/<id>/f3rt-tool` for `mode: "gameplay"`, `spriteCheck`, `motionProof`).

```ts
import { openGame } from "./tools/sdk/index.ts";
const g = await openGame("landmakrj");   // one f3recomp server; analysis is cached between calls
```

Results are the JSON of `recomp/JSON-API.md` (snake_case fields; addresses are numbers). Helpers: `g.readRom`,
`g.words`, `g.longs`, `g.disasm`, `g.inspect`, `g.vectors`, `g.routines`, `g.ramCode`, `g.unresolvedIndirect`,
`g.xref`, `g.flow`, `g.lower`, `g.decisions`, `g.coverage`, `g.checkIrq`, `g.run`, `openRun`,
`compareWav`, `compareImage`.

## Start

1. Orient in `docs/notes/` per `skill://re-wiki`, then read `games/<id>.md` and grep your addresses.
   `f3recomp` names routines `sub_00xxxx`; the note labels (`routines/<game>-<8hex>-<name>.md`) are not applied
   to its output, so map them by address yourself.
2. Map the frame: `await g.vectors()`, then `await g.flow("irq2", { depth: 2 })` (frame interrupt, callees,
   hit counts). An interrupt that only syncs is not the frame worker; compare hit counts across `irqN`.

## Evidence

`hits` / `entry_hits` come from `profiles/<game>.profile` (default when it exists; `profile: false` to drop).
Routine `kinds` say why code is believed live (`vector`, `config`, `ram`, `call`, `indirect`). Without a profile
there are no counts and `?dispatch` is weaker.

## Recipes

**Meaning of a global**
```ts
const r = await g.xref("a5-0x60ac");                       // every read/write/bit test + routine
const writers = r.refs.filter((x) => x.role === "write" || x.role === "rmw");
console.log(await g.disasm(writers[0].pc - 0x20, 24));
const run = await g.run({ frames: 2000, dumpStart: 1000, dumpEvery: 1, keep: "mainram.bin", out: "build/sdk/g60ac" });
run.series("a5-0x60ac", 2).filter(([, v]) => v);          // confirm the meaning against a run
```

**Record layout**
```ts
(await g.xref("0x410000-0x4107ff", { role: "write" })).refs;   // who writes an array
const rows = run.table(0x410000, 0x100, 8, 1200);              // raw records in one frame
rows.map((r) => r.w(0x2));                                      // check an invariant over rows/frames yourself
```

**Who writes this video RAM?**
```ts
(await g.xref("sprites", { role: "write", limit: 5000 })).refs;      // static; also "control", "pf0", "0x660000-0x66001f"
```
Static references cannot place every computed pointer (`indexed`, `target: null`); confirm a candidate with a run.

**Computed jumps, hooks, interpreters**
```ts
const sites = await g.flow("?dispatch");         // unresolved sites; callees need a profile
await g.unresolvedIndirect();
```
Confirm live targets with an instrumented build: `g.run({ exe: "build/<id>-instrument/<id>", args: ["--profile-out", f, "--watch-log", log, "--watch-entries", "8e414,9b32"] })`
(see `--indirect-log` for computed-jump targets).

**Sprites**
```ts
const sr = await g.run({ frames: 1210, dumpStart: 1200, dumpEvery: 1, keep: "mainram.bin,graphics.bin", out: "build/sdk/spr" });
sr.sprites(1205).entries.filter((e) => e.drawn);   // walks the list as the chip does
```

**Audio / video checks**: `compareWav(ref, cand)` (lag, SNR, correlation), `compareImage(a, b, { diffPath })`,
`g.spriteCheck()`, `g.motionProof()`.

## Reaching code attract mode never runs

Headless runs stay in attract mode unless the run presses buttons. Script grammar: header comment of
`runtime/debug/input_script.hpp`; read it, do not restate it from memory. Script frames are 1-based,
same as dumps and logs.

```ts
import { mkdirSync, writeFileSync } from "node:fs";
mkdirSync("build/f3a/landmakrj-inputs", { recursive: true });
writeFileSync("build/f3a/landmakrj-inputs/<scene>.txt",
  "# <scene>: <target> reached at frame <N>\n700+20 coin\n1200-end mash seed=3\n2000-2100 p2 right+b1\n3000-end poke 0x401f54.w=10\n");
const run = await g.run({ frames: 8000, inputs: "build/f3a/landmakrj-inputs/<scene>.txt", dumpEvery: 100, keep: "rendered.bmp,mainram.bin", out: "build/sdk/scene" });
```

- Dump `rendered.bmp` shows which screen you are on (`compareImage` reads it; view it with an image tool).
- Task routines (cooperative yield traps) run their entry once and resume inside: watch a resume point.

Finding a script is trial and error: hand it to a subagent with the game, target routines/PCs, the
grammar file path, a ~25-run budget, and the save convention above. Ask per target: reached or not,
frame, script and run paths, and when unreached the gate (branch, required RAM value) with evidence.

## Gotchas

- A poke run proves reachability, not that normal play gets there; say so wherever you use it.
- Pokes write main RAM only (`0x400000..0x41ffff`; `xref("a5-0xNNNN").match.address` is the absolute address). A
  range re-pokes at the start of each frame; the game can change it within the frame. Netplay rejects
  poke scripts.
- Dumps are taken at the end of each emulated frame. Dumping flushes lazy CPU flags, so `stateCrc` only compares
  between dumping runs.
- "0x3ee6 is shared by 35 routines" means shared code; do not credit the first owner.
- `profiles/landmakrj.profile` is frozen (feeds codegen tiers): do not overwrite it; write new profiles elsewhere
  and `mergeProfiles(out, [a, b])`.
- Removed with the old Python `f3a`: `writes`, `struct`, `graph`, `mametap`, `records --check` and note-derived
  labels. Use `xref`/`flow`/`run.table` and your own checks.

## Finishing

- Report each finding with address, the call, and the output that proves it.
- File keepers per `skill://re-wiki`; the evidence is the exact calls and their output (plus the input script).
- Tool problems go in the report: exact call, expected, actual. Tool regressions: `node --test tools/sdk/sdk.test.ts`.
