open Sem
open Semantics_ops

let bits value mask = bin And value (int mask)
let add = bin Add
let sub = bin Sub
let either = bin Or
let eq = bin Eq
let zero value = eq value (int 0)
let negative value = bits value 0x80000000
let signed_less left right =
  let bias value = bin Xor value (int 0x80000000) in
  bin Lt (bias left) (bias right)

let sign_extend width value = match width with Long -> value | Byte | Word -> Sext (width, value)

let multiply_word sign operand dst =
  let extend = match sign with Insn.Signed -> sign_extend Word | Unsigned -> narrow Word in
  read Word 0 operand
  @ [Let (2, bin Mul (extend (Temp 0)) (extend (d dst)));
     Set_reg (D dst, Long, Temp 2);
     Flags (Logic { result = Temp 2; width = Long })]

let no_high_register = 8

let signed_argument = function Insn.Signed -> int 1 | Unsigned -> int 0

let multiply_long sign operand low high =
  let high = Option.value ~default:no_high_register high in
  read Long 0 operand
  @ [Helper (Mul_long { signed = signed_argument sign; src = Temp 0; low = int low; high = int high })]

let divide ctx width operand operation =
  read width 0 operand
  @ [If (zero (Temp 0), [Set_flags [C, int 0]; trap 5 ctx.next], [Helper operation])]

let read_modify_write width operand update =
  let setup, dst, finish = place width 1 operand in
  setup @ [read_place width 0 dst] @ finish @ update dst (Temp 0)

let decimal_flags result carry unadjusted =
  Set_flags
    [X, carry; C, carry; N, bits result 0x80;
     V, bits (bin And (bin Xor unadjusted (int 0xff)) result) 0x80;
     Z, bin And (zero result) (Flag Z)]

let decimal_pair (operation : binop) src dst =
  let setup, target, finish = place Byte 3 dst in
  let low value = bits value 0xf and high value = bits value 0xf0 in
  let adjust = match operation with Add -> sub | _ -> add in
  read Byte 0 src @ setup @ [read_place Byte 2 target] @ finish
  @ [Let (4, bin operation (bin operation (low (Temp 2)) (low (Temp 0))) (Flag X));
     Let (5, bin operation (Temp 4) (bin Mul (bin Lt (int 9) (Temp 4)) (int 6)));
     Let (6, add (Temp 5) (bin operation (high (Temp 2)) (high (Temp 0))));
     Let (7, bin Lt (int 0x99) (Temp 6));
     Let (8, narrow Byte (adjust (Temp 6) (bin Mul (Temp 7) (int 0xa0))));
     decimal_flags (Temp 8) (Temp 7) (Temp 4);
     write Byte target (Temp 8)]

let negate_decimal operand =
  read_modify_write Byte operand (fun dst original ->
      [Let (2, narrow Byte (sub (sub (int 0x9a) original) (Flag X)));
       If (eq (Temp 2) (int 0x9a),
           [Set_flags [X, int 0; C, int 0; N, int 1; V, int 0]],
           [Let (3, narrow Byte (add (Temp 2)
                (bin Mul (eq (bits (Temp 2) 0xf) (int 0xa)) (int 6))));
            decimal_flags (Temp 3) (int 1) (Temp 2);
            write Byte dst (Temp 3)])])

let pack src dst adjustment =
  let setup, unpacked = match src with
    | Insn.Predec _ ->
        read Byte 0 src @ read Byte 2 src, either (bin Shl (Temp 0) (int 8)) (Temp 2)
    | _ -> read Word 0 src, Temp 0
  in
  let target_setup, target, finish = place Byte 4 dst in
  setup @ [Let (5, add unpacked (int adjustment))] @ target_setup
  @ [write Byte target (either (bits (bin Shr (Temp 5) (int 4)) 0xf0) (bits (Temp 5) 0xf))]
  @ finish

let unpack src dst adjustment =
  let setup = read Byte 0 src
    @ [Let (2, add (either (bits (bin Shl (Temp 0) (int 4)) 0xf00) (bits (Temp 0) 0xf))
                   (int adjustment))]
  in
  let store width slot value =
    let setup, target, finish = place width slot dst in
    setup @ [write width target value] @ finish
  in
  setup @ (match dst with
    | Insn.Predec _ -> store Byte 3 (bin Shr (Temp 2) (int 8)) @ store Byte 4 (Temp 2)
    | _ -> store Word 3 (Temp 2))

let comparison width src dst result =
  [Let (result, narrow width (sub dst src));
   Flags (Cmp { src; dst; result = Temp result; width })]

let compare_and_swap width ({ compare; update } : Insn.cas_registers) operand =
  let setup, target, finish = place width 1 operand in
  setup @ finish @ [read_place width 0 target; Let (2, narrow width (d compare))]
  @ comparison width (Temp 2) (Temp 0) 3
  @ [If (zero (Temp 3), [write width target (d update)], [Set_reg (D compare, width, Temp 0)])]

let register_value : Insn.reg -> expr = function D reg -> d reg | A reg -> a reg

let compare_and_swap_pair width (first : Insn.cas_operand) (second : Insn.cas_operand) =
  let read_operand slot (operand : Insn.cas_operand) =
    [Let (slot, register_value operand.address);
     Load (slot + 1, width, Temp slot);
     Let (slot + 2, narrow width (d operand.registers.compare))]
  in
  let restore slot (operand : Insn.cas_operand) =
    (* The pinned core sign-extends CAS2.W loads addressed through An. *)
    let width, value = match width, operand.address with
      | Word, A _ -> Long, Sext (Word, Temp slot)
      | _ -> width, Temp slot
    in
    Set_reg (D operand.registers.compare, width, value)
  in
  let failed =
    (* A failed CAS2 with aliased compare registers returns the first operand. *)
    if first.registers.compare = second.registers.compare then [restore 1 first]
    else [restore 1 first; restore 5 second]
  in
  read_operand 0 first @ read_operand 4 second
  @ comparison width (Temp 2) (Temp 1) 3
  @ [If (zero (Temp 3),
        comparison width (Temp 6) (Temp 5) 7
        @ [If (zero (Temp 7),
               [Store (width, Temp 0, d first.registers.update);
                Store (width, Temp 4, d second.registers.update)], failed)],
        failed)]

let check ctx width operand reg =
  let value = Temp 2 in
  read width 0 operand
  @ [Let (2, sign_extend width (d reg));
     Set_flags [Z, zero value; V, int 0; C, int 0];
     If (either (negative value) (signed_less (sign_extend width (Temp 0)) value),
         [Set_flags [N, negative value]; trap 6 ctx.next], [])]

let bounds ctx kind width operand reg =
  let setup, addr = address ~slot:0 operand in
  let value = match reg with
    | Insn.D _ -> sign_extend width (register_value reg)
    | A _ -> register_value reg
  in
  let lower = sign_extend width (Temp 2) and upper = sign_extend width (Temp 3) in
  let below = signed_less (Temp 1) lower and above = signed_less upper (Temp 1) in
  (* Reversed limits describe a range that wraps around the signed endpoints. *)
  let reversed = signed_less upper lower in
  let outside = either (bin And below above) (bin And (zero reversed) (either below above)) in
  setup @ [Let (0, addr); Let (1, value);
           Load (2, width, Temp 0); Load (3, width, add (Temp 0) (int (bytes width)));
           Let (4, outside);
           Set_flags [Z, either (eq (Temp 1) lower) (eq (Temp 1) upper); C, Temp 4]]
  @ match kind with Insn.Check -> [If (Temp 4, [trap 6 ctx.next], [])] | Compare -> []

let of_op ctx op =
  let body stmts = Some (advance ctx stmts) in
  match (op : Insn.op) with
  | Mul_word (sign, src, dst) -> body (multiply_word sign src dst)
  | Mul_long { sign; operand; low; high } -> body (multiply_long sign operand low high)
  | Div_word (sign, operand, dst) ->
      body (divide ctx Word operand (Div_word { signed = signed_argument sign; src = Temp 0; dst = int dst }))
  | Div_long { sign; operand; dividend; quotient; remainder } ->
      let high = match dividend with Single _ -> no_high_register | Double { high; _ } -> high in
      body (divide ctx Long operand
              (Div_long { signed = signed_argument sign; src = Temp 0; high = int high;
                          quotient = int quotient; remainder = int remainder }))
  | Abcd (src, dst) -> body (decimal_pair Add src dst)
  | Sbcd (src, dst) -> body (decimal_pair Sub src dst)
  | Unary (Nbcd, Byte, operand) -> body (negate_decimal operand)
  | Unary (Tas, Byte, operand) ->
      body (read_modify_write Byte operand (fun dst result ->
          [Flags (Logic { result; width = Byte }); write Byte dst (either result (int 0x80))]))
  | Pack (src, dst, adjustment) -> body (pack src dst adjustment)
  | Unpk (src, dst, adjustment) -> body (unpack src dst adjustment)
  | Cas (width, regs, operand) -> body (compare_and_swap width regs operand)
  | Cas2 (width, first, second) -> body (compare_and_swap_pair width first second)
  | Chk (width, operand, reg) -> body (check ctx width operand reg)
  | Bounds (kind, width, operand, reg) -> body (bounds ctx kind width operand reg)
  | _ -> None
