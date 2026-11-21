# f3recomp JSON API

`f3recomp` answers structured queries about a game's ROM, listings, routines, cross-references
and call flow. Every query exists as a one-shot subcommand with `--json` and as a method of
`f3recomp serve`, which keeps loaded games, discovery results and profiles in memory.

Conventions:

- Addresses are JSON integers (`0x400` is `1024`). Hit counts are integers (saturated at
  `2^63 - 1`). Hex byte strings are lowercase, two digits per byte, no separators.
- On the command line addresses are hexadecimal with an optional `0x` or `$` prefix. In `serve`
  an address parameter may be an integer or such a string. `lower --pc` keeps `Arg.int` syntax
  (decimal, or `0x` hex).
- Every query takes `--config FILE --rom-dir DIR` (`config` and `rom_dir`), except `disasm` and
  `lower` with `--hex`, and `profile-merge`. Errors print one line on stderr and exit 1.
- `--profile-tiers FILE` and `--profile-slim FILE` (`profile_tiers`, `profile_slim`; mutually
  exclusive) load a block profile. In `inspect`, `xref` and `flow` both only attach hit counts.
  The profile must name the game's main ROM CRC.
- Text output of `coverage`, `check-irq`, `decisions` and `lower` is byte-identical to before
  when `--json` is absent.

## Subcommands

### `rom-read --region R --len N [--addr A] [--format json|hex|raw]`

Regions: `rom sound sprites sprites_hi tiles tiles_hi samples eeprom`. `A` is a byte offset into
the assembled region (default 0). `hex` prints the hex string and a newline, `raw` the bytes.

```
{region: string, addr: int, length: int, size: int /* region size */, bytes: hexstring}
```

### `disasm (--config F --rom-dir D [--region rom|sound] | --hex BYTES) --pc A [--count N | --to END] [--a5 A] [--json]`

Default `--count 16`. `--to END` lists instructions starting below `END`. With `--hex` the bytes
are placed at `--pc`. Sound listings use the sound CPU's `0xc00000` address space (a `--pc` below
the region size is taken as an offset). Text lines look like
`000406:  6100 0018                bsr $000420`.

JSON: an array of lines, in order. A decoded instruction:

```
{pc, length, bytes: hexstring, ok: true, op_name: string /* Insn constructor, e.g. "Move" */,
 mnemonic: string /* "move.l" */, operands: string /* "d0,(a0)+" */, text: string,
 targets: int[] /* static transfer targets, physical addresses, sorted */,
 falls_through: bool,
 escape: null | "return" | "exception-return" | "computed" | "module-call" | "raises" | "halt",
 refs: [{role, target?: int, a5_disp?: int, size: int, indexed: bool}]}
```

An illegal or truncated word: `{pc, length, bytes, ok: false, reason, vector: int, text: "dc.w $4afc"}` with
`reason` one of `unassigned illegal-opcode a-line truncated breakpoint f-line movec-control-0xNNN`.

`refs[].role` is `read write rmw call branch lea`. `target` is present when the operand's address
is static (absolute, PC-relative, zero-base, or `disp(a5)` when `--a5` is given); `a5_disp`
whenever the operand is `disp(a5)`; `indexed` marks a run-time index added to `target`. `size`
is the access width in bytes (0 for code addresses and LEA/PEA).
The decoder does not keep the quick/immediate encoding choice, so `add`/`sub` immediates 1..8
print as `addq`/`subq`, others as `addi`/`subi`.

### `lower (--hex BYTES | --config F --rom-dir D [--region rom|sound]) --pc A [--count N] [--live-out FLAGS] [--json]`

Text output is unchanged. JSON:

```
{pc, count, live_out: string /* letters of XNZVC, "" for none */,
 instructions: [ <disasm line fields> + ({c: string} | {error: string}) ]}
```

`c` is the C statements for that instruction (what the text form prints for `--count 1`); `error`
says why it cannot be lowered (the text form aborts instead).

### `coverage --json`

```
{game: string, unsupported: {<op_name>: int}, total: int}
```

### `check-irq --json`

```
{game, interrupt_observes: string /* XNZVC letters, "" for none */,
 handlers: [{handler: int, verdict: "proven", flags: string, undeclared: bool}
          | {handler: int, verdict: "unknown", at: int, why: string}]}
```

### `decisions [--sprite-only] --json`

```
{game, sprite_only: bool,
 columns: ["pc","disposition","length","flow","reads","writes","live_out","idle","chain_candidates"],
 rows: [{pc: int, disposition: string /* native, ram, exception:N, ram-exception:N,
                                          sprite-writer, sprite-reset */,
         length: int|null, flow: string|null, reads: string|null, writes: string|null,
         live_out: string|null, idle: string|null, chain_candidates: int[]}]}
```

`null` is the TSV `-`. `flow`, `reads`, `writes` and `live_out` keep the TSV spelling
(`branch:0x000420,next`, `NZVC`). Sprite rows carry their record in `flow`.

### `lower`, `coverage`, `check-irq`, `decisions` options

Unchanged apart from `--json`; `decisions` still accepts `--sprite-writers`, `--sprite-header`
and `--sprite-stats`.

### `inspect [--profile-tiers F | --profile-slim F] [--a5 A] [--summary] [--json]`

```
{game, rom_size: int, rom_crc32: hex8, rom_sha256: hex64,
 a5: int|null, a5_info: {value: int|null, source: "explicit"|"detected"|null,
                         candidates: [{value, count}] /* most frequent first */},
 vectors: [{number: 1..63, name, raw: int /* 32-bit table entry */, target: int /* physical */,
            valid: bool /* >= 0x400, even, decodes */}],
 routine_count: int,
 summary: {by_kind: {vector, ram, config, call, indirect: int},
           live_instructions: int, decoded_instructions: int, jump_tables: int},
 routines: [Routine]            // omitted with --summary
 ram_code: [{destination, entry, length, instructions, illegal_words, exits: int[],
             callers: int[], source?: int, transform?: "negate_be32"}],
 unresolved_indirect_sites: [Site],
 profile: null | {identities, executed, cold_misses}}
```

Vector names: `reset bus-error address-error illegal zero-divide chk trapv privilege trace line-a
line-f spurious irq1..irq7 trap0..trap15`, otherwise `vN`. `number` is the table slot (reset is 1,
irq2 is 26, trap0 is 32).

```
Routine = {entry: int, name: "sub_00xxxx", kind: "vector"|"ram"|"config"|"call"|"indirect",
           kinds: string[] /* every reason, kind first */, vectors: string[] /* vector names */,
           region: "rom"|"ram", instructions: int, bytes: int,
           ranges: [[start, stop]] /* contiguous code, stop exclusive */,
           callers: int[], callees: int[] /* routine entries, sorted */,
           entry_hits?: int|null, hits?: int}   // hit fields only with a profile
Site = {pc: int, routine: int|null, text: string, hits?: int}
```

A routine is an entry plus the code reachable from it without crossing another entry. Entries
are: vector targets (`vector`), config `entry_points` (`config`), RAM code entries (`ram`), direct
`bsr`/`jsr` targets reached from live code (`call`), jump-table `jsr` candidates and, with a
profile, executed PCs nothing reaches statically (`indirect`). Live code is what is reachable from
vectors, config entries, RAM entries, jump tables and profile hits. `a5` is the most frequent
`lea`/`movea.l #imm,a5` value in live code. `hits` is the sum of per-PC hit counts inside the
routine; `entry_hits` the count at the entry PC. Unresolved sites are live `jmp`/`jsr` with a
computed operand and no jump table.

### `xref --target T [--role all|read|write|rmw|call|branch|lea] [--limit N] [--a5 A] [profile] [--json]`

`T` is one of: a hex address; `a5+OFFSET` / `a5-OFFSET` (hex, spaces and `$` allowed; resolved
through the detected or `--a5` base, and also matched against the `disp(a5)` operands
directly); `START-END` (hex, inclusive); or a region name: `rom ram palette io timer sprites
pf0 pf1 pf2 pf3 pf2-alt pf3-alt text charram lines pivot control shared sound-reset`.
A reference matches when its access range `[target, target + max(size,1))` overlaps the query. Work
RAM mirrors (`0x400000..0x43ffff`, 128 KiB) fold together. `--role write` also matches `rmw`.
Default `--limit 500`.

```
{target: string /* as given */,
 match: {kind: "address"|"range"|"a5"|"region:NAME", start?, end?, offset?, address?: int|null},
 a5: int|null, total: int, truncated: bool,
 refs: [{pc, routine: int|null, op_name, text, role, target: int|null, a5_disp: int|null,
         size: int, indexed: bool, hits?: int}]}   // ascending pc; hits only with a profile
```

### `flow --target T [--depth N] [profile] [--json]`

`T` is a hex PC, a vector name (`reset`, `irq2`, `trap5`, `vector:irq2`, `v12`), or `?dispatch`.
A PC inside a routine resolves to that routine. Default `--depth 2` (0 lists no callees).

```
{target, pc: int, entry: int, name, exact: bool /* pc is the entry */, kind, vectors: string[],
 callers: [{routine: int, site: int, kind: EdgeKind, hits?: int}],
 callees: [Callee], unresolved: [Site]}
Callee = {routine: int, site: int, kind: EdgeKind, hits?: int,
          repeated: bool /* already listed above or an ancestor */, children: [Callee]}
EdgeKind = "call" | "tail" | "table-call" | "table-jump" | "fall"
```

`unresolved` lists unresolved computed sites in the target routine and every routine shown in
the tree. For `?dispatch`:

```
{target: "?dispatch", dispatch: true,
 callers: [{routine: int|null, site: int, kind: "dispatch"}],   // every unresolved computed site
 callees: [{routine, site: null, kind: "dispatch", children: []}], // routines entered only through dispatch (needs a profile)
 unresolved: [Site]}
```

### `profile-merge --output PATH INPUT... [--json]`

Sums hits and misses with uint64 saturation and writes the merged text profile (identities sorted
by region and unsigned CRC, then hits, then misses). Duplicate inputs are rejected.

```
{output: string, inputs: int, roms: int /* identities */, ever_executed: int, cold_misses: int}
```

Text output: `merged N profiles into PATH`.

## `serve`

`f3recomp serve` reads one JSON object per line on stdin and writes one per line on stdout,
flushing after each. Blank lines are skipped.

```
request:  {"id": <any JSON>, "method": string, "params": {...}}
success:  {"id": <same>, "ok": true,  "result": <JSON>}
failure:  {"id": <same, null if unreadable>, "ok": false, "error": string}
```

A malformed line or a request without a string `method` gets a failure response; the server keeps
running until EOF. Games are cached by `(config, rom_dir)`; discovery results, analyses (by
profile file, its mtime and `a5`), coverage and `check_irq` results are cached with them.

| method | params | result |
| --- | --- | --- |
| `ping` | | `{pong: true}` |
| `version` | | `{name: "f3recomp", api: 1, methods: string[]}` |
| `rom_read` | `config rom_dir region? addr len` | as `rom-read` JSON (region defaults to `rom`) |
| `disasm` | `pc` and (`hex` or `config rom_dir region?`), `count?` (default 16) or `to?`, `a5?` | array of lines as `disasm --json` |
| `inspect` | `config rom_dir profile_tiers? profile_slim? a5? routines?: bool` | as `inspect --json`; `routines: false` is `--summary` |
| `xref` | `config rom_dir target role? limit? a5? profile_*?` | as `xref --json` |
| `flow` | `config rom_dir target depth? profile_*?` | as `flow --json` |
| `coverage` | `config rom_dir` | as `coverage --json` |
| `check_irq` | `config rom_dir` | as `check-irq --json` |
| `decisions` | `config rom_dir profile_*? sprite_only?: bool sprite_writers?` | as `decisions --json` |
| `lower` | `pc` and (`hex` or `config rom_dir region?`), `count?` (default 1), `live_out?` (default `XNZVC`) | as `lower --json` |
| `profile_merge` | `inputs: string[] output` | as `profile-merge --json` |
| `close` | `config rom_dir` (both) or none | `{closed: true}`; evicts that game, or everything |

`pc`, `addr`, `to` and `a5` accept integers or hex strings. `len`, `count`, `depth` and `limit` are
integers.
