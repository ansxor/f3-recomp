(* Builders shared by the per-family semantics modules. *)
open Sem

type place = Register of register | Memory of expr

exception Unsupported_operand

(* What a family needs to know about the instruction it describes. *)
type ctx = { pc : Pc.t; next : Pc.t }

let bin op a b = Bin (op, a, b)
let int n = Const n
let d n = Reg (D n)
let a n = Reg (A n)
let goto pc = Goto (int pc)
let advance ctx body = Body (body @ [goto ctx.next])
let trap vector return_pc = Raise { vector; return_pc }
let supervisor ctx body = Body [If (Supervisor, body, [trap 8 ctx.pc])]

(* The register reads and ALU temporaries are already unsigned 32-bit values. *)
let narrow width value =
  match width with Insn.Long -> value | Byte | Word -> truncate width value

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
