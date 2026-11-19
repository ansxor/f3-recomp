(* Family b: ADD/SUB/CMP/AND/OR/EOR/NEG/NOT/CLR/TST/EXT/SWAP, quick and
   immediate forms, ADDX/SUBX/NEGX. *)
open Sem
open Semantics_ops

let alu operation width src dst =
  let setup, dest, finish = place width 3 dst in
  let op : Sem.binop = match operation with
    | Insn.Add -> Add | Sub | Cmp -> Sub | And -> And | Or -> Or | Eor -> Xor in
  let src_value = Temp 0 and dst_value = Temp 2 and result = Temp 4 in
  let arithmetic = { src = src_value; dst = dst_value; result; width } in
  let flags = match operation with
    | Insn.Add -> Add arithmetic | Sub -> Sub arithmetic | Cmp -> Cmp arithmetic
    | And | Or | Eor -> Logic { result; width }
  in
  let store = if operation = Insn.Cmp then [] else [write width dest result] in
  read width 0 src @ setup @
  [read_place width 2 dest; Let (4, narrow width (bin op dst_value src_value))] @
  store @ finish @ [Flags flags]

let addx width src dst =
  let setup, dest, finish = place width 3 dst in
  let src_value = Temp 0 and dst_value = Temp 2 and sum = Temp 4 and result = Temp 5 in
  let carry = bin Or (bin Lt sum src_value) (bin Lt result sum) in
  let overflow = bin And (bin Xor src_value result) (bin Xor dst_value result) in
  let negative value = bin Ne (bin And value (int (sign width))) (int 0) in
  read width 0 src @ setup @
  [read_place width 2 dest;
   Let (4, narrow width (bin Add dst_value src_value));
   Let (5, narrow width (bin Add sum (Flag X)));
   write width dest result] @ finish @
  [Set_flags [X, carry; C, carry; V, negative overflow;
              N, negative result; Z, bin And (Flag Z) (bin Eq result (int 0))]]

let of_op ctx : Insn.op -> Sem.t option = function
  | Alu (op, width, src, dst) -> Some (advance ctx (alu op width src dst))
  | Addx (width, src, dst) -> Some (advance ctx (addx width src dst))
  | _ -> None
