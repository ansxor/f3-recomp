open Sem

type place = Register of register | Memory of expr
exception Unsupported_operand

let bin op a b = Bin (op, a, b)
let int n = Const n
let d n = Reg (D n)
let a n = Reg (A n)

(* The register reads and ALU temporaries are already unsigned 32-bit values. *)
let narrow width value =
  match width with Long -> value | Byte | Word -> truncate width value

let address = function
  | Insn.Ind reg -> a reg
  | Disp { reg; disp } -> bin Add (a reg) (int disp)
  | Absolute addr | Pc_rel addr -> int addr
  | ea -> (match Insn.static_address ea with Some addr -> int addr | None -> raise Unsupported_operand)

let place width slot = function
  | Insn.Register (D reg) -> [], Register (D reg), []
  | Register (A reg) -> [], Register (A reg), []
  | Predec reg ->
      [Set_reg (A reg, Long, bin Sub (a reg) (int (Insn.auto_step width reg)));
       Let (slot, a reg)], Memory (Temp slot), []
  | Postinc reg ->
      [Let (slot, a reg)], Memory (Temp slot),
      [Set_reg (A reg, Long, bin Add (Temp slot) (int (Insn.auto_step width reg)))]
  | Immediate _ -> raise Unsupported_operand
  | ea -> [Let (slot, address ea)], Memory (Temp slot), []

let read_place width slot = function
  | Register reg -> Let (slot, narrow width (Reg reg))
  | Memory addr -> Load (slot, width, addr)

let read width slot = function
  | Insn.Immediate value -> [Let (slot, int (value land mask width))]
  | ea ->
      let setup, src, finish = place width (slot + 1) ea in
      setup @ [read_place width slot src] @ finish

let write width dst value = match dst with
  | Register reg -> Set_reg (reg, width, value)
  | Memory addr -> Store (width, addr, value)

let move width src dst =
  let setup, dest, finish = place width 2 dst in
  let store = match width, dst, dest with
    | Long, Insn.Predec _, Memory addr -> Store_long_low_first (addr, Temp 0)
    | _ -> write width dest (Temp 0)
  in
  read width 0 src @ setup @ [store] @ finish @
  [Flags (Logic { result = Temp 0; width })]

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

let of_insn { Insn.pc; length; op } =
  let next = Insn.address (pc + length) in
  let goto pc = Goto (int pc) in
  let advance body = Body (body @ [goto next]) in
  let trap vector return_pc = Raise { vector; return_pc } in
  let supervisor body =
    Body [If (Supervisor, body, [trap 8 pc])]
  in
  try
    match op with
    | Move (width, src, dst) -> advance (move width src dst)
    | Movea (width, src, dst) ->
        advance (read width 0 src @ [Set_reg (A dst, Long, Sext (width, Temp 0))])
    | Moveq (value, dst) ->
        advance [Set_reg (D dst, Long, int value); Flags (Logic { result = int value; width = Long })]
    | Alu (op, width, src, dst) -> advance (alu op width src dst)
    | Addx (width, src, dst) -> advance (addx width src dst)
    | Bra target -> Body [goto target]
    | Bcc (cond, target) -> Body [If (Cond cond, [goto target], [goto next])]
    | Dbcc (cond, reg, target) ->
        let decrement = Let (0, truncate Word (bin Sub (d reg) (int 1))) in
        Body [If (Cond cond, [goto next],
                  [decrement; Set_reg (D reg, Word, Temp 0);
                   If (bin Eq (Temp 0) (int 0xffff), [goto next], [goto target])])]
    | Jsr ea ->
        let setup, target = match address ea with
          | Const _ as target -> [], target
          | target -> [Let (0, target)], Temp 0
        in
        Body (setup @ [Set_reg (A 7, Long, bin Sub (a 7) (int 4));
                       Store (Long, a 7, int next); Goto target])
    | Rts ->
        Body [Load (0, Long, a 7); Set_reg (A 7, Long, bin Add (a 7) (int 4)); Goto (Temp 0)]
    | Trap number -> Body [trap (32 + number) next]
    | Trapv -> Body [If (Cond Vs, [trap 7 next], [goto next])]
    | Trapcc (cond, _) -> Body [If (Cond cond, [trap 7 next], [goto next])]
    | Bit (Btst, count, ea) ->
        let width = match ea with Insn.Register (D _) -> Long | _ -> Byte in
        let bit = match count with Insn.Constant bit -> int bit | Data_register reg -> d reg in
        advance (read width 0 ea @ [Helper (Btst { value = Temp 0; bit; bytes = int (bytes width) })])
    | From_status (Ccr, dst) ->
        let setup, dest, finish = place Word 0 dst in
        advance (setup @ [write Word dest Ccr] @ finish)
    | From_status (Sr, dst) ->
        let setup, dest, finish = place Word 0 dst in
        supervisor (setup @ [write Word dest Sr] @ finish @ [goto next])
    | To_status (Ccr, src) -> advance (read Word 0 src @ [Set_ccr (Temp 0)])
    | To_status (Sr, src) ->
        supervisor (read Word 0 src @ [Runtime (Set_sr (Temp 0)); goto next])
    | Move_usp (From_control, reg) ->
        supervisor [Set_reg (A reg, Long, Reg Usp); goto next]
    | Move_usp (To_control, reg) ->
        supervisor [Set_reg (Usp, Long, a reg); goto next]
    | Reset -> supervisor [Runtime Reset_devices; goto next]
    | Stop sr -> supervisor [Runtime (Set_sr (int sr)); Stop next]
    | Nop -> advance []
    | _ -> Unsupported "instruction family is outside the semantic core"
  with Unsupported_operand -> Unsupported "effective address is outside the semantic core"

let of_decoded ~pc : Decode.decoded -> Sem.t = function
  | Insn insn -> of_insn insn
  | Illegal { reason = Truncated; _ } -> Unsupported "truncated instruction"
  | Illegal { vector; _ } -> Body [Raise { vector; return_pc = pc }]
