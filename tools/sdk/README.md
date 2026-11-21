# f3 code-mode SDK

TypeScript over `f3recomp` (static analysis, JSON API in `recomp/JSON-API.md`) and the game binary /
`f3rt-tool` (headless runs). No dependencies, no `node_modules`, no build step: plain TypeScript that
runs under Node 22+ (type stripping), Bun, Deno and Codex JS code-mode `exec`.

```ts
import { openGame } from "./tools/sdk/index.ts";   // from the repository root

const g = await openGame("commandw");               // starts one `f3recomp serve`; later calls reuse its caches
const irq2 = await g.flow("irq2", { depth: 2 });    // callees with hit counts from profiles/commandw.profile
const writers = (await g.xref("sprites", { role: "write" })).refs;
console.log(await g.disasm(writers[0].pc - 8, 6));
await g.close();                                    // optional: idle clients never keep the process alive
```

`openGame(id, opts?)` resolves `games/<id>/config.toml`, the ROM directory (`opts.romDir`, `$F3_ROM_DIR`,
then `roms/<id>`) and `profiles/<id>.profile` (`profile: false` for none, a path, or `{ slim }`). The
server is `recomp/_build/default/bin/main.exe` (build with `dune build --root recomp`), `$F3_RECOMP_EXECUTABLE`,
`opts.f3recompPath`, or `f3recomp` on `PATH`. Share one server across games with `new RecompClient()` and
`client.game(id)` / `openGame(id, { client })`.

## API

Addresses are numbers or hex strings (`0x400`, `"$400"`). Result types are in `types.ts` and mirror the JSON API field names.

`Game` (static, through `f3recomp serve`):
`readRom(region, addr, len)` (a `Uint8Array` with `.region/.addr/.regionSize`), `words(addr, count, {signed, region})`,
`longs(addr, count, {region})`, `disasm(pc | {pc, count, to, region: "rom"|"sound", a5, hex}, count?)`,
`inspect({summary, a5, profile})`, `vectors()`, `routines()`, `ramCode()`, `unresolvedIndirect()`, `a5()`,
`xref(target, {role, limit, a5, profile})` (target: address, `"a5-0x60ac"`, `"START-END"` or a region such as `"sprites"`, `"control"`, `"pf0"`),
`flow(target, {depth, profile})` (target: PC, `"irq2"`/`"reset"`/`"trap5"`, `"?dispatch"`),
`coverage()`, `checkIrq()`, `decisions({spriteOnly, spriteWriters, profile})` (the unfiltered table is ~500k rows and takes tens of seconds),
`lower({pc, count, liveOut, region, hex})`.

`Game` (runtime):

- `run({frames, inputs, dumpStart, dumpEvery, keep, wav, out, seed, exe, args, mode: "game" | "gameplay"})` runs `build/<id>/<id>`
  headless (or `f3rt-tool gameplay --json`) and returns a `RunHandle`:
  `ok, exitCode, frameCrc, stateCrc, framesRun, audioFrames, wavPath, dumpDir, dir, output, summary`, plus dump readers
  `frames()`, `mem(frame, addr, size)`, `b/w/l(frame, addr)`, `series(addr, size, frames?)`, `table(base, stride, count, frame, size?)` (records with `.b/.w/.l(offset)`),
  `sprites(frame)` (walks the sprite list like the chip), `framePath(frame, file)`. Addresses may be `"a5-0x60ac"`.
  The run directory (`build/sdk/<id>-<frames>-<pid>` by default) keeps `run.txt`, `inputs.txt`, `dumps/`; reopen it with `openRun(dir, gameId?, {game | a5})`.
  Dumping flushes the CPU's lazy flags, so `stateCrc` of a dumping run differs from a non-dumping run of the same game.
- `spriteCheck({frames, compareEvery, seed, noInputs, behaviours})`, `motionProof({frames, seed, every, scale, interp, spriteGroups})`:
  `f3rt-tool ... --json` objects (their field names) plus `ok` and `exitCode`.

Standalone: `disasmHex(pc, hex, count?)`, `lowerHex(pc, hex, {count, liveOut})`, `mergeProfiles(output, inputs)`
(on a shared lazily started client; `closeDefaultRecompClient()`), `compareWav(ref, cand, {startSec, endSec, maxLagSamples, lag})`
(`lagSamples > 0`: candidate lags the reference), `compareImage(a, b, {tolerance, alpha, diffPath})` (PNG 8-bit and BMP 24/32-bit),
`decodePng/decodeBmp/encodeBmp/readWav/encodeWav`, `decodeSpriteList(spriteRam)`, `runDifferential({cases, seed, filter, output})`
(runs `uv run python -m tools.differential`, parses `differential_accounting.json`).

## Quick starts

Node (22.18+/23.6+ strip types by default; older 22.x needs `--experimental-strip-types`):

```sh
node --input-type=module -e 'import { openGame } from "./tools/sdk/index.ts"; const g = await openGame("landmakrj");
  console.log((await g.inspect({ summary: true })).routine_count)'
node my-analysis.ts
```

Bun / Deno:

```sh
bun -e 'import { openGame } from "./tools/sdk/index.ts"; const g = await openGame("commandw"); console.log(await g.vectors())'
deno run -A my-analysis.ts
```

Codex `exec` (JS/TS code mode, cwd = repository root):

```ts
const { openGame, compareWav } = await import(`${process.cwd()}/tools/sdk/index.ts`);
const g = await openGame("commandw");
const run = await g.run({ frames: 2000, dumpStart: 1000, dumpEvery: 1, keep: "mainram.bin", out: "build/sdk/g60ac" });
console.log(run.series("a5-0x60ac", 2).filter(([, v]) => v));
```

## Tests

```sh
node --test tools/sdk/sdk.test.ts
bun test tools/sdk/sdk.test.ts
```

The synthetic suites (WAV, PNG/BMP, dump reader, sprite list, differential stand-in) need nothing; the `f3recomp serve`
tests need the built `f3recomp`; the ROM sections run when `roms/commandw` / `roms/landmakrj` exist, and the runtime ones also need
`build/commandw/commandw` / `build/commandw/f3rt-tool`.
