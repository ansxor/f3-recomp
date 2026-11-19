(* Family d: MUL/DIV, BCD, PACK/UNPK, CAS/CAS2, TAS, CHK/CHK2/CMP2. *)

let of_op (_ : Semantics_ops.ctx) : Insn.op -> Sem.t option = function
  | _ -> None
