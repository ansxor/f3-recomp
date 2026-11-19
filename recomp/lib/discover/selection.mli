type disposition =
  | Native of Insn.t
  | Exception_entry of int
  | Ram of Insn.t
  | Ram_exception of int

type tier = Hot | Cold
type mode = Baseline | Tiers | Slim
type profile = [ `Tiers of Profile.t | `Slim of Profile.t ]

type t = private {
  program : Program_core.t;
  config : Config.t;
  units : Emit_units.t;
  mode : mode;
  entries : (disposition * tier) Pc.Map.t;
}

type violation =
  | Undecoded_hook of { unit_name : string; kind : string; pc : int }
  | Dropped_hook of { unit_name : string; kind : string; pc : int }
  | Inaccessible_entry of int
  | No_entries
  | No_executable_hits

val of_program : profile:profile option -> Program_core.t -> t
val check : t -> (unit, violation list) result
val describe_violation : violation -> string
val chain_candidates : t -> Insn.t -> Flow.t -> int list
