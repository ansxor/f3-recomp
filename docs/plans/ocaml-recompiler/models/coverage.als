/*
 * Coverage: can an executed PC end up with no native entry, no exception
 * entry and no justified exclusion (a "hole", fatal at run time)?
 *
 * Pc atoms are word-aligned addresses in ROM or in a RAM-code payload.
 * `decodes` is what the decoder says about the word at a PC; `Unknown` models a
 * partial decoder (Capstone today: valid primary word rejected on extensions,
 * "undecoded_valid_primary_entries"). `direct` are transfers encoded in the
 * instruction; `actual` are transfers the CPU can really take; the extra
 * actual edges of a computed jmp/jsr are not visible to the recompiler.
 * `scanned` are jump-table candidates; a site in `Bounded` carries a proof
 * (index compare + branch before the table) that its candidates cover every
 * actual target. Heuristic scans have no such proof.
 */
module coverage

abstract sig Decoded {}
one sig Insn, Illegal, Unknown extends Decoded {}

abstract sig Strategy {}
one sig FollowFlow, AllAligned extends Strategy {}

one sig Build {
  rom: one Strategy,        -- [discovery] coverage = recursive | all_aligned
  ram: one Strategy,        -- how RAM payloads are decoded
  slim: lone Build,         -- opt-in slim profile build (cold code removed)
  joint: lone Build         -- RAM payload edges seed ROM discovery (proposed)
}

sig Pc {
  decodes: one Decoded,
  direct: set Pc,
  actual: set Pc,
  scanned: set Pc
}
sig RamPc in Pc {}
sig Excluded in Pc {}       -- [[exclude]] with reason/evidence; ROM only
sig Computed in Pc {}       -- computed jmp/jsr sites
sig Bounded in Computed {}  -- sites whose table scan carries a bound proof
sig Seed in Pc {}           -- vectors, configured entries, RAM routine entries
sig Profiled in Pc {}       -- profile hits
sig Hook in Pc {}           -- emit-unit start/end PCs (must stay native)

fact wellFormed {
  no Excluded & RamPc
  -- direct edges are real; only computed sites add edges the decoder cannot see
  all p: Pc | p.direct in p.actual
  all p: Pc - Computed | p.actual = p.direct
  all p: Bounded | p.actual in p.direct + p.scanned
  -- control never moves between ROM and a RAM payload except by a direct call or return
  -- (a call into RAM, a return out); keep regions closed under actual edges otherwise
  all p: Pc | p.decodes != Insn implies no p.actual
  -- exclusions are honest: nothing executed lands in them (a violated exclusion is a
  -- deliberate fatal stop, reported separately)
  no Seed & Excluded
}

-- Executing an excluded PC is a deliberate fatal stop: paths end there.
fun executed: set Pc { Seed.*(actual :> (Pc - Excluded)) - Excluded }

fun discoveredRam: set Pc {
  Build.ram = AllAligned
    implies RamPc
    else (Seed & RamPc).*((direct + scanned) :> RamPc)
}
-- ram_code.py today decodes RAM after ROM discovery and drops RAM->ROM targets;
-- the joint build lets every decoded RAM instruction's targets seed ROM discovery.
fun romRoots: set Pc {
  (Seed - RamPc) + (some Build.joint implies discoveredRam.(direct + scanned) - RamPc else none)
}
fun discoveredRom: set Pc {
  Build.rom = AllAligned
    implies (Pc - RamPc - Excluded)
    else (romRoots - Excluded).*((direct + scanned) :> (Pc - RamPc - Excluded))
}
-- A decoded PC becomes an entry when the decoder classifies its word; Illegal
-- words become exception entries (line A/F, ILLEGAL, unassigned opcodes).
fun entries: set Pc { (discoveredRom + discoveredRam) & decodes.(Insn + Illegal) }

fun registered: set Pc {
  some Build.slim implies (entries & (Profiled + RamPc + Hook)) else entries
}
fun holes: set Pc { executed - registered }

-- The plan's rules for a hole-free build.
pred totalDecoder { no decodes.Unknown }
pred provenTables { Computed in Bounded }

-- 1. All-aligned ROM + all-aligned RAM + total decoder: no hole, any program.
AllAlignedTotalNoHoles: check {
  (Build.rom = AllAligned and Build.ram = AllAligned and totalDecoder and no Build.slim)
    implies no holes
} for 7

-- 2. Expected counterexample: all-aligned with a partial decoder leaves holes.
AllAlignedPartialDecoder: check {
  (Build.rom = AllAligned and Build.ram = AllAligned and no Build.slim) implies no holes
} for 7

-- 3. Expected counterexample (found by this model): recursive ROM discovery run
--    before and apart from RAM code misses ROM code entered only from RAM.
RecursiveSeparateRegions: check {
  (Build.rom = FollowFlow and Build.ram = AllAligned and no Build.joint and totalDecoder
     and provenTables and no Build.slim)
    implies no holes
} for 7

-- 3b. Recursive discovery is hole-free when tables are proven and RAM edges seed ROM.
RecursiveProvenJointNoHoles: check {
  (Build.rom = FollowFlow and Build.ram = AllAligned and some Build.joint and totalDecoder
     and provenTables and no Build.slim)
    implies no holes
} for 7

-- 4. Expected counterexample: a heuristic table scan under-approximates.
RecursiveHeuristicTables: check {
  (Build.rom = FollowFlow and Build.ram = AllAligned and some Build.joint and totalDecoder and no Build.slim)
    implies no holes
} for 7

-- 5. Expected counterexample: following flow inside a RAM payload misses a
--    computed transfer that stays inside the payload (ram_code.py today).
RamFollowFlow: check {
  (Build.rom = AllAligned and Build.ram = FollowFlow and totalDecoder and no Build.slim)
    implies no holes
} for 7

-- 6. Slim builds: holes are exactly executed-but-unprofiled PCs (the documented
--    cold abort); RAM entries and hooks are never among them.
SlimHolesAreColdOnly: check {
  (Build.rom = AllAligned and Build.ram = AllAligned and totalDecoder and some Build.slim)
    implies holes in executed - Profiled - RamPc - Hook
} for 7

-- 7. Registration tables the runtime accepts: entries never overlap exclusions.
EntriesAvoidExclusions: check { no registered & Excluded } for 7
