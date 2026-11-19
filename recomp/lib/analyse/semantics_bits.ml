(* Family c: shifts/rotates, bit ops, bitfields, Scc. *)
open Sem
open Semantics_ops

let of_op ctx : Insn.op -> Sem.t option = function
  | Bit (Btst, count, ea) ->
      let width = match ea with Insn.Register (D _) -> Insn.Long | _ -> Byte in
      let bit = match count with Insn.Constant bit -> int bit | Data_register reg -> d reg in
      Some (advance ctx (read width 0 ea @ [Helper (Btst { value = Temp 0; bit; bytes = int (bytes width) })]))
  | _ -> None
