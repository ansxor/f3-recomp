# Design models for the OCaml recompiler plan

These models check the design in `../PLAN.md` before any code exists. Each one
encodes a rule the generated code or the pipeline must keep, plus the variants
that break it, so a passing check means something. Every expected
counterexample below was reproduced; every "no counterexample" ran to
completion within the stated scope.

## Running

```sh
nix shell nixpkgs#alloy6 nixpkgs#tlaplus -c ./run-all.sh       # everything, ~1 min
nix shell nixpkgs#alloy6 -c ./run-alloy.sh liveness.als         # one Alloy model
nix shell nixpkgs#tlaplus -c tlc -deadlock -config LazyFlags_python_rule.cfg LazyFlags.tla
```

`run-alloy.sh` runs Alloy 6.2 headless with MiniSat (SAT4J, the default, takes
minutes on `liveness.als`) and prints one line per command. TLC is
tla2tools 1.7.4. Each `.cfg` starts with an `Expected:` line.

## Results (2026-11-19)

| Model | Command / config | Result | Meaning |
|---|---|---|---|
| liveness.als | PythonRuleSound (5 nodes) | counterexample | Python's rule elides a flag an IRQ handler reads |
| | PythonRuleSoundWithoutIrqObservers (6) | none | the rule is sound if handlers never read inherited CCR |
| | OcamlRuleSound (8) | none | adding the IRQ-observer set to every live-out is sound |
| | CheckerSound (8) | none | anything the local checker accepts elides only dead writes |
| | CheckerAcceptsTruth (6) | none | the exact liveness is always accepted (checker is complete) |
| | WideningOwnWritesStaysAccepted (6) | none | any less aggressive elision stays certifiable |
| | OcamlFixpointAccepted (6) | none | solver output and checker agree |
| | SharedTail, FlagsAcrossRts, AliasedEntry | SAT | the messy shapes exist in the checked space |
| coverage.als | AllAlignedTotalNoHoles | none | all-aligned + total decoder never leaves an executed PC unregistered |
| | AllAlignedPartialDecoder | counterexample | a decoder that can answer "unknown" leaves holes |
| | RecursiveSeparateRegions | counterexample | ROM code entered only from RAM code is never discovered |
| | RecursiveProvenJointNoHoles | none | fixed by seeding ROM discovery with RAM-code exits |
| | RecursiveHeuristicTables | counterexample | heuristic jump-table scans can miss targets |
| | RamFollowFlow | counterexample | following flow inside a RAM payload misses computed/ROM entries into it |
| | SlimHolesAreColdOnly, EntriesAvoidExclusions | none | slim holes are exactly the documented cold aborts |
| pipeline.als | PythonHasOrder | SAT | today's order works only through sprite_groups.json on disk |
| | NaiveJsonDropHasOrder | UNSAT | dropping the JSON but keeping a separate enhancements generator breaks the build |
| | OcamlHasOrder | SAT | the plan's pipeline has an order in both CMake modes |
| | SpriteTraceBeforeLiveness, RamCodeBeforeDiscovery | none | forced orderings the plan states |
| LazyFlags.tla | python_rule | no error (39,592 states) | elision by an accepted certificate refines eager flags |
| | arbitrary_entry | violated | entry states must keep the representation invariant (below) |
| | no_stale_clear | violated | rediscovers the X-only ADD bug `_cc_arith` guards against |
| | irq_reader_python_rule | violated | same IRQ gap as liveness.als, now on the real lazy mechanics |
| | irq_reader_proposed_rule | no error | the IRQ-observer rule closes it |
| | stacked_frames | violated | stacked SR carries stale dead bits; byte-exact RAM comparison across different liveness is invalid |
| Dispatch.tla | safe, safe_without_idle | no error | RAM freshness, no slice overrun, IRQ eventually taken |
| | chain_into_ram | RamCodeFresh violated | a native transfer into RAM skips the payload check |
| | ram_block_two | RamCodeFresh violated | two RAM instructions in one function run stale code after a self-write |
| | ram_block_two_no_self_write | no error | multi-instruction RAM functions need a no-alias proof |
| | unguarded_back_edge | NoOverrun violated | every chained transfer needs the deadline guard |
| | witnesses | violated (= reachable) | idle yield, native RAM execution and the IRQ handler all occur |

## Findings and how the plan changed

1. **Interrupts observe flags (liveness.als, LazyFlags.tla).** `recomp/liveness.py`
   says interrupts "add no liveness". An IRQ can be taken at any instruction
   boundary; the handler starts with the interrupted CCR and stacks it. If a
   handler reads a flag before writing it, an elided producer is observable.
   *Change:* the CFG carries an `interrupt_observes` flag set that every node's
   live-out includes (default empty), and an `Irq_contract` analysis checks the
   empty default against every interrupt handler (PLAN D7, task T15).
2. **Stacked frames carry dead bits (LazyFlags stacked_frames).** Even with sound
   elision, an IRQ frame stores CCR bits that differ from hardware's. They are
   dead (rte restores them, later code overwrites them first), but they sit in
   RAM. *Change:* runtime replay comparisons between builds with different
   liveness are byte-exact only after masking; the plan compares builds with
   identical imported decisions first (PLAN "Equivalence", D9).
3. **Representation invariant (LazyFlags arbitrary_entry).** TLC's first run, with
   arbitrary entry states, found a "violation" where a pending ADD carries an X
   different from `sr.X` and a LOGIC replaces it. That state only arises when X
   was dead at the ADD, so it never reaches a dispatcher entry after a real
   boundary. *Change:* the invariant is written down in the plan ("a pending
   ADD/SUB has `X == sr.X` whenever X may be live") and the eager-vs-lazy test
   generators must draw entry states that satisfy it, or they report false
   failures.
4. **RAM code exits into ROM (coverage RecursiveSeparateRegions).** `ram_code.py`
   decodes RAM payloads after discovery and drops targets outside the payload.
   In recursive mode, ROM code reached only from RAM is never decoded. Latent
   today: all 22 configs use `coverage = "all_aligned"`. *Change:* RAM payloads
   are decoded first and their direct exits seed discovery
   (pipeline.als RamCodeBeforeDiscovery); with all-aligned-only discovery
   (PLAN D3) the hole cannot occur.
5. **RAM payloads need all-aligned decoding (coverage RamFollowFlow).** Following
   flow from the configured entry (pbobble3j/pbobble4j today) misses a computed
   transfer or a ROM jump into the payload's middle. *Change:* decode every
   aligned offset of a payload, as all-aligned ROM mode does; payloads are tiny.
6. **The decoder must be total (coverage AllAlignedPartialDecoder).** Today
   Capstone rejects some valid primary words on their extensions
   ("undecoded_valid_primary_entries"), which are holes in all-aligned builds.
   *Change:* `Decode.at` returns `Insn | Illegal of vector`, never "unknown";
   reserved encodings whose hardware behaviour is unclear are listed for 12
   (PLAN open question Q2).
7. **Heuristic jump tables are a coverage claim, not a soundness claim.**
   Devirtualized `switch` chains compare the runtime pc, so wrong candidates are
   harmless; missing candidates in recursive mode are holes
   (RecursiveHeuristicTables). *Change:* jump tables carry an `evidence`
   (`Configured`, `Bounded`, `Heuristic`); under all-aligned discovery the
   evidence only matters for the summary, and if recursive mode returns, a
   `Heuristic` site is a reported coverage gap.
8. **RAM functions stay one instruction, never chained into (Dispatch.tla).**
   Both relaxations run stale RAM code. *Change:* `Layout.check` rejects a RAM
   chain target or a multi-instruction RAM function.
9. **Sprite trace is not post-processing (pipeline.als).** Liveness pins the
   reset PCs and printing keeps their stores on the bus path, so the trace runs
   before liveness and printing.
10. **sprite_groups.json cannot simply become in-memory (pipeline.als
    NaiveJsonDropHasOrder).** `compile/enhancements.py` runs in another process
    and reads the file. *Change:* `emit` writes `enhancements.hpp`; a
    config-only build writes the default header (PLAN D6, tasks T18, T5, T22).

## Limits

The Alloy scopes are small (≤ 8 instruction nodes, 7 PCs); the TLA+ programs
are bounded (≤ 6 instructions, 5 device events, 3 abstract flags X/Z/C standing
for X, N/Z and V/C). The models check the rules, not the OCaml code; the plan's
property tests (exhaustive small CFGs, eager-vs-lazy C runs, Musashi
differential) carry the same properties to the implementation.
