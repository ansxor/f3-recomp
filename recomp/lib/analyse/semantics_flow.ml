(* Family e: branches, DBcc, JMP/JSR/RTS/RTD/RTR/RTE, TRAP/TRAPcc/TRAPV, MOVEC,
   MOVES, SR/CCR/USP moves, STOP, RESET, NOP. *)
open Sem
open Semantics_ops

let of_op ctx : Insn.op -> Sem.t option =
  let next = ctx.next in
  function
  | Bra target -> Some (Body [goto target])
  | Bcc (cond, target) -> Some (Body [If (Cond cond, [goto target], [goto next])])
  | Dbcc (cond, reg, target) ->
      let decrement = Let (0, truncate Word (bin Sub (d reg) (int 1))) in
      Some (Body [If (Cond cond, [goto next],
                      [decrement; Set_reg (D reg, Word, Temp 0);
                       If (bin Eq (Temp 0) (int 0xffff), [goto next], [goto target])])])
  | Jsr ea ->
      let setup, target = match address ea with
        | Const _ as target -> [], target
        | target -> [Let (0, target)], Temp 0
      in
      Some (Body (setup @ [Set_reg (A 7, Long, bin Sub (a 7) (int 4));
                           Store (Long, a 7, int next); Goto target]))
  | Rts ->
      Some (Body [Load (0, Long, a 7); Set_reg (A 7, Long, bin Add (a 7) (int 4)); Goto (Temp 0)])
  | Trap number -> Some (Body [trap (32 + number) next])
  | Trapv -> Some (Body [If (Cond Vs, [trap 7 next], [goto next])])
  | Trapcc (cond, _) -> Some (Body [If (Cond cond, [trap 7 next], [goto next])])
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
