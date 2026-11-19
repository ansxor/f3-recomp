(* Family a: moves, LEA/PEA, MOVEM, MOVEP, EXG, LINK/UNLK. *)
open Sem
open Semantics_ops

let store_predecrement width addr value =
  match width with
  | Long -> Store_long_low_first (addr, value)
  | _ -> Store (width, addr, value)

let move width src dst =
  let source = read width 0 src in
  let setup, dest, finish = place width 2 dst in
  let store = match dst, dest with
    | Insn.Predec _, Memory addr -> store_predecrement width addr (Temp 0)
    | _ -> write width dest (Temp 0)
  in
  (* A source auto-update is visible even to a destination using the same An. *)
  source @ setup @ [store] @ finish @ [Flags (Logic { result = Temp 0; width })]

let register : Insn.reg -> Sem.register = function
  | D n -> D n
  | A n -> A n

let push value =
  [Set_reg (A 7, Long, bin Sub (a 7) (int 4)); Store (Long, a 7, value)]

let lea src dst =
  let setup, addr = address ~slot:0 src in
  setup @ [Set_reg (A dst, Long, addr)]

let pea src =
  let setup, addr = address ~slot:0 src in
  setup @ [Let (0, addr)] @ push (Temp 0)

let movem direction width regs ea =
  let setup, base = match ea with
    | Insn.Predec reg | Postinc reg -> [], a reg
    | _ -> address ~slot:0 ea
  in
  let stride = bytes width in
  let offset n = bin Add (Temp 0) (int n) in
  (* The decoder lists registers in transfer order, including predecrement. *)
  let transfer index reg =
    let reg = register reg in
    match direction, ea with
    | Insn.From_memory, _ ->
        let slot = index + 1 in
        let load = Load (slot, width, offset (index * stride)) in
        (match ea with
         | Postinc base when reg = A base -> [load]
         | _ -> [load; Set_reg (reg, Long, Sext (width, Temp slot))])
    | To_memory, Predec base ->
        let addr = offset (-((index + 1) * stride)) in
        (* The 68020 stores the original base minus one operand when the
           base register itself is in the list (Motorola PRM, MOVEM). *)
        let value = if reg = A base then offset (-stride) else Reg reg in
        [store_predecrement width addr value]
    | To_memory, _ -> [Store (width, offset (index * stride), Reg reg)]
  in
  let total = List.length regs * stride in
  let finish = match ea with
    | Insn.Predec reg -> [Set_reg (A reg, Long, offset (-total))]
    | Postinc reg -> [Set_reg (A reg, Long, offset total)]
    | _ -> []
  in
  setup @ [Let (0, base)] @ List.concat (List.mapi transfer regs) @ finish

let movep width src dst =
  let count = bytes width in
  let byte_addr index = bin Add (Temp 0) (int (2 * index)) in
  match src, dst with
  | Insn.Register (D reg), mem ->
      let setup, addr = address ~slot:0 mem in
      let stores = List.init count (fun index ->
        let shift = 8 * (count - index - 1) in
        Store (Byte, byte_addr index, bin Shr (d reg) (int shift)))
      in
      setup @ [Let (0, addr)] @ stores
  | mem, Insn.Register (D reg) ->
      let setup, addr = address ~slot:0 mem in
      let loads = List.init count (fun index -> Load (index + 1, Byte, byte_addr index)) in
      let value = List.init count (fun index -> Temp (index + 1))
        |> List.fold_left (fun value byte -> bin Or (bin Shl value (int 8)) byte) (int 0)
      in
      setup @ [Let (0, addr)] @ loads @ [Set_reg (D reg, width, value)]
  | _ -> raise Unsupported_operand

let exg left right =
  let left = register left and right = register right in
  [Let (0, Reg left); Set_reg (left, Long, Reg right); Set_reg (right, Long, Temp 0)]

let link reg disp =
  (* LINK A7 pushes the already decremented stack pointer. *)
  push (a reg) @
  [Set_reg (A reg, Long, a 7); Set_reg (A 7, Long, bin Add (a 7) (int disp))]

let unlk reg =
  [Set_reg (A 7, Long, a reg); Load (0, Long, a 7);
   Set_reg (A 7, Long, bin Add (a 7) (int 4)); Set_reg (A reg, Long, Temp 0)]

let of_op ctx : Insn.op -> Sem.t option = function
  | Move (width, src, dst) -> Some (advance ctx (move width src dst))
  | Movea (width, src, dst) ->
      Some (advance ctx (read width 0 src @ [Set_reg (A dst, Long, Sext (width, Temp 0))]))
  | Moveq (value, dst) ->
      Some (advance ctx [Set_reg (D dst, Long, int value); Flags (Logic { result = int value; width = Long })])
  | Lea (src, dst) -> Some (advance ctx (lea src dst))
  | Pea src -> Some (advance ctx (pea src))
  | Movem (direction, width, regs, ea) -> Some (advance ctx (movem direction width regs ea))
  | Movep (width, src, dst) -> Some (advance ctx (movep width src dst))
  | Exg (left, right) -> Some (advance ctx (exg left right))
  | Link (reg, disp) -> Some (advance ctx (link reg disp))
  | Unlk reg -> Some (advance ctx (unlk reg))
  | _ -> None
