(* Family e: control flow and privileged or status-register instructions. *)
open Sem
open Semantics_ops

let stack_at offset = bin Add (a 7) (int offset)
let adjust_stack offset = Set_reg (A 7, Long, stack_at offset)

let call ctx target =
  [adjust_stack (-4); Store (Long, a 7, int ctx.next); Goto target]

let return displacement =
  [Load (0, Long, a 7); adjust_stack (4 + displacement); Goto (Temp 0)]

let rte ctx =
  let format = Temp 1 and status = Temp 2 in
  let resume =
    [Load (3, Long, stack_at 2);
     If (bin Eq format (int 2), [adjust_stack 12], [adjust_stack 8]);
     Runtime (Set_sr status); Goto (Temp 3)]
  in
  (* The runtime builds formats 0 and 2, and a format 1 throwaway frame when
     an interrupt switches from the master stack to the interrupt stack. *)
  supervisor ctx
    [Repeat
       [Load (0, Word, stack_at 6); Let (1, bin Shr (Temp 0) (int 12));
        If (bin Lt format (int 3),
            [Load (2, Word, a 7);
             If (bin Eq format (int 1), [adjust_stack 8; Runtime (Set_sr status)], resume)],
            [trap 14 ctx.pc])]]

let general : Insn.reg -> register = function D n -> D n | A n -> A n

let control_register : Insn.control -> register = function
  | Sfc -> Sfc | Dfc -> Dfc | Cacr -> Cacr | Usp -> Usp | Vbr -> Vbr
  | Caar -> Caar | Msp -> Msp | Isp -> Isp

let movec ctx direction reg control =
  let reg = general reg in
  let control = control_register control in
  let transfer control =
    match direction with
    | Insn.From_control ->
        (* The 68020 CACR cache clear commands always read as zero. *)
        let value = if control = Cacr then bin And (Reg control) (int 3) else Reg control in
        Set_reg (reg, Long, value)
    | To_control ->
        let value = match control with
          | Sfc | Dfc -> bin And (Reg reg) (int 7)
          | Cacr -> bin And (Reg reg) (int 3)
          | _ -> Reg reg
        in
        Set_reg (control, Long, value)
  in
  let move = match control with
    | Msp | Isp ->
        let master = bin And Sr (int 0x1000) in
        let active = bin (if control = Msp then Ne else Eq) master (int 0) in
        If (active, [transfer (A 7)], [transfer control])
    | _ -> transfer control
  in
  supervisor ctx [move; goto ctx.next]

let invalid_movec pc =
  supervisor { pc; next = pc } [trap 4 pc]

let moves ctx direction width reg ea =
  let setup, location, finish = place width 0 ea in
  let reg = general reg in
  (* F3 exposes a single memory map through the ABI. An auto-update takes
     effect before reading the transfer register, which may alias the base. *)
  let transfer = match direction with
    | Insn.To_memory -> [write width location (Reg reg)]
    | From_memory ->
        let destination_width, value = match reg with
          | A _ -> Long, Sext (width, Temp 1)
          | _ -> width, Temp 1
        in
        [read_place width 1 location; Set_reg (reg, destination_width, value)]
  in
  supervisor ctx (setup @ finish @ transfer @ [goto ctx.next])

let logic_status ctx operation status immediate =
  let op = match operation with
    | Insn.And -> And | Or -> Or | Eor -> Xor
    | _ -> invalid_arg "invalid status-register logic operation"
  in
  match status with
  | Insn.Ccr -> advance ctx [Set_ccr (bin op Ccr (int immediate))]
  | Sr -> supervisor ctx [Runtime (Set_sr (bin op Sr (int immediate))); goto ctx.next]

let of_op ctx : Insn.op -> Sem.t option =
  let next = ctx.next in
  function
  | Bra target -> Some (Body [goto target])
  | Bsr target -> Some (Body (call ctx (int target)))
  | Bcc (cond, target) -> Some (Body [If (Cond cond, [goto target], [goto next])])
  | Dbcc (cond, reg, target) ->
      let decrement = Let (0, truncate Word (bin Sub (d reg) (int 1))) in
      Some (Body [If (Cond cond, [goto next],
                      [decrement; Set_reg (D reg, Word, Temp 0);
                       If (bin Eq (Temp 0) (int 0xffff), [goto next], [goto target])])])
  | Jmp ea ->
      let setup, target = address ~slot:0 ea in
      Some (Body (setup @ [Goto target]))
  | Jsr ea ->
      let setup, target = match address ~slot:0 ea with
        | [], (Const _ as target) -> [], target
        | reads, target -> reads @ [Let (0, target)], Temp 0
      in
      Some (Body (setup @ call ctx target))
  | Rts -> Some (Body (return 0))
  | Rtd displacement -> Some (Body (return displacement))
  | Rtr ->
      Some (Body [Load (0, Word, a 7); adjust_stack 2; Set_ccr (Temp 0);
                  Load (1, Long, a 7); adjust_stack 4; Goto (Temp 1)])
  | Rte -> Some (rte ctx)
  | Trap number -> Some (Body [trap (32 + number) next])
  | Trapv -> Some (Body [If (Cond Vs, [trap 7 next], [goto next])])
  | Trapcc (cond, _) -> Some (Body [If (Cond cond, [trap 7 next], [goto next])])
  | Callm _ | Rtm _ ->
      Some (Unsupported "module calls require MC68020 module-frame and access-control support")
  | Movec (direction, reg, control) -> Some (movec ctx direction reg control)
  | Moves (direction, width, reg, ea) -> Some (moves ctx direction width reg ea)
  | Logic_status (operation, status, immediate) -> Some (logic_status ctx operation status immediate)
  | From_status (Ccr, dst) ->
      let setup, dest, finish = place Word 0 dst in
      Some (advance ctx (setup @ [write Word dest Ccr] @ finish))
  | From_status (Sr, dst) ->
      let setup, dest, finish = place Word 0 dst in
      Some (supervisor ctx (setup @ [write Word dest Sr] @ finish @ [goto next]))
  | To_status (Ccr, src) -> Some (advance ctx (read Word 0 src @ [Set_ccr (Temp 0)]))
  | To_status (Sr, src) ->
      Some (supervisor ctx (read Word 0 src @ [Runtime (Set_sr (Temp 0)); goto next]))
  | Move_usp (From_control, reg) -> Some (supervisor ctx [Set_reg (A reg, Long, Reg Usp); goto next])
  | Move_usp (To_control, reg) -> Some (supervisor ctx [Set_reg (Usp, Long, a reg); goto next])
  | Reset -> Some (supervisor ctx [Runtime Reset_devices; goto next])
  | Stop sr -> Some (supervisor ctx [Runtime (Set_sr (int sr)); Stop next])
  | Nop -> Some (advance ctx [])
  | _ -> None
