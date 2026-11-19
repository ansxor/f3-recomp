type scan = Relative_words | Absolute_pointers | Register_pointers
type evidence =
  | Configured
  | Bounded of { compare_pc : Pc.t; limit : int }
  | Heuristic of scan

type t = { site : Pc.t; candidates : Pc.t list; evidence : evidence }

val configured : Code_space.t -> Image.t -> Config.jump_table -> t option
val scan : Code_space.t -> Insn.t -> t option

(** Validate the site, sorted executable ROM candidates, and any recorded guard. *)
val check : t -> Code_space.t -> unit
