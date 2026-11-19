/*
 * Pipeline stage ordering: does some order run every stage after the stages
 * that make what it needs, in each build mode CMake supports, given that
 * in-memory values cannot cross process boundaries (files can)?
 *
 * Three designs:
 *   Py*    the stage graph of recomp/generate.py + cmake/codegen.cmake today
 *   Naive* the user's decision "sprite_groups.json becomes in-memory" applied
 *          without moving anything else (enhancements stays a separate generator)
 *   Oc*    the plan's OCaml pipeline
 */
module pipeline

abstract sig Data { file: lone Data }   -- `file = this` marks a file on disk
abstract sig Proc {}
one sig EmitProc, EnhanceProc, RomsProc, UnitsProc extends Proc {}
abstract sig Mode {}
one sig WithRom, ConfigOnly extends Mode {}

one sig Config, Rom, Exclusions, Profile, EmitUnits,
        Program,        -- decoded ROM instructions, direct edges, functions, tables
        RamRoutines,    -- decoded RAM payloads (+ their direct exits into ROM)
        Selection,      -- retained entries, tiers, exception entries
        SpriteGroups,   -- writers + reset PCs (sprite_trace), in memory
        FallbackSet,    -- PCs lowering cannot translate
        Semantics,      -- per-PC semantic IR, independent of liveness
        LiveOut, IdleLoops, Lowered, Layout,
        ProgramC, SpriteGroupsHpp, SpriteGroupsJson, EnhancementsHpp,
        RomManifestHpp, GameVideoHpp, SpriteUnitsH
  extends Data {}

fact files {
  file = (Config + Rom + Profile + ProgramC + SpriteGroupsHpp + SpriteGroupsJson + EnhancementsHpp
          + RomManifestHpp + GameVideoHpp + SpriteUnitsH) <: iden
}

abstract sig Stage {
  needs: set Data,
  makes: set Data,
  runs: set Mode,
  proc: one Proc,
  rank: one Int
}

-- Files the build consumes in each mode (cmake/codegen.cmake, recomp/CMakeLists.txt).
fun consumed[m: Mode]: set Data {
  (m = WithRom implies ProgramC + SpriteGroupsHpp else none)
  + EnhancementsHpp + RomManifestHpp + GameVideoHpp + SpriteUnitsH
}

pred validOrder[family: set Stage] {
  all m: Mode {
    let active = { s: family | m in s.runs } {
      all s: active, d: s.needs | some t: active {
        d in t.makes
        t.rank < s.rank
        no d.file implies t.proc = s.proc        -- in-memory values stay in one process
      }
      all d: consumed[m] | one t: active | d in t.makes
    }
  }
}

-- ---------------------------------------------------------------- Python today
abstract sig PyStage extends Stage {}
one sig PyLoad, PyDiscover, PyRamCode, PySelect, PyLowerProbe, PySpriteTrace,
        PyLiveness, PyIdle, PyLower, PyPack, PyPrint,
        PyEnhancements, PyCompileRoms, PySpriteUnits, PyConfig extends PyStage {}

fact pythonStages {
  PyConfig.needs = none and PyConfig.makes = Config + Rom + Profile
  PyLoad.needs = Config + Rom and PyLoad.makes = Exclusions + EmitUnits
  PyDiscover.needs = Config + Rom + Exclusions and PyDiscover.makes = Program
  PyRamCode.needs = Config + Rom and PyRamCode.makes = RamRoutines
  PySelect.needs = Program + Profile + EmitUnits + RamRoutines and PySelect.makes = Selection
  PyLowerProbe.needs = Selection and PyLowerProbe.makes = FallbackSet   -- first lower() pass
  PySpriteTrace.needs = Program + Config and PySpriteTrace.makes = SpriteGroups
  PyLiveness.needs = Selection + FallbackSet + SpriteGroups + EmitUnits + RamRoutines
  PyLiveness.makes = LiveOut
  PyIdle.needs = Program + Selection + LiveOut and PyIdle.makes = IdleLoops
  PyLower.needs = Selection + LiveOut and PyLower.makes = Lowered         -- second lower() pass
  PyPack.needs = Selection + Program and PyPack.makes = Layout
  PyPrint.needs = Lowered + Layout + IdleLoops + SpriteGroups + EmitUnits + RamRoutines + Exclusions
  PyPrint.makes = ProgramC + SpriteGroupsHpp + SpriteGroupsJson
  PyEnhancements.needs = Config + SpriteGroupsJson and PyEnhancements.makes = EnhancementsHpp
  PyCompileRoms.needs = Config and PyCompileRoms.makes = RomManifestHpp + GameVideoHpp
  PySpriteUnits.needs = Config and PySpriteUnits.makes = SpriteUnitsH
  (PyStage - PyEnhancements - PyEnhancementsDefault - PyCompileRoms - PySpriteUnits - PyConfig).proc = EmitProc
  PyEnhancements.proc = EnhanceProc and PyCompileRoms.proc = RomsProc and PySpriteUnits.proc = UnitsProc
  PyConfig.proc = EmitProc
  all s: PyStage - PyCompileRoms - PySpriteUnits - PyConfig - PyEnhancements - PyEnhancementsDefault | s.runs = WithRom
  PyConfig.runs = Mode and PyCompileRoms.runs = Mode and PySpriteUnits.runs = Mode
  -- ConfigOnly: compile/enhancements.py runs without groups and writes defaults
  PyEnhancements.runs = WithRom
}
one sig PyEnhancementsDefault extends PyStage {}
fact { PyEnhancementsDefault.needs = Config and PyEnhancementsDefault.makes = EnhancementsHpp
       PyEnhancementsDefault.runs = ConfigOnly and PyEnhancementsDefault.proc = EnhanceProc }

-- ---------------------------------------------------------------- naive JSON drop
abstract sig NaiveStage extends Stage {}
one sig NaiveEmit, NaiveEnhancements, NaiveEnhancementsDefault, NaiveCompileRoms,
        NaiveSpriteUnits, NaiveConfig extends NaiveStage {}
fact naiveStages {
  NaiveConfig.needs = none and NaiveConfig.makes = Config + Rom + Profile and NaiveConfig.runs = Mode
  NaiveConfig.proc = EmitProc
  NaiveEmit.needs = Config + Rom + Profile and NaiveEmit.makes = SpriteGroups + ProgramC + SpriteGroupsHpp
  NaiveEmit.runs = WithRom and NaiveEmit.proc = EmitProc
  NaiveEnhancements.needs = Config + SpriteGroups and NaiveEnhancements.makes = EnhancementsHpp
  NaiveEnhancements.runs = WithRom and NaiveEnhancements.proc = EnhanceProc
  NaiveEnhancementsDefault.needs = Config and NaiveEnhancementsDefault.makes = EnhancementsHpp
  NaiveEnhancementsDefault.runs = ConfigOnly and NaiveEnhancementsDefault.proc = EnhanceProc
  NaiveCompileRoms.needs = Config and NaiveCompileRoms.makes = RomManifestHpp + GameVideoHpp
  NaiveCompileRoms.runs = Mode and NaiveCompileRoms.proc = RomsProc
  NaiveSpriteUnits.needs = Config and NaiveSpriteUnits.makes = SpriteUnitsH
  NaiveSpriteUnits.runs = Mode and NaiveSpriteUnits.proc = UnitsProc
}

-- ---------------------------------------------------------------- OCaml plan
abstract sig OcStage extends Stage {}
one sig OcConfig, OcLoad, OcRamCode, OcDiscover, OcSelect, OcSpriteTrace, OcSemantics,
        OcLiveness, OcIdle, OcSpecialise, OcLayout, OcPrint, OcEnhancementsDefault,
        OcCompileRoms, OcSpriteUnits extends OcStage {}

fact ocamlStages {
  OcConfig.needs = none and OcConfig.makes = Config + Rom + Profile
  OcLoad.needs = Config + Rom and OcLoad.makes = Exclusions + EmitUnits
  -- RAM payloads first: their direct exits into ROM seed discovery (coverage.als)
  OcRamCode.needs = Config + Rom and OcRamCode.makes = RamRoutines
  OcDiscover.needs = Config + Rom + Exclusions + RamRoutines and OcDiscover.makes = Program
  OcSelect.needs = Program + RamRoutines + Profile + EmitUnits and OcSelect.makes = Selection
  OcSpriteTrace.needs = Program + Config and OcSpriteTrace.makes = SpriteGroups
  -- one semantics pass; the fallback set is a property of the semantics
  OcSemantics.needs = Selection and OcSemantics.makes = Semantics + FallbackSet
  OcLiveness.needs = Selection + Semantics + FallbackSet + SpriteGroups + EmitUnits + RamRoutines
  OcLiveness.makes = LiveOut
  OcIdle.needs = Program + Selection + LiveOut and OcIdle.makes = IdleLoops
  OcSpecialise.needs = Semantics + LiveOut and OcSpecialise.makes = Lowered
  OcLayout.needs = Selection + Program and OcLayout.makes = Layout
  -- emit writes every ROM-derived header, including enhancements.hpp
  OcPrint.needs = Lowered + Layout + IdleLoops + SpriteGroups + EmitUnits + Exclusions + Config + RamRoutines
  OcPrint.makes = ProgramC + SpriteGroupsHpp + EnhancementsHpp
  OcEnhancementsDefault.needs = Config and OcEnhancementsDefault.makes = EnhancementsHpp
  OcCompileRoms.needs = Config and OcCompileRoms.makes = RomManifestHpp + GameVideoHpp
  OcSpriteUnits.needs = Config and OcSpriteUnits.makes = SpriteUnitsH
  (OcStage - OcEnhancementsDefault - OcCompileRoms - OcSpriteUnits).proc = EmitProc
  OcEnhancementsDefault.proc = EnhanceProc and OcCompileRoms.proc = RomsProc
  OcSpriteUnits.proc = UnitsProc
  all s: OcStage - OcConfig - OcEnhancementsDefault - OcCompileRoms - OcSpriteUnits | s.runs = WithRom
  OcConfig.runs = Mode and OcCompileRoms.runs = Mode and OcSpriteUnits.runs = Mode
  OcEnhancementsDefault.runs = ConfigOnly
}

-- Expected SAT: today's pipeline works because enhancements reads sprite_groups.json.
PythonHasOrder: run { validOrder[PyStage] } for 5 Int
-- Expected UNSAT: dropping the JSON while keeping a separate enhancements generator.
NaiveJsonDropHasOrder: run { validOrder[NaiveStage] } for 5 Int
-- Expected SAT: the plan's pipeline has an order in both modes.
OcamlHasOrder: run { validOrder[OcStage] } for 5 Int
-- Sprite trace is not post-processing: liveness pins its reset PCs, printing keeps
-- their stores on the bus path.
SpriteTraceBeforeLiveness: check {
  validOrder[OcStage] implies OcSpriteTrace.rank < OcLiveness.rank and OcSpriteTrace.rank < OcPrint.rank
} for 5 Int
RamCodeBeforeDiscovery: check { validOrder[OcStage] implies OcRamCode.rank < OcDiscover.rank } for 5 Int

-- Sanity: the facts alone are consistent (guards against a vacuous UNSAT above).
FactsConsistent: run {} for 5 Int
