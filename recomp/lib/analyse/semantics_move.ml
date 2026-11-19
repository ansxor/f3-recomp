(* Family a: moves, LEA/PEA, MOVEM, MOVEP, EXG, LINK/UNLK. *)
open Sem
open Semantics_ops

let move width src dst =
  let setup, dest, finish = place width 2 dst in
  let store = match width, dst, dest with
    | Insn.Long, Insn.Predec _, Memory addr -> Store_long_low_first (addr, Temp 0)
    | _ -> write width dest (Temp 0)
  in
  read width 0 src @ setup @ [store] @ finish @
  [Flags (Logic { result = Temp 0; width })]

let of_op ctx : Insn.op -> Sem.t option = function
  | Move (width, src, dst) -> Some (advance ctx (move width src dst))
  | Movea (width, src, dst) ->
      Some (advance ctx (read width 0 src @ [Set_reg (A dst, Long, Sext (width, Temp 0))]))
  | Moveq (value, dst) ->
      Some (advance ctx [Set_reg (D dst, Long, int value); Flags (Logic { result = int value; width = Long })])
  | _ -> None
