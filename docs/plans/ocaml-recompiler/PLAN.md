# OCaml recompiler: redesign plan

Status: proposal for review. Nothing here is implemented. Models that checked
the design live in `models/` (see `models/README.md` for commands and results).

## 0. Why, and what not to repeat

The Python recompiler (`recomp/*.py`, ~6,500 lines) works. Its problems are
shape, not results:

- Every pass re-interprets Capstone output through mnemonic strings
  (`insn.mnemonic.split(".")[0]`) and then patches Capstone's mistakes by
  reading `insn.bytes` (full-format extension words, PC-relative bases, UNPK
  labelled SBCD, CMPM operands, DIVUL's missing Dr, F-line mnemonics).
- Flag effects are a hand table in `liveness.py` that must agree with what
  `emitter.lower` emits; a test checks the two agree after the fact.
- C is built by string concatenation; `generate.py` greps its own output
  (`"cpu->cc_op = F3_CC_OP_" in statement`) to count flag stores and rewrites
  memory calls with a regex.
- `generate()` is one 500-line function that lowers every instruction twice
  (once to find fallbacks, once with liveness).

The Go rewrite (5ae09f1, reverted) ported all of that faithfully: Capstone
through cgo, `i.Base() == "move"` dispatch, `fmt.Sprintf` C, a 3,422-line
golden C test, and byte-identical output as the goal, which forced it to copy
every Python quirk. It was not easier to read.

This plan changes the representations instead of the language only:

| Today | Plan |
|---|---|
| Capstone `CsInsn` + raw byte patches | own total decoder producing `Insn.t` variants |
| mnemonic strings everywhere | `match insn.op with Move (size, src, dst) -> ...` |
| hand flag-effect table | effects derived from the semantic IR (`Effects.of_sem`) |
| C strings + regex post-processing | small C AST and one printer |
| golden C text tests | tests on typed values and on behaviour (compile and run C) |
| JSON reports | typed values in memory; tests inspect them; a short run summary |
| "trust the fixpoint" | independent certificate checkers run on every emit |
| byte-identical output | decision-level and behavioural equivalence (section 8) |

## 1. Design decisions (please review)

- **D1 Language and build.** OCaml 5, dune 3, one library `f3recomp` plus one
  executable `f3recomp`, sources under `recomp/` next to the C headers the
  generated code includes (`recomp/cpu_ops.h`, `recomp/bitfield.h`,
  `recomp/CMakeLists.txt` stay where they are). `(include_subdirs unqualified)`
  keeps one flat module namespace while directories group files for humans.
- **D2 Libraries.** `otoml` 1.0.5 for TOML (verified: parses all 22
  `games/*/config.toml`; hex `0x604ff0`, `0xffffffff`, `0x80000000`, octal,
  binary and `1_000` all come back as exact OCaml ints; the alternative
  `toml` 7.1.0 rejects hex integers). `digestif` (SHA-1), `checkseum` (CRC-32),
  `cmdliner` (CLI), `alcotest` + `qcheck-core` + `qcheck-alcotest` (tests). All
  are in nixpkgs `ocamlPackages`. No Capstone, no C bindings.
- **D3 All-aligned discovery only.** All 22 in-tree configs set
  `coverage = "all_aligned"`. Recursive discovery and its seed heuristics
  (`trap #1` scanning, LEA callbacks, actor scripts, inline-string helpers,
  `scan_*` flags) are used by no game. The OCaml recompiler rejects
  `coverage = "recursive"` with a clear error. Configured `entry_points` and
  `jump_tables` stay (they feed sprite-trace seeds and devirtualized chains).
  This removes roughly 400 lines of discovery and the coverage gaps the models found in recursive
  mode. (Decided, Q1.)
- **D4 Total decoder.** `Decode.at` returns `Insn of Insn.t | Illegal of
  vector`; it never answers "unknown". A-line → vector 10, F-line → 11 (the
  EC020 has no coprocessor), ILLEGAL/BKPT/unassigned → 4, RESET is legal.
  Edge-case encodings follow MAME's M68EC020 (decided; see Q2):
  - Reserved full-format extension fields decode field by field, never trap:
    BD size `00` = null BD, bit 3 ignored, IS=0 I/IS `100` = post-indexed null
    OD, IS=1 I/IS `100`–`111` = memory indirect by bits 1–0.
  - BKPT → vector 4.
  - Every F-line word → vector 11, including cpGEN/cpScc/cpDBcc/cpTRAPcc.
    Musashi (ours and FBNeo's) treats those four as 2-byte no-ops, an
    `unemulated` placeholder; do not copy it.
  - MOVEC accepts `$000 $001 $002 $800 $801 $802 $803 $804`; any other code
    → vector 4; privilege is checked first (vector 8).
  Each of these is marked `Mame_default` in the decoder so a later board
  observation from 12 changes it in one place. Musashi's legality table
  (`recomp/68020_opcodes.csv`) is the test oracle apart from those encodings;
  per `AGENTS.md` a board observation by 12 outranks both.
- **D5 Semantic IR between decoder and C.** `Semantics.of_insn : Insn.t ->
  Sem.t` describes what an instruction does (reads, stores, flag producers,
  exceptions, pc update), independent of liveness. Liveness reads its effects
  from it; lowering specialises it with `live_out` and prints C. One semantics
  pass replaces the two `lower()` passes.
- **D6 Enhancements move into emit.** `sprite_groups.json` disappears, so
  `compile/enhancements.py`'s cross-process read has nothing to read
  (models: `NaiveJsonDropHasOrder` is UNSAT). `f3recomp emit` writes
  `enhancements.hpp` from the in-memory sprite groups; a config-only build runs
  `f3recomp enhancements` for the default header. (Decided, Q3.)
- **D7 Interrupt observers are explicit.** Liveness adds a per-game
  `interrupt_observes` flag set (default empty) to every live-out, and an
  `Irq_contract` analysis checks the default against each interrupt handler
  (flags read before written, following direct calls/returns, stopping at
  `rte`). Today's rule silently assumes the empty set; both models found the
  elision unsound when a handler reads inherited flags. (Q4.)
- **D8 Certificates run on every emit.** Each analysis result has an
  independent, local checker (section 7). Checkers are cheap (linear), so they
  always run; a failure aborts emit with the violated condition.
- **D9 Equivalence is judged on decisions and behaviour, not text.** A
  per-PC decision table (disposition, length, flow, effects, live-out, idle
  shape, chain candidates) is compared against the frozen Python; runtime
  replays compare per-frame machine state (section 8).
- **D10 Errors.** Config/ROM problems are fatal user errors with no recovery,
  so stages raise one exception `Error.Invalid of Error.t` (structured: where,
  what) caught once in `bin/main.ml`; checkers return
  `(unit, Violation.t list) result` so all violations are reported together.
  This keeps the entry chain a plain `|>` pipeline.
- **D11 Generated C keeps its shape.** Same ABI (`F3RT_ABI_VERSION 7`), same
  file set (`program.c`, `program.h`, `blocks_*.c`, `exceptions_{hot,cold}.c`,
  `sources.cmake`, `program.bin`, `sprite_groups.hpp`) plus `enhancements.hpp`,
  same macros (`F3_CHAIN`, `F3_DISPATCH_PC`, `F3_IDLE*`, `F3_UNIT_*`,
  `F3_PROFILE_*`). Text may differ (names, temporaries, ordering of
  declarations); behaviour must not.
- **D12 Frozen Python.** `recomp/*.py` moves to `tools/recomp_py/` (package
  `tools.recomp_py`) unchanged except imports and the CSV path; the five tool
  import sites switch to it. CMake keeps using it until the switch (T22).

## 2. Scope

In: discovery (all-aligned), RAM code, ROM loading/verification, profile
tiers/slim, decoder, semantics, liveness, idle loops, sprite trace, lowering,
layout, C printing, compile generators (`rom_manifest.hpp`,
`game_video_config.hpp`, `sprite_units.h/.hpp`, `enhancements.hpp`), the CMake
game-id check, the tests in `tests/recomp/`.

Out: the runtime (`runtime/`, `include/`) except the optional debug hook in
T21; the Rust crates; the Python tools themselves (they keep the frozen copy);
the sound CPU.

Preserved interfaces:
- CLI behaviour CMake depends on (`emit --config --rom-dir --output
  [--profile-tiers|--profile-slim] [--max-block-instructions]
  [--sprite-writers]`), as `f3recomp emit` with the same flags plus
  `--config-output-dir` (for `enhancements.hpp`).
- Config TOML schema (no renames). New optional keys: `[cpu]
  interrupt_observes = []`.
- Generated-code ABI and file set (D11). Block profile file format
  `F3-BLOCK-PROFILE 1` (read only).

Removed outputs: `lowering.json`, `coverage.json`, `profile_inventory.json`,
`sprite_groups.json` (no consumer outside `recomp/` and docs; checked with
grep).

Commands: `dune build --root recomp`, `dune test --root recomp`, the CMake
build per game (`cmake -B build -DF3_GAME=<id> -DF3_ROM_DIR=roms/<id>`), and
`nix shell nixpkgs#alloy6 nixpkgs#tlaplus -c docs/plans/ocaml-recompiler/models/run-all.sh`.

## 3. Architecture sketch

The entry point reads as the workflow. Each step is a function in the module
of the type it receives and returns the next type.

```ocaml
(* recomp/bin/main.ml *)
let emit (a : Args.emit) =
  Game.load a.config ~rom_dir:a.rom_dir                 (* Game.t        config + verified ROM image *)
  |> Game.discover                                       (* Program.t     every aligned PC decoded, RAM payloads, tables *)
  |> Program.select ~profile:a.profile                   (* Selection.t   entries, tiers, exception entries, hooks *)
  |> Selection.analyse ~sprite_writers:a.sprite_writers ?parity:a.parity
                                                         (* Analysis.t    semantics, sprite groups, CFG, liveness, idle loops *)
  |> Analysis.lower ~flags:a.flags                       (* Lowered.t     per-PC C statements, elisions recorded *)
  |> Lowered.lay_out ~max_block_instructions:a.max_block (* Layout.t      native functions, tiers, shards, chain owners *)
  |> Layout.print                                        (* Output.t      file names and contents *)
  |> Output.write ~dir:a.output ~config_dir:a.config_output_dir
  |> Summary.print

let compile_roms a = Config.load_all a.configs |> Rom_manifest.of_configs |> Rom_manifest.render ~game:a.game |> Output.write ~dir:a.out_dir
let sprite_units a = Config.load a.config |> Emit_units.of_config |> Emit_units.render |> Output.write ~dir:a.output_dir
let enhancements a = Config.load a.config |> Enhancements.default |> Enhancements.render |> Output.write ~dir:a.output_dir
let check_game a   = Config.load a.config |> Config.require_id a.id
```

Second level, inside the receivers (short sequences over their own data):

```ocaml
(* Game.discover *)
let discover g =
  let ram = Ram_code.decode g.config.ram_code g.rom in      (* all-aligned payload decode, before ROM *)
  Code_space.make g ~ram
  |> Discovery.all_aligned ~entry_points:g.config.discovery.entry_points
                           ~jump_tables:g.config.discovery.jump_tables

(* Selection.analyse : order fixed by models/pipeline.als *)
let analyse ?parity ~sprite_writers s =   (* optional first, so the pipe applies s last *)
  let semantics = Semantics.of_selection s in               (* one pass; Unsupported marks fallbacks *)
  let sprites = Sprite_trace.run s.program ~config:s.config ~writers:sprite_writers in
  let cfg = Cfg.build s semantics ~pins:(Pins.collect s semantics sprites) in
  let liveness, idle = match parity with
    | None -> Liveness.solve cfg, Idle_loop.find s.program ~native:(Cfg.native cfg)
    | Some d -> Decisions.liveness d, Decisions.idle_loops d s.program in   (* test-only, still checked *)
  Certificates.require [ Cfg.check s.program cfg; Liveness.check cfg liveness;
                         Idle_loop.check_all s.program cfg idle; Selection.check s ];
  { selection = s; semantics; sprites; cfg; liveness; idle }

(* Analysis.lower *)
let lower a ~flags =
  Pc_map.mapi (fun pc sem ->
      Lower.entry sem ~live_out:(Liveness.live_out a.liveness pc ~flags)
                      ~memory:(Memory_path.at a.sprites pc))
    a.semantics
  |> Lowered.make a
```

Third level is the work itself (`Decode.ea`, `Semantics.alu`, `Flags.specialise`,
`C.print_stmt`, ...).

## 4. Types

Files are under `recomp/lib/`. Signatures are the contract; private helpers
are the implementer's choice. "Absorbs" names the Python code each message
replaces.

### 4.1 Base values (`lib/base/`)

| Type | Data | Messages | Absorbs |
|---|---|---|---|
| `Ccr.t` (`ccr.ml`) | 5-bit set over `X N Z V C` | `all`, `none`, `nzvc`, `union`, `inter`, `diff`, `subset`, `to_string` | `CCR_*` constants |
| `Pc_map`, `Pc_set` (`pc.ml`) | `Map.Make(Int)` / `Set.Make(Int)` | stdlib | dicts keyed by pc |
| `Image.t` (`image.ml`) | `{ base : int; bytes : string }` | `u8 u16 u32 s8 s16 s32 : t -> int -> int`, `contains`, `crc32`, `slice` | `int.from_bytes(rom[...])` everywhere |
| `Error.t` (`error.ml`) | `{ where : string; what : string }`, exception `Invalid` | `fail : ('a, unit, string, 'b) format4 -> 'a` | `ValueError` |
| `Violation.t` (`violation.ml`) | `{ checker : string; pc : int option; detail : string }` | `pp` | — |
| `Output.t` (`output.ml`) | `{ files : (string * string) list; config_files : (string * string) list; summary : string list }` | `write ~dir ~config_dir : t -> string list` (write-if-changed, delete stale non-dot, non-`.stamp` files, return the summary plus what changed) | `write_if_changed`, the cleanup loop in `generate` |

### 4.2 Configuration and ROMs (`lib/config/`)

| Type | Data | Messages | Absorbs |
|---|---|---|---|
| `Config.t` (`config.ml`) | typed records: `game`, regions, `discovery`, `exclusions`, `ram_code`, `video` (incl. emit units), `input`, `sound`, `enhancements`, `cpu.interrupt_observes` | `load : string -> t`, `load_all`, `require_id` | `tomllib.loads` + ad-hoc `.get` chains, `_reject_unknown_keys`, the CMake one-liner |
| `Rom_manifest.t` (`rom_manifest.ml`) | per-game regions and lanes | `of_configs`, `load_region : t -> name -> rom_dir:string -> Image.t`, `render ~game` | `recomp/roms.py`, `discovery.load_rom` (merged: one lane loader), `compile/roms.py` |
| `Exclusion.t` (`exclusion.ml`) | `{ start; stop; reason; evidence; fill : int option }` | `of_config ~image`, `find : t list -> int -> t option` (24-bit mask), `check` (fill) | `parse_exclusions`, `exclusion_at`, `_verify_fill` |
| `Profile.t` (`profile.ml`) | roms, hits, misses | `read : string -> t`, `hot : t -> Image.t -> exclusions -> Pc_set.t` | `block_profile.read_profile`, `load_hot` |
| `Emit_units.t` (`emit_units.ml`) | `unit list` with ids, starts, ends, reg, size | `of_config`, `hooks : t -> (enter, exit) Pc_map`, `digest`, `render`, `guard_lines` | `sprite_units.py`, `compile/sprite_units.py` |
| `Game.t` (`game.ml`) | `{ config; rom : Image.t; exclusions }` | `load : string -> rom_dir:string -> t`, `discover : t -> Program.t` | `discovery.load_rom`, `discover` entry |

### 4.3 Decoder (`lib/m68k/`)

```ocaml
(* insn.ml *)
type size = Byte | Word | Long
type cond = T | F | Hi | Ls | Cc | Cs | Ne | Eq | Vc | Vs | Pl | Mi | Ge | Lt | Gt | Le
type reg = D of int | A of int
type index = { reg : reg; long : bool; scale : int }              (* scale 1 2 4 8 *)
type base = Areg of int | Pc_at of int | Suppressed                (* Pc_at = address of the extension word *)
type indirect = Direct | Pre of int | Post of int                  (* memory indirect with outer displacement *)
type ea =
  | Dreg of int | Areg_direct of int
  | Ind of int | Postinc of int | Predec of int
  | Disp of { reg : int; disp : int }
  | Indexed of { base : base; disp : int; index : index option; indirect : indirect }  (* brief and full formats *)
  | Absolute of int                                                (* .w already sign-extended *)
  | Pc_rel of int                                                  (* resolved address *)
  | Immediate of int
type op =
  | Move of size * ea * ea | Movea of size * ea * int | Moveq of int * int
  | Alu of alu * size * ea * ea          (* add sub and or eor cmp, incl. immediate and quick forms *)
  | Adda | Suba | Cmpa ... | Addx | Subx | Negx | Neg | Not | Clr | Tst | Ext | Extb | Swap
  | Shift of shift * size * count * ea | Bit of bit_op * bit_number * ea | Bitfield of ...
  | Mul of ... | Div of ... | Abcd | Sbcd | Nbcd | Pack | Unpk | Cas | Cas2 | Chk | Chk2 | Cmp2 | Tas
  | Bra of int | Bsr of int | Bcc of cond * int | Dbcc of cond * int * int | Scc of cond * ea
  | Jmp of ea | Jsr of ea | Rts | Rtd of int | Rtr | Rte | Trap of int | Trapcc of cond | Trapv
  | Lea of ea * int | Pea of ea | Link of int * int | Unlk of int | Exg of reg * reg
  | Movem of ... | Movep of ... | Moves of ... | Movec of ... | Move_sr | Move_ccr | Move_usp ...
  | Logic_ccr of ... | Logic_sr of ... | Stop of int | Reset | Nop
type t = { pc : int; length : int; op : op }

(* decode.ml *)
type decoded = Insn of Insn.t | Illegal of { vector : int; reason : string }
val at : Image.t -> int -> decoded           (* total, pure, reads only [pc, pc + length) *)

(* flow.ml *)
type escape = Return | Exception_return | Computed of Insn.ea | Raises
type t = { targets : int list; falls_through : bool; escape : escape option }
val of_insn : Insn.t -> t                    (* absorbs emitter.static_flow, discovery._resolve_target *)
```

The decoder absorbs `emitter._decode_ea`, `_full_ea`, `_pc_base`,
`_ea_extension_bytes`, `_get_size`, `_get_base_mnemonic`,
`_decode_index_expr`, `discovery._computed_transfer`, and every raw
`insn.bytes` read. Cases it must get right (each is a test in T6):

1. Brief vs full extension (bit 8); full-format BS/IS, BD size, I/IS
   combinations (reserved fields decoded the MAME way, D4), outer
   displacement; signed word base displacements.
2. PC-relative base = address of the extension word, also after operand
   prefixes (bitfields, CMP2/CHK2, CAS, MOVES, MOVEM, MUL/DIV.L, bit-immediate
   `0x08xx`, immediate ALU ops), and for MOVE destinations after the source's
   extension words.
3. Memory UNPK/PACK/SBCD/ABCD by primary word; CMPM's postincrement operands;
   DIVUL/DIVSL 32/32 remainder form with its Dr; MOVEC control codes; CAS2
   register pairs; TRAPcc operand words; Bcc/BSR `.s/.w/.l` (`0xff` → long).
4. A/F-line (all F-line words, coprocessor forms included), BKPT, ILLEGAL,
   unassigned MOVEC codes and unassigned words → `Illegal`; RESET legal.
5. Absolute short sign extension; A7 byte postincrement/predecrement steps by 2
   (semantics, but the decoder keeps the register number exact).

`asm.ml` (test-only library `f3recomp_test`): an encoder/assembler DSL,
`Asm.[ moveq 1 (d 0); add Long (d 0) (d 1); bcc Eq "loop" ]`, used to build test
programs readably and for round-trip properties. Replaces the hex word lists in
today's tests.

### 4.4 Code space, RAM code, discovery (`lib/discover/`)

| Type | Data | Messages | Absorbs |
|---|---|---|---|
| `Ram_code.routine` (`ram_code.ml`) | `{ destination; payload : Image.t; entry; decoded : decoded Pc_map.t }` | `decode : spec list -> Image.t -> routine list` (validates, applies `negate_be32`, decodes **every aligned offset**), `exits : routine list -> Pc_set.t` (direct targets outside the payload), `check` | `ram_code.py` (which followed flow and dropped exits) |
| `Code_space.t` (`code_space.ml`) | `{ game : Game.t; ram : routine list }` (ROM image and exclusions come from the game) | `make`, `region : t -> int -> region` (`Rom`, `Ram routine`, `Excluded exclusion`, `Outside`), `image_at` (the image a PC decodes from) | `ChainMap(discovery.instructions, ram_instructions)` |
| `Jump_table.t` (`jump_table.ml`) | `{ site; candidates : int list; evidence }`, evidence one of `Configured`, `Bounded {compare_pc; limit}`, `Heuristic scan` | `scan : Code_space.t -> Insn.t -> t option`, `check` | `_scan_register_table`, `_scan_pci_index_table`, `_scan_pc_memi_table`, `_scan_absolute_table`, `_candidate_targets` |
| `Program.t` (`program.ml`) | `{ game : Game.t; space; decoded : decoded Pc_map.t; functions : Pc_set.t; tables : Jump_table.t Pc_map.t; seeds }` | `insn`, `flow`, `select : t -> profile:Profile.t option -> Selection.t` (emit units come from `game.config`) | `Discovery` dataclass; `branch_targets`/`indirect_targets` become `Flow` + `tables` |
| `Discovery` (`discovery.ml`, functions only) | — | `all_aligned : Code_space.t -> entry_points:int list -> jump_tables:Config.jump_table list -> Program.t` | `discover` (exhaustive branch), `_extract_vector_seeds` |

`functions` keeps today's meaning (vector seeds, configured entries, direct
call targets), because sprite trace seeds from it.

### 4.5 Selection (`lib/discover/selection.ml`)

```ocaml
type disposition = Native of Insn.t | Exception_entry of int | Ram of Insn.t
type tier = Hot | Cold
type mode = Baseline | Tiers | Slim
type t = { program : Program.t; config : Config.t; units : Emit_units.t; mode : mode;
           entries : (disposition * tier) Pc_map.t }
val analyse : ?parity:Decisions.t -> sprite_writers:Sprite_trace.seed list -> t -> Analysis.t
val check : t -> (unit, Violation.t list) result
```

Absorbs the profile/tier/slim/exception-entry part of `generate`
(`rom_exceptions`, `load_hot`, unit-hook retention checks, "RAM entries are
always hot").

### 4.6 Semantics and effects (`lib/analyse/`)

```ocaml
(* sem.ml *)
type width = W8 | W16 | W32
type temp = int
type expr =
  | Const of int | Temp of temp | D of int | A of int | Sr | Ccr | Pc_const of int
  | Un of unop * width * expr | Bin of binop * width * expr * expr
  | Sext of width * expr | Cond of Insn.cond | Flag of Ccr.flag
type producer =                                  (* lazy CCR producers of cpu_abi.h *)
  | Logic of { result : expr; width : width }
  | Add of { src : expr; dst : expr; result : expr; width : width; carry : expr }
  | Sub of { src : expr; dst : expr; result : expr; width : width; carry : expr }
  | Cmp of { src : expr; dst : expr; result : expr; width : width }
type stmt =
  | Let of temp * expr
  | Load of temp * width * expr                  (* ordered bus read *)
  | Store of width * expr * expr                 (* ordered bus write *)
  | Store_long_low_first of expr * expr          (* MOVE.L -(An) order *)
  | Set_d of int * width * expr | Set_a of int * expr | Set_usp of expr | Set_control of control * expr
  | Flags of producer
  | Set_flags of (Ccr.flag * expr) list          (* eager, materialised flags *)
  | Flush
  | Set_sr of expr | Set_ccr of expr
  | If of expr * stmt list * stmt list
  | Helper of Helper.t                           (* cpu_ops.h helper, declared effects *)
  | Runtime of Runtime_call.t                    (* f3_reset_devices, f3_set_sr, ...; declares needs_canonical_sr *)
  | Raise of { vector : int; return_pc : int }   (* terminal *)
  | Goto of expr                                 (* sets pc, terminal *)
  | Stop of int
type t = Body of stmt list | Unsupported of string

(* semantics.ml + semantics_{move,alu,shift,bits,muldiv,bcd,flow,system}.ml *)
val of_insn : Insn.t -> Sem.t
val of_selection : Selection.t -> Sem.t Pc_map.t

(* effects.ml *)
type t = { reads : Ccr.t; writes : Ccr.t (* must *); boundary : bool }
val of_sem : Sem.t -> t
```

`Effects.of_sem` replaces `liveness.flag_effects` and its sets of mnemonics:
reads are flags in `Cond`/`Flag`/`Sr`/`Ccr` reads, plus all flags on any path
with `Raise` or a `Runtime` call that observes SR; must-writes are the
intersection over `If` branches; helpers carry a declared `(reads, writes)`
(the only hand table left, ~20 rows, each tested by eager-vs-lazy runs). A
conditional write is simply not a must-write, so it passes through without the
"model it as a read" rule.

`Flags.specialise : Sem.producer -> live:Ccr.t -> Sem.stmt list` is the single
place the elision rule lives (verified by `models/LazyFlags.tla`):

- `Logic`, `Cmp`: emit the pending op iff `live ∩ NZVC ≠ ∅`.
- `Add`/`Sub`: write X eagerly iff `X ∈ live`; pending op iff `live ∩ NZVC ≠ ∅`;
  if only X is live, clear the pending op (else a stale ADD flush clobbers X).

Representation invariant (TLC finding 3): whenever X may be live, a pending
ADD/SUB carries the same X as `sr`. Everything that creates pending state must
keep it; tests that start from arbitrary CPU states must satisfy it.

### 4.7 CFG, liveness, idle loops, interrupt contract (`lib/analyse/`)

```ocaml
(* pins.ml *)
type reason = Fallback | Unit_hook | Sprite_reset | Ram_code
type t = reason list Pc_map.t
val collect : Selection.t -> Sem.t Pc_map.t -> Sprite_groups.t -> t

(* cfg.ml *)
type boundary = Return | Computed | Leaves_native of int | Pinned of Pins.reason | Raises
type node = { pc : int; effects : Effects.t; succ : int list; boundaries : boundary list }
type t = { nodes : node Pc_map.t; interrupt_observes : Ccr.t }
val build : Selection.t -> Sem.t Pc_map.t -> pins:Pins.t -> t
val native : t -> int -> bool               (* emitted natively and unpinned *)
val check : Program.t -> t -> (unit, Violation.t list) result

(* liveness.ml *)
type t = { live_in : Ccr.t Pc_map.t; live_out : Ccr.t Pc_map.t }
val solve : Cfg.t -> t                       (* worklist fixpoint *)
val check : Cfg.t -> t -> (unit, Violation.t list) result
val live_out : t -> int -> flags:[`Lazy | `Eager] -> Ccr.t

(* irq_contract.ml *)
type verdict = Proven of Ccr.t | Unknown of { handler : int; at : int; why : string }
val observed : Program.t -> Cfg.t -> verdict list     (* one per interrupt vector handler *)

(* idle_loop.ml *)
type shape = Spin | Poll_bcc of { address : Sem.expr; size : Insn.size; cond : Insn.cond }
           | Affine of { word : Sem.expr }         (* DBcc polls are not idle loops (commit 1330b4a) *)
type t = { head : int; path : int list; shape : shape }
val find : Program.t -> native:(int -> bool) -> t Pc_map.t
val check_all : Program.t -> Cfg.t -> t Pc_map.t -> (unit, Violation.t list) result
```

Absorbs `liveness.analyse`, `Liveness.reaching_writers` (kept only if a test
needs it), `idle.py`, the `pinned` set in `generate`. The jsr rule stays: a
direct call flows into its callee only; the return point is reached through
the callee's `rts`, a boundary (`FlagsAcrossRts` in `liveness.als`).

### 4.8 Sprite trace and enhancements (`lib/analyse/`)

| Type | Data | Messages | Absorbs |
|---|---|---|---|
| `Sprite_value` (`sprite_value.ml`) | `Word.t`, `Value.t`, `State.t` (the abstract domain) | `merge`, `add`, `derive`, `constant`, `unknown` | `Word`, `Value`, `merge_*`, `_add_word`, `_derive_word` |
| `Sprite_groups.t` (`sprite_groups.ml`) | writers, resets, classifications, unresolved, stats | `render_hpp`, `writer : t -> int -> writer option` | `TraceResult`, `render_header` |
| `Sprite_trace` (`sprite_trace.ml`) | — | `run : Program.t -> config:Config.t -> writers:seed list -> Sprite_groups.t`, `transfer : Insn.t -> State.t -> State.t` | `SpriteTrace` class (transfer functions match on `Insn.op`/`Insn.ea` instead of Capstone operand types) |
| `Enhancements.t` (`enhancements.ml`) | frame counter, flicker holds | `of_config : Config.t -> Sprite_groups.t -> t`, `default`, `render` | `compile/enhancements.py` |

### 4.9 Lowering, layout, printing (`lib/emit/`)

| Type | Data | Messages | Absorbs |
|---|---|---|---|
| `C` (`c.ml`) | small AST: `expr` (ints print as `0x..u`, `cpu->d[i]`, calls, casts, binops), `stmt` (decl, assign, if, goto, label, switch, return, `chain`, `dispatch_pc`), `func` | `print_func`, `print_stmt` | every f-string of C |
| `Memory_path.t` (`memory_path.ml`) | `Inline` or `Bus_writes` (sprite reset PCs keep stores on `f3_write*`) | `at : Sprite_groups.t -> int -> t` | `_inline_memory` regex |
| `Lower` (`lower.ml`) | — | `entry : Sem.t -> live_out:Ccr.t -> memory:Memory_path.t -> C.stmt list * Elided.t` | `emitter.lower`, `_gen_write`, `_cc_*` |
| `Lowered.t` (`lowered.ml`) | `{ analysis; bodies : body Pc_map.t }`, a body is `Native {stmts; elided}`, `Fallback` or `Exception vector` | `make`, `lay_out ~max_block_instructions`, `check` (elided ∩ live_out = ∅), counters for the summary | `_count_flag_stores` string scan |
| `Layout.t` (`layout.ml`) | functions `{ name; tier; pcs }`, `owner : string Pc_map.t`, shards of ≤128 functions per tier | `print : t -> Output.t`, `check` (RAM functions hold one instruction; no chain or switch case targets RAM) | block packing, sharding, chain guards, indirect chains, exception tiers in `generate` |
| `Blocks_c`, `Program_c` (`blocks_c.ml`, `program_c.ml`) | — | `files : Layout.t -> (string * string) list` | the remaining string assembly of `generate` |
| `Summary` (`summary.ml`) | — | `of_layout : Layout.t -> string list` (≤ 15 lines: entries by disposition, tiers, elided producers, idle loops by shape, chain sites/cases, certificates checked, sprite writers), `print` | `lowering.json`, `coverage.json`, printed JSON |
| `Decisions` (`decisions.ml`) | per-PC decision rows | `of_lowered`, `print` (TSV, section 8) | — (new, for equivalence) |

## 5. Protocols and mediators

- **`Certified`** (`certificates.ml`): `module type CERTIFIED = sig type t
  type context val name : string val check : context -> t -> (unit,
  Violation.t list) result end`. Implementors: `Cfg` (context `Program.t`),
  `Liveness` (`Cfg.t`), `Idle_loop` (`Program.t * Cfg.t`), `Jump_table`
  (`Program.t`), `Exclusion` (`Image.t`), `Ram_code` (`Code_space.t`),
  `Selection` (`unit`), `Lowered` (`Liveness.t`), `Layout` (`unit`).
  `Certificates.require` runs a list, prints every violation, raises once. This
  is a real shared capability: the same message with the same meaning
  ("prove this result satisfies its local conditions").
- **Rendered headers** (`Rom_manifest.render`, `Emit_units.render`,
  `Enhancements.render`, `Sprite_groups.render_hpp`) all return
  `Output.t`; plain functions with one shape, no module type needed.
- **No managers.** The pipeline has no mediators; the runtime is the only
  peer and is reached through generated C. `Selection.analyse` is a receiver
  method, not a manager: it works on the selection's own program.

## 6. Module layout

```
recomp/
  dune-project                       (lang dune 3.x; package f3recomp)
  68020_opcodes.csv cpu_ops.h bitfield.h CMakeLists.txt   (unchanged)
  bin/main.ml                        CLI: emit, compile-roms, sprite-units, enhancements,
                                     check-game, lower, decisions
  lib/dune                           (library f3recomp) (include_subdirs unqualified)
  lib/base/      ccr pc image error violation output certificates
  lib/config/    config rom_manifest exclusion profile emit_units game
  lib/m68k/      insn decode flow
  lib/discover/  ram_code code_space jump_table discovery program selection
  lib/analyse/   sem semantics semantics_{move,alu,shift,bits,muldiv,bcd,flow,system}
                 helper runtime_call effects flags pins cfg liveness irq_contract
                 idle_loop sprite_value sprite_groups sprite_trace enhancements analysis
  lib/emit/      c memory_path lower lowered layout blocks_c program_c summary decisions
  test/          asm (test library) + one test executable per area:
                 decode_test sem_test liveness_test idle_test layout_test sprite_test
                 config_test codegen_c_test (compiles and runs C) models_test
tools/recomp_py/                     frozen Python (package tools.recomp_py)
tools/recomp_compare/                decisions dumper for the frozen Python + diff + replay scripts
```

## 7. Verification built into the program

### 7.1 Certificates (run on every `emit`, D8)

| Result | Certificate | Checker condition (local, no re-solving) |
|---|---|---|
| CFG | the node table | every `Flow.of_insn` successor is an edge to a native node or the node lists a boundary reason; every pin is a boundary. This is the precondition the Alloy soundness proof needs ("actual edges ⊆ graph edges ∪ edges from boundaries"). |
| Liveness | `live_out` per node | `boundary ⇒ live_out = all`; `interrupt_observes ⊆ live_out`; for each edge n→s: `reads(s) ∪ (live_out(s) − writes(s)) ⊆ live_out(n)`. `liveness.als` proves: accepted ⇒ no observable elision (8 nodes), exact liveness is accepted, solver output is accepted. |
| Dropped producers | `Elided.t` per PC | `elided ⊆ writes(pc)` and `elided ∩ live_out(pc) = ∅`. |
| Idle loops | shape + path | re-derive the shape from the decoded instructions; every path PC `Cfg.native`; poll addresses are invariant EAs (`(An)`, `d16(An)`, absolute); affine path stores only the RNG word; RAM-only polling is still enforced by `f3_ram` at run time. |
| Jump tables | evidence + candidates | candidates are even, decoded, not excluded, ROM, and for `Bounded` the compare/branch at `compare_pc` limits the index to `limit` entries. Candidates need no soundness proof: generated switches compare the run-time pc. |
| Exclusions | fill, intervals | fill bytes match; sorted, disjoint; no entry starts inside; decoded transfers into an exclusion are listed in the summary as fatal-if-taken. |
| RAM code | payload, routines | payload = transform(source bytes); ranges disjoint and inside work/sprite RAM windows; entry inside its payload; `Layout.check`: one instruction per RAM function, no chain/switch/`F3_DISPATCH_PC` target in RAM (`Dispatch.tla`). |
| Coverage | `Selection.t` | each entry has one disposition; registered table sorted and unique; unit hooks retained (slim) and printed exactly once; interrupt-contract verdicts are `Proven` subsets of `interrupt_observes` (`Unknown` is an error unless overridden, Q4). |

### 7.2 Small-scope exhaustive tests (in `dune test`)

- **Decoder, all 65,536 primary words** × a fixed extension set (zeros, ones,
  and for EA-bearing words all 512 low-bit patterns of a full-format extension):
  never raises; `Illegal 4` exactly where `68020_opcodes.csv` says illegal,
  except the documented list (RESET, A/F-line vectors, the `Mame_default`
  encodings of D4, notably the cpGEN/cpScc/cpDBcc/cpTRAPcc words the table
  calls legal); length even
  and ≤ 22; *locality*: mutating bytes at or beyond `pc + length` never changes
  the result; round trip `Decode (Asm.encode i) = i`.
- **Liveness over all small CFGs:** every graph with ≤ 3 nodes, every
  successor relation, boundary set and per-node reads/must-writes over two
  abstract flags (≈ 1.7·10⁷ cases), compare `solve` with brute-force path
  enumeration (the `trulyLive` definition of `liveness.als`); check that the
  checker accepts the solution and rejects it once any truly live bit is
  removed. QCheck extends to random 8–20 node graphs.
- **Flag specialisation:** exhaustive over producer sequences of length ≤ 4
  and all live sets, executed by an OCaml model of `f3_cc_flush` against an
  eager model (a direct port of `LazyFlags.tla`'s semantics on concrete 8/16/32
  bit values), entry states satisfying the representation invariant.

### 7.3 Refinement and differential checks

- **Eager-vs-lazy C** (port of `FlagLivenessEquivalenceTests`): random
  straight-line and forward-branch programs (built with `Asm`), compiled twice,
  once with every `live_out = all`, once with liveness; run from many entry
  points and invariant-respecting pending states; compare registers, `sr`
  after flush, and a write log. Also exercised with `--flags eager` builds of
  whole games (section 8).
- **Musashi differential** (`tools/differential`): add `--lowerer ocaml` to
  `runner.py`, which calls `f3recomp lower --pc 0x… --hex … [--count n]` and
  uses its printed statements in place of `recomp.emitter.lower`. Mismatches
  are triaged; where 12's board observation disagrees with Musashi, hardware
  wins and the case is recorded (`AGENTS.md`).
- **Decision tables vs frozen Python** and **runtime replays** (section 8).

## 8. Migration and equivalence

### 8.1 Frozen Python

T1 moves `recomp/*.py` to `tools/recomp_py/` (drop `recomp/compile/`; the
OCaml generators replace it), fixes `opcodes.py` to read
`recomp/68020_opcodes.csv`, and updates the imports in
`tools/analysis/game.py`, `tools/analysis/cli.py`, `tools/rom/exclusions.py`,
`tools/differential/runner.py`, `tools/profile/merge.py` and `pyproject.toml`
(`packages`). `cmake/codegen.cmake` temporarily calls
`python -m tools.recomp_py emit`. Capstone stays a Python dependency for the
tools. Frozen means: no feature work; bug fixes only if a tool needs them.

### 8.2 Decision tables

`f3recomp decisions --config C --rom-dir R [--profile-tiers P]` prints one TSV
row per entry:

```
pc  disposition  length  flow                 reads  writes  live_out  idle  chain_candidates
0x000400  native  2  next                    -      NZVC    Z         -     -
0x000402  native  2  branch:0x000400,next    Z      -       XNZVC     poll  -
```

plus `sprite-writer` / `sprite-reset` rows. `tools/recomp_compare/decisions.py`
produces the same rows from the frozen Python (Capstone length, `static_flow`,
`flag_effects`, `liveness.live_out`, `find_idle_loops`, `indirect_targets`,
`lower() is None`, `trace()`). `tools/recomp_compare/diff.py` groups
differences by column. Every difference is either a bug or an entry in
`tools/recomp_compare/expected.tsv` with a reason (for example "Capstone
mis-decodes full-format word displacement", "effects derived from IR: X not
must-written by LSL #0"). Gate: no unexplained rows on all 22 games, in
baseline, tiers and slim modes.

### 8.3 Parity builds and runtime replay

*Amended after T19b (2026-11-19).* The certificate rejects the frozen Python
decisions for landmakrj. OCaml decodes 11,842 entries that Python lacks.
5,630 Python instruction rows are OCaml `Mame_default` exception stubs. Python
gives a divide-exception boundary only `X` where a stacked SR needs `XNZVC`.
So OCaml-with-Python-decisions cannot be built, and steps 1–2 below cannot be
byte for byte. T21 therefore compares the frozen Python build with the OCaml
build using step 3's masked relation directly: frame, palette, graphics and
non-stack RAM exact; CPU registers exact except CCR bits outside
`live_in_A ∩ live_in_B`; stacked-SR CCR bits masked. `--parity-from` stays for
OCaml-vs-OCaml comparisons.

`emit --parity-from DECISIONS.tsv` (test-only flag) makes OCaml use the
imported `live_out` and idle-loop set instead of its own, after its checkers
accept them. With identical decisions, elided flags and idle yields are
identical, so machine state must match byte for byte. Steps:

1. Build each game twice (frozen Python, OCaml parity) and run the headless
   frontend with the same input script: `--headless --frames N --inputs S
   --dump-dir D --dump-every K`. `dump_machine` (`runtime/capture_io.hpp`)
   writes `cpu.json` (pc, sr, d, a, steps), `mainram.bin`, `shared.bin`,
   palette, graphics, control, sprite writers and the rendered frame.
2. Compare every dump byte for byte. Gate: equal on all 22 games, baseline and
   tiers, for N ≥ 3000 frames each.
3. Compare OCaml-own vs OCaml-parity (and `--flags eager`) builds with the
   masked relation: `sr` CCR bits compared only for `live_in_A(pc) ∩
   live_in_B(pc)` (both analyses are sound, so every truly observable flag is in
   both sets; an eager build's set is all flags); RAM compared exactly except the CCR bits of SR words stacked
   by IRQ/exception entry (`LazyFlags.tla` `stacked_frames`). The runtime does
   not know which words those are today; T21 adds an opt-in debug hook that
   records stacked-SR addresses into the dump. Until then, step 3 runs only for
   games whose own and parity decisions agree on liveness.

### 8.4 CMake switch

Three steps (T5, then T22):
1. `F3_RECOMP_EXECUTABLE` cache variable; when set, CMake calls it for the
   game-id check (`check-game`), `compile-roms`, `sprite-units`, `emit`
   (adds `--config-output-dir ${F3_GENERATED_CONFIG_DIR}`) and
   `enhancements` (config-only builds, or when `F3_GENERATED_DIR` has no
   `enhancements.hpp`); otherwise the frozen Python path. The emit stamp hashes
   the executable instead of `recomp/*.py`; outputs drop `sprite_groups.json`
   and add `enhancements.hpp`.
2. Default to OCaml: `find_program(f3recomp)` (from the Nix flake), else
   `dune build --root recomp` at configure time.
3. Remove the Python generator path from `cmake/codegen.cmake`. `uv` stays
   for shader compilation (`CMakeLists.txt:285`) and the tools.

Then `tests/recomp/*.py` are deleted (their OCaml ports landed in T20) and the
CI workflow gains a `dune test` job (T23).

### 8.5 Nix requirements (user wires the flake)

`ocaml` (5.x), `dune_3`, `findlib`, `ocamlPackages.{otoml, digestif,
checkseum, cmdliner, alcotest, qcheck-core, qcheck-alcotest}`, a C compiler
for the C-running tests, and a package output that installs `f3recomp` on
`PATH` (e.g. `buildDunePackage`). Note: in a plain `nix shell` findlib's setup
hook does not run, so `OCAMLPATH` is empty; `nix develop` or the dune package
sets it.

## 9. Tasks

Each task leaves `dune build` and `dune test` green and the existing CMake
build working. "Gate" lines are the acceptance criteria. Dependencies are in
brackets; tasks whose dependencies are met and whose files are disjoint can
run in parallel. ROM-based gates use the local `roms/<game>` sets (22 games);
`diff.py --columns ...` is the decision comparison of T2b, run on all 22 games
with `tools/recomp_compare/expected.tsv` holding explained differences.

The OCaml `decisions` command grows column by column: each task that owns a
column adds it and gates on it.

### Phase A — foundation (all parallel after T0)

- **T0 Scaffold** []. `recomp/dune-project`, `lib/dune`, `bin/main.ml` with
  cmdliner subcommands that exit 2 "not implemented", `test/dune` with one
  alcotest. Gate: `dune build --root recomp && dune test --root recomp`;
  `f3recomp --help` lists emit, compile-roms, sprite-units, enhancements,
  check-game, lower, decisions.
- **T1 Freeze Python** []. Section 8.1. Gate: `uv run pytest tests/tools`
  passes; `uv run python -m tools.recomp_py emit --config
  games/landmakrj/config.toml --rom-dir roms/landmakrj --output <tmp>` writes
  the same files as `python -m recomp emit` did before the move; the CMake
  build of one game is green.
- **T2a Config** [T0]. `Config`, `Exclusion`, `Error`, `Ccr`, `Pc`, `Image`.
  Every key the Python reads, with the same validations (unknown lane keys,
  ranges, fills). Gate: all 22 configs load; one table-driven test per
  rejection case in `test_roms.py`, `test_discovery.py` (exclusions),
  `test_sprite_units.py`, `test_enhancements.py`.
- **T2b Python decision dumper and diff** [T1]. `tools/recomp_compare/
  decisions.py` (rows of section 8.2 from the frozen Python), `diff.py
  --columns`, empty `expected.tsv`. Gate: dumps all 22 games in baseline,
  tiers and slim modes; `diff.py` of a dump against itself is empty.
- **T3 ROM manifest and game-id check** [T2a]. `Rom_manifest` (one lane loader
  merging `discovery.load_rom` and `roms.load_region`: CRC-32/SHA-1, short
  dumps, word swap, mirrors), `render`, `check-game`, `compile-roms`. Gate:
  `rom_manifest.hpp` and `game_video_config.hpp` byte-identical to
  `compile/roms.py` for every config and `--game`; loaded main images equal
  Python's for all 22 games.
- **T4 Emit units** [T2a]. `Emit_units`, `sprite-units`. Gate:
  `sprite_units.h/.hpp` byte-identical for all 22 configs; ports of
  `test_sprite_units.py`.
- **T5 CMake plumbing** [T3, T4]. `F3_RECOMP_EXECUTABLE` cache variable
  (section 8.4 step 1) for `check-game`, `compile-roms`, `sprite-units`,
  `enhancements` and `emit`; the Python path stays the default. Gate: a
  configure with `-DF3_RECOMP_EXECUTABLE=...` uses OCaml for the three
  config-time generators and produces identical headers; without it nothing
  changes.

### Phase B — decoder

- **T6 Decoder** [T2a, T2b]. `Insn`, `Decode`, `Flow`; the cases of 4.3;
  `decisions` with `length` and `flow` columns. Gate: the exhaustive decoder
  tests of 7.2; one named test per case in 4.3; `diff.py --columns
  length,flow` clean (Capstone errors explained). May split: T6a EAs and
  extension words, T6b instruction families.
- **T7 Asm** [T6, or T6a if T6 is split]. Encoder DSL in `test/asm.ml`; QCheck round trip over
  generated `Insn.t`. Gate: 10⁵ round trips pass.
- **T8 Code space and RAM code** [T6]. `Ram_code` (all-aligned payload decode,
  `negate_be32`, exits), `Code_space`. Gate: pbobble3j/pbobble4j payloads and
  entries match Python's routines; a synthetic payload with a computed jump
  into its middle registers that PC (the `RamFollowFlow` case).

### Phase C — analyses

- **T9 Discovery and jump tables** [T6, T8]. `Discovery.all_aligned`,
  `Jump_table` with evidence, `Program`; `disposition` (decode part),
  `chain_candidates` columns. Gate: `diff.py --columns
  disposition,chain_candidates` clean; `coverage = "recursive"` fails with a
  message.
- **T10 Profile and selection** [T9]. `Profile`, `Selection`,
  `Selection.check`; `tier` column. Gate: ports of `test_block_profile.py` and
  the selection parts of `test_profile_codegen.py`; `diff.py --columns tier`
  clean in tiers and slim modes.
- **T11 Semantic IR core and `lower` command** [T6]. `Sem`, `Effects`,
  `Flags.specialise`, `Helper`, `Runtime_call`, `C` AST and printer,
  `Lower.entry`, `f3recomp lower --pc --hex [--count] [--live-out]`. Gate: the
  exhaustive flag-specialisation test (7.2); printer unit tests.
- **T12 C test harness and Musashi adapter** [T11, T7]. An OCaml test utility
  that assembles `Asm` programs, lowers them twice (eager and with liveness of
  the snippet), compiles with `cc` against `include/` and `recomp/`, runs from
  invariant-respecting entry states, and compares state and a write log;
  `tools/differential/runner.py --lowerer ocaml`. Gate: the harness runs the
  IR-level cases of T11; the runner starts, and reports every case as
  unsupported until T13 lands.
- **T13a–e Semantics by family** [T11, T12]. a: moves, LEA/PEA, MOVEM, MOVEP,
  EXG, LINK/UNLK; b: ADD/SUB/CMP/AND/OR/EOR/NEG/NOT/CLR/TST/EXT/SWAP, quick and
  immediate forms, ADDX/SUBX/NEGX; c: shifts/rotates, bit ops, bitfields, Scc;
  d: MUL/DIV, BCD, PACK/UNPK, CAS/CAS2, TAS, CHK/CHK2/CMP2; e: branches, DBcc,
  JMP/JSR/RTS/RTD/RTR/RTE, TRAP/TRAPcc/TRAPV, MOVEC, MOVES, SR/CCR/USP moves,
  STOP, RESET, NOP. Gate per family: Musashi differential cases of the family
  pass or are triaged (hardware observations win, `AGENTS.md`); the T12
  eager-vs-lazy run for the family passes; generated C compiles with
  `-Wall -Wextra -Werror`; the family's `Unsupported` count on the 22 ROMs is
  ≤ Python's fallback count. All five can run in parallel.
- **T14 Lowering details** [T13*]. `Memory_path`, `Lowered`, `Lowered.check`.
  Gate: port of `test_inline_memory.py`.
- **T15 CFG, liveness, interrupt contract** [T11, T10]. `Pins`, `Cfg`,
  `Liveness.solve/check`, `Irq_contract`, `[cpu] interrupt_observes`;
  `reads`, `writes`, `live_out` columns. Gate: the exhaustive small-CFG test
  (7.2); ports of `FlagLivenessTests`; `diff.py --columns live_out` clean;
  `reads,writes` differences all explained (IR-derived effects may be more
  precise); `Irq_contract` verdict printed for every game (feeds Q4).
- **T16 Idle loops** [T15]. `Idle_loop`; `idle` column. Gate: port of
  `test_idle.py`; `diff.py --columns idle` clean.
- **T17 Sprite trace** [T9]. `Sprite_value`, `Sprite_trace`, `Sprite_groups`;
  sprite rows. Gate: port of `test_sprite_trace.py`; `sprite_groups.hpp`
  byte-identical to Python's on 22 games, with and without
  `--sprite-writers`; stats equal.
- **T18 Enhancements** [T17, T2a]. Gate: port of `test_enhancements.py`;
  `enhancements.hpp` byte-identical to `compile/enhancements.py` for the same
  groups; `enhancements` command writes the default header.

### Phase D — output

- **T19 Layout, printing, emit** [T14, T15, T16, T17, T18, T5]. `Layout`
  (page packing of 2·max_block bytes, tiers, shards of 128, names, chain
  guards, `F3_DISPATCH_PC`, indirect switches capped at 256, unit hooks,
  profile hits, exception entries per tier, RAM payload tables, exclusion
  table, registration function), `Layout.check`, `Program_c`, `Blocks_c`,
  `Output`, `Summary`, the `emit` command with `--parity-from` and
  `--flags eager`. Gate: every game builds through
  `-DF3_RECOMP_EXECUTABLE` with `-Werror` in baseline, tiers and
  instrumented-profile configurations; `Layout.check` rejects a hand-built
  layout that chains into RAM; CLI tests for argument errors.

### Phase E — equivalence

- **T20 Port tests/recomp** [T19]. C-running OCaml tests for deadlines and
  resumption, overlapping entries, CMPM aliasing, block chaining, indirect
  chains, flag-liveness equivalence, profile codegen and runtime persistence,
  inline memory, ABI version (read `F3RT_ABI_VERSION` from
  `include/f3rt/cpu_abi.h`). Tests assert on behaviour and typed values, never
  on C text. Gate: a mapping table (Python test → OCaml test) in the PR covers
  every Python test.
- **T21 Replay harness** [T19]. Scripts for section 8.3 steps 1–3; the opt-in
  runtime hook recording stacked-SR addresses (debug builds only). Gate:
  Python and OCaml-parity builds equal byte for byte on 22 games, baseline and
  tiers, 3000 frames each.

### Phase F — switch

- **T22 Flip CMake** [T19, T20, T21]. Section 8.4 steps 2 and 3. Gate: a clean
  configure without `uv` on `PATH` except for shaders builds every game.
- **T23 Cleanup** [T22]. Delete `tests/recomp/`; update
  `docs/notes/raw/docs/runtime-game-config-reference.md` (it names
  `lowering.json` and `coverage.json`) and the README prerequisites; CI job
  `dune test`.

## 10. Open details (implementer's choice)

Names of private helpers and temporaries in generated C; how `Sem` temps map
to C locals; the exact C AST constructors; whether semantics files split
further; QCheck generator shapes; error message wording (meaning must match);
how the summary is formatted; input scripts for replays (any deterministic
script, committed under `tools/recomp_compare/inputs/`).

## 11. Decisions on the open questions

- **Q1 Recursive discovery — decided: drop it** (D3). All 22 configs use
  `all_aligned`, and the recursive-mode gaps the models found (findings 4
  and 7) disappear with it. If a future game needs it, it comes back as its
  own task that seeds ROM discovery with RAM-code exits and reports heuristic
  jump tables as unproven coverage.
- **Q2 Reserved encodings — decided: follow MAME** (D4). Reserved
  extension fields, BKPT, F-line words and unassigned MOVEC codes behave as
  MAME's M68EC020 does. Python made them fallback stubs (fatal if executed);
  none is executed in tested play, so this changes decoder output and the
  decision-table diff, not gameplay. A question to 12 may still go out; any
  board answer replaces the matching `Mame_default` case.
- **Q3 Enhancements — decided: `emit` writes `enhancements.hpp`** (D6). The
  `constexpr` lookup alternative would touch runtime code for no gain.
- **Q4 Interrupt observers — decided: fail.** If `Irq_contract` cannot prove
  a handler clean (e.g. a computed jump before every flag is written), emit
  fails; `[cpu] interrupt_observes` in the game config is the explicit
  override. A wrong silent default would break hardware equivalence.
  *Amended after T15 (2026-11-19):* the sharpened analysis proves 30 of 81
  handler entries clean, yet every game keeps at least one `Unknown`. The causes
  are real but benign-looking: task switchers that spill A7 into a TCB
  (`move.l a7,(a6)`, `move.l a7,d(a5)`), callback tables (`jsr (a0)`), and
  A-line or TRAP dispatch. Neither of the two `interrupt_observes` choices fits.
  An empty set is unproven. The full set is sound, but it may give up most
  elision if interrupts can be taken at every block boundary. So `emit`
  accepts two overrides:
  - `interrupt_observes`, which is sound and costs elision;
  - `[cpu] interrupt_handlers_trusted = "<evidence>"`, which records that the
    handlers do not branch on the interrupted CCR, printed in the summary.

  Each `Unknown` must be covered by one of the two. T19 reports the elision
  cost of the sound option per game, so the choice can be made on numbers.
- **Q5 RAM-code pins — decided: keep until the switch, then drop.** Python
  pins every RAM-code PC (reads all flags). They keep decision tables at
  parity during Phase E; the payload check makes them unnecessary for
  soundness, so a follow-up after the switch removes them.
- **Q6 Binary source for CMake — decided: both.** CMake uses an `f3recomp`
  on `PATH` (the flake package) when present, and otherwise falls back to a
  configure-time `dune build`, so non-Nix builds keep working.
- **Q7 Frozen Python lifetime — decided: until the tools are ported.**
  `tools/recomp_py` stays until the five tool imports move to `f3recomp`
  subcommands; tracked in Beads as a follow-up after the switch, not part
  of this plan's tasks.

## 12. What the model checking changed

Summarised from `models/README.md`:

1. Liveness gains `interrupt_observes` and `Irq_contract` (D7): both models
   found elision observable by an IRQ handler under today's rule.
2. Replay equivalence uses parity builds first and a masked relation second:
   stacked IRQ frames legitimately carry stale dead CCR bits (8.3).
3. The lazy-flag representation invariant is written down and constrains test
   generators (4.6).
4. RAM payloads are decoded all-aligned and before ROM discovery; their exits
   seed discovery (4.4, coverage and pipeline models).
5. The decoder is total (D4): partial decoding leaves holes even in
   all-aligned mode.
6. Jump-table evidence is a coverage claim only; devirtualized chains need no
   proof because they compare the run-time pc (7.1).
7. RAM functions stay single-instruction and are never chain targets,
   enforced by `Layout.check` (Dispatch model).
8. Sprite trace runs before liveness and printing; `emit` writes
   `enhancements.hpp` (pipeline model).
