(* Shifts, rotates, bit operations, bitfields and Scc. *)
open Sem
open Semantics_ops

let count_value = function
  | Insn.Constant value -> int value
  | Data_register reg -> d reg

let shift ctx operation width count ea =
  let setup, dst, finish = place width 1 ea in
  let nonzero_count = match count with
    | Insn.Constant value -> value land 63 <> 0
    | Data_register _ -> false
  in
  advance ctx
    (setup @ [read_place width 0 dst;
              Helper (Shift { operation; value = Temp 0; count = count_value count;
                              bytes = int (bytes width); nonzero_count; result = 2 });
              write width dst (Temp 2)] @ finish)

let bit ctx operation count ea =
  let width = match ea with Insn.Register (D _) -> Insn.Long | _ -> Byte in
  let bit = count_value count in
  let test = Helper (Btst { value = Temp 0; bit; bytes = int (bytes width) }) in
  let bit_mask = bin Shl (int 1) (bin And bit (int (8 * bytes width - 1))) in
  let update = match operation with
    | Insn.Btst -> None
    | Bchg -> Some (bin Xor (Temp 0) bit_mask)
    | Bclr -> Some (bin And (Temp 0) (bin Xor bit_mask (int (mask width))))
    | Bset -> Some (bin Or (Temp 0) bit_mask)
  in
  match update with
  | None -> advance ctx (read width 0 ea @ [test])
  | Some value ->
      let setup, dst, finish = place width 1 ea in
      advance ctx (setup @ [read_place width 0 dst; test; write width dst value] @ finish)

let bitfield ctx operation offset width operand =
  let operation, result_reg = match operation with
    | Insn.Bftst -> 0, 0 | Bfextu reg -> 1, reg
    | Bfchg -> 2, 0 | Bfexts reg -> 3, reg
    | Bfclr -> 4, 0 | Bfffo reg -> 5, reg
    | Bfset -> 6, 0 | Bfins reg -> 7, reg
  in
  let setup, memory, location = match operand with
    | Insn.Register (D reg) -> [], 0, int reg
    | ea ->
        let setup, location = address ~slot:1 ea in
        setup, 1, location
  in
  advance ctx
    (setup @ [Helper (Bitfield { operation = int operation; memory = int memory; location;
                                offset = count_value offset; width = count_value width;
                                result_reg = int result_reg })])

let scc ctx condition ea =
  let setup, dst, finish = place Byte 1 ea in
  let value = match condition with
    | Insn.T -> int 0xff
    | F -> int 0
    | _ -> bin Mul (Cond condition) (int 0xff)
  in
  advance ctx (setup @ [write Byte dst value] @ finish)

let of_op ctx : Insn.op -> Sem.t option = function
  | Shift (operation, width, count, ea) -> Some (shift ctx operation width count ea)
  | Bit (operation, count, ea) -> Some (bit ctx operation count ea)
  | Bitfield { operation; offset; width; operand } ->
      Some (bitfield ctx operation offset width operand)
  | Scc (condition, ea) -> Some (scc ctx condition ea)
  | _ -> None
