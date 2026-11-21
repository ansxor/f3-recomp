type shape =
  | Spin
  | Poll_bcc of { address : Sem.expr; size : Insn.size; cond : Insn.cond }
  | Affine of { word : Sem.expr }

(* The path starts at the head and ends at the instruction that branches back. *)
type t = { head : int; path : int list; shape : shape }

type rule = Head | Native_nodes | Cycle | Body_effects | Shape_parameters
type violation = { head : int; rule : rule }

val find : Program.t -> native:(int -> bool) -> t Pc.Map.t
val check_all : Program.t -> Cfg.t -> t Pc.Map.t -> (unit, violation list) result
val describe_violation : violation -> string
val to_tsv : shape -> string
