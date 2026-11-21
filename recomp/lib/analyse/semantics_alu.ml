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
  (* CMPM can use the same address register for both postincrements. *)
  read width 0 src @ setup @
  [read_place width 2 dest; Let (4, narrow width (bin op dst_value src_value))] @
  store @ finish @ [Flags flags]

let address_alu operation width src reg =
  let source = match width with
    | Word -> Sext (Word, Temp 0)
    | Byte | Long -> Temp 0
  in
  let dst = a reg in
  (* Source auto-update takes effect before reading the destination register. *)
  read width 0 src @
  match operation with
  | `Add -> [Set_reg (A reg, Long, bin Add dst source)]
  | `Sub -> [Set_reg (A reg, Long, bin Sub dst source)]
  | `Cmp ->
      [Let (2, source); Let (3, bin Sub dst (Temp 2));
       Flags (Cmp { src = Temp 2; dst; result = Temp 3; width = Long })]

(* Split the operation into two width-limited steps so carry and borrow
   also work at long width without a wider intermediate. *)
let with_extend operation width src dst store =
  let intermediate = Temp 4 and result = Temp 5 in
  let (op, carry, overflow : binop * expr * expr) =
    match operation with
    | `Add ->
        Add, bin Or (bin Lt intermediate src) (bin Lt result intermediate),
        bin And (bin Xor src result) (bin Xor dst result)
    | `Sub ->
        Sub, bin Or (bin Lt dst src) (bin Lt intermediate result),
        bin And (bin Xor dst src) (bin Xor dst result)
  in
  let negative value = bin Ne (bin And value (int (sign width))) (int 0) in
  [Let (4, narrow width (bin op dst src));
   Let (5, narrow width (bin op intermediate (Flag X)))] @ store result @
  (* A zero result preserves Z across a multi-precision operation. *)
  [Set_flags [X, carry; C, carry; V, negative overflow;
              N, negative result; Z, bin And (Flag Z) (bin Eq result (int 0))]]

let extended_alu operation width src dst =
  let setup, dest, finish = place width 3 dst in
  read width 0 src @ setup @ [read_place width 2 dest] @
  with_extend operation width (Temp 0) (Temp 2)
    (fun result -> write width dest result :: finish)

let modify operation width operand =
  let setup, dest, finish = place width 1 operand in
  let src = Temp 0 and result = Temp 2 in
  let store value = write width dest value :: finish in
  let compute value flags =
    read_place width 0 dest :: Let (2, value) :: (store result @ [Flags flags])
  in
  setup @
  match operation with
  | `Clr -> store (int 0) @ [Flags (Logic { result = int 0; width })]
  | `Not -> compute (bin Xor src (int (mask width))) (Logic { result; width })
  | `Neg -> compute (narrow width (bin Sub (int 0) src))
      (Sub { src; dst = int 0; result; width })
  | `Negx -> read_place width 0 dest :: with_extend `Sub width src (int 0) store

let data_result width reg value =
  [Let (0, value); Set_reg (D reg, width, Temp 0);
   Flags (Logic { result = Temp 0; width })]

let extend kind reg =
  let src, dst = match kind with
    | Insn.Byte_to_word -> Byte, Word
    | Byte_to_long -> Byte, Long
    | Word_to_long -> Word, Long
  in
  data_result dst reg (narrow dst (Sext (src, d reg)))

let swap reg =
  let value = d reg and half = int 16 in
  data_result Long reg (bin Or (bin Shl value half) (bin Shr value half))

let of_op ctx : Insn.op -> Sem.t option =
  let next body = Some (advance ctx body) in
  function
  | Alu (op, width, src, dst) -> next (alu op width src dst)
  | Adda (width, src, reg) -> next (address_alu `Add width src reg)
  | Suba (width, src, reg) -> next (address_alu `Sub width src reg)
  | Cmpa (width, src, reg) -> next (address_alu `Cmp width src reg)
  | Addx (width, src, dst) -> next (extended_alu `Add width src dst)
  | Subx (width, src, dst) -> next (extended_alu `Sub width src dst)
  | Unary (Clr, width, dst) -> next (modify `Clr width dst)
  | Unary (Not, width, dst) -> next (modify `Not width dst)
  | Unary (Neg, width, dst) -> next (modify `Neg width dst)
  | Unary (Negx, width, dst) -> next (modify `Negx width dst)
  | Unary (Tst, width, src) ->
      next (read width 0 src @ [Flags (Logic { result = Temp 0; width })])
  | Ext (kind, reg) -> next (extend kind reg)
  | Swap reg -> next (swap reg)
  | _ -> None
