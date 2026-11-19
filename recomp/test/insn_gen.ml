open F3recomp
open Insn

(* EA pools describe legal operands per instruction family. The PC passed to a
   pool is the extension address, including any preceding operand words. *)
let generate state =
  let below limit = Random.State.int state limit in
  let pick values = List.nth values (below (List.length values)) in
  let yes () = below 2 = 0 in
  let signed bits = below (1 lsl bits) - (1 lsl (bits - 1)) in
  let unsigned_long () = (below 0x10000 lsl 16) lor below 0x10000 in
  let signed_long () = unsigned_long () - 0x80000000 in
  let variable_disp () = pick [0; signed 8; signed 16; signed_long ()] in
  let reg () = below 8 in
  let general () = if yes () then D (reg ()) else A (reg ()) in
  let data_reg () = Register (D (reg ())) in
  let address_reg () = Register (A (reg ())) in
  let size () = pick [Byte; Word; Long] in
  let wide () = pick [Word; Long] in
  let cond () = pick [T; F; Hi; Ls; Cc; Cs; Ne; Eq; Vc; Vs; Pl; Mi; Ge; Lt; Gt; Le] in
  let sign () = pick [Signed; Unsigned] in
  let direction () = pick [To_memory; From_memory] in
  let control_move () = pick [To_control; From_control] in
  let status () = pick [Sr; Ccr] in
  let literal = function Byte -> below 256 | Word -> below 65536 | Long -> unsigned_long () in
  let indexed base =
    let index =
      if yes () then Some { reg = general (); long = yes (); scale = 1 lsl below 4 }
      else None
    in
    let outer = variable_disp () in
    let indirect = pick (Direct :: Pre outer :: (if Option.is_some index then [Post outer] else [])) in
    Indexed { base; disp = variable_disp (); index; indirect }
  in
  let absolute () = Absolute (if yes () then address (signed 16) else unsigned_long ()) in
  let disp () = Disp { reg = reg (); disp = signed 16 } in
  let ind () = Ind (reg ()) in
  let postinc () = Postinc (reg ()) in
  let predec () = Predec (reg ()) in
  let areg_index () = indexed (if yes () then Areg (reg ()) else Suppressed) in
  let pc_modes ext = [(fun () -> Pc_rel (address (ext + signed 16)));
                      (fun () -> indexed (Pc_at ext))] in
  let control_alterable = [ind; disp; areg_index; absolute] in
  let memory = postinc :: predec :: control_alterable in
  let alterable = data_reg :: memory in
  let control ext = control_alterable @ pc_modes ext in
  let data ext width = alterable @ pc_modes ext @ [fun () -> Immediate (literal width)] in
  let any ext width = address_reg :: data ext width in
  let operand modes = (pick modes) () in
  let pair () =
    let mode = pick [data_reg; predec] in
    mode (), mode ()
  in
  let cas_registers () = { compare = reg (); update = reg () } in
  let cas_operand () = { address = general (); registers = cas_registers () } in
  let count constants = if yes () then Constant (constants ()) else Data_register (reg ()) in
  let pc = if yes () then 2 * below 0x100000 else address (-2 * (1 + below 32)) in
  let ext = pc + 2 and prefixed = pc + 4 in
  let target () = address (ext + (pick [(fun () -> signed 8); (fun () -> signed 16); signed_long]) ()) in
  let db_target () = address (ext + signed 16) in
  let cases = [
    (fun () ->
      let width = size () in
      Move (width, operand (if width = Byte then data ext width else any ext width), operand alterable));
    (fun () -> let width = wide () in Movea (width, operand (any ext width), reg ()));
    (fun () -> Moveq (signed 8, reg ()));
    (fun () ->
      let width = size () in
      let operation = pick [Add; Sub; And; Or; Eor; Cmp] in
      let immediate () =
        let value = if List.mem operation [Add; Sub] && yes () then 1 + below 8 else literal width in
        let dst_modes = if operation = Cmp then alterable @ pc_modes (ext + (if width = Long then 4 else 2))
                        else alterable in
        Alu (operation, width, Immediate value, operand dst_modes)
      in
      let to_register () =
        let src_modes = if width <> Byte && List.mem operation [Add; Sub; Cmp]
                        then any ext width else data ext width in
        Alu (operation, width, operand src_modes, data_reg ())
      in
      let from_register () =
        let dst_modes = if operation = Eor then alterable else memory in
        Alu (operation, width, data_reg (), operand dst_modes)
      in
      let forms = match operation with
        | Eor -> [immediate; from_register]
        | Cmp -> [immediate; to_register; (fun () -> Alu (Cmp, width, postinc (), postinc ()))]
        | _ -> [immediate; to_register; from_register]
      in
      (pick forms) ());
    (fun () ->
      let width = wide () in
      let src = if yes () then Immediate (1 + below 8) else operand (any ext width) in
      Adda (width, src, reg ()));
    (fun () ->
      let width = wide () in
      let src = if yes () then Immediate (1 + below 8) else operand (any ext width) in
      Suba (width, src, reg ()));
    (fun () -> let width = wide () in Cmpa (width, operand (any ext width), reg ()));
    (fun () -> let src, dst = pair () in Addx (size (), src, dst));
    (fun () -> let src, dst = pair () in Subx (size (), src, dst));
    (fun () ->
      let operation = pick [Negx; Neg; Not; Clr; Tst; Nbcd; Tas] in
      let width = if List.mem operation [Nbcd; Tas] then Byte else size () in
      let modes = match operation with
        | Tst -> if width = Byte then data ext width else any ext width
        | _ -> alterable
      in
      Unary (operation, width, operand modes));
    (fun () -> Ext (pick [Byte_to_word; Word_to_long; Byte_to_long], reg ()));
    (fun () -> Swap (reg ()));
    (fun () ->
      let operation = pick [Asr; Asl; Lsr; Lsl; Roxr; Roxl; Ror; Rol] in
      if yes () then Shift (operation, size (), count (fun () -> 1 + below 8), data_reg ())
      else Shift (operation, Word, Constant 1, operand memory));
    (fun () ->
      let operation = pick [Btst; Bchg; Bclr; Bset] in
      let number = count (fun () -> below 256) in
      let modes = match operation, number with
        | Btst, Data_register _ -> data ext Byte
        | Btst, Constant _ -> alterable @ pc_modes prefixed
        | _ -> alterable
      in
      Bit (operation, number, operand modes));
    (fun () ->
      let operation = pick [Bftst; Bfextu (reg ()); Bfchg; Bfexts (reg ());
                            Bfclr; Bfffo (reg ()); Bfset; Bfins (reg ())] in
      let modes = match operation with
        | Bftst | Bfextu _ | Bfexts _ | Bfffo _ -> control prefixed
        | _ -> control_alterable
      in
      Bitfield { operation; offset = count (fun () -> below 32);
                 width = count (fun () -> 1 + below 32); operand = operand (data_reg :: modes) });
    (fun () -> Mul_word (sign (), operand (data ext Word), reg ()));
    (fun () ->
      Mul_long { sign = sign (); operand = operand (data prefixed Long);
                 low = reg (); high = (if yes () then Some (reg ()) else None) });
    (fun () -> Div_word (sign (), operand (data ext Word), reg ()));
    (fun () ->
      let quotient = reg () and remainder = reg () in
      let dividend = if yes () then Single quotient else Double { high = remainder; low = quotient } in
      Div_long { sign = sign (); operand = operand (data prefixed Long); dividend; quotient; remainder });
    (fun () -> let src, dst = pair () in Abcd (src, dst));
    (fun () -> let src, dst = pair () in Sbcd (src, dst));
    (fun () -> let src, dst = pair () in Pack (src, dst, signed 16));
    (fun () -> let src, dst = pair () in Unpk (src, dst, signed 16));
    (fun () -> Cas (size (), cas_registers (), operand memory));
    (fun () -> Cas2 (wide (), cas_operand (), cas_operand ()));
    (fun () -> let width = wide () in Chk (width, operand (data ext width), reg ()));
    (fun () -> Bounds (pick [Check; Compare], size (), operand (control prefixed), general ()));
    (fun () -> Bra (target ()));
    (fun () -> Bsr (target ()));
    (fun () -> Bcc (pick [Hi; Ls; Cc; Cs; Ne; Eq; Vc; Vs; Pl; Mi; Ge; Lt; Gt; Le], target ()));
    (fun () -> Dbcc (cond (), reg (), db_target ()));
    (fun () -> Scc (cond (), operand alterable));
    (fun () -> Jmp (operand (control ext)));
    (fun () -> Jsr (operand (control ext)));
    (fun () -> Rts);
    (fun () -> Rtd (signed 16));
    (fun () -> Rtr);
    (fun () -> Rte);
    (fun () -> Trap (below 16));
    (fun () -> Trapcc (cond (), pick [None; Some (Word, literal Word); Some (Long, literal Long)]));
    (fun () -> Trapv);
    (fun () -> Callm (literal Byte, operand (control prefixed)));
    (fun () -> Rtm (general ()));
    (fun () -> Lea (operand (control ext), reg ()));
    (fun () -> Pea (operand (control ext)));
    (fun () -> Link (reg (), if yes () then signed 16 else signed_long ()));
    (fun () -> Unlk (reg ()));
    (fun () ->
      let left, right = pick [D (reg ()), D (reg ()); A (reg ()), A (reg ()); D (reg ()), A (reg ())] in
      Exg (left, right));
    (fun () ->
      let direction = direction () in
      let modes = match direction with
        | To_memory -> predec :: control_alterable
        | From_memory -> postinc :: control prefixed
      in
      let ea = operand modes in
      let registers = List.init 16 (fun i -> if i < 8 then D i else A (i - 8)) in
      let registers = match ea with Predec _ -> List.rev registers | _ -> registers in
      Movem (direction, wide (), List.filter (fun _ -> yes ()) registers, ea));
    (fun () ->
      let mem = disp () and data = data_reg () in
      if yes () then Movep (wide (), mem, data) else Movep (wide (), data, mem));
    (fun () -> Moves (direction (), size (), general (), operand memory));
    (fun () -> Movec (control_move (), general (), pick [Sfc; Dfc; Cacr; Usp; Vbr; Caar; Msp; Isp]));
    (fun () -> From_status (status (), operand alterable));
    (fun () -> To_status (status (), operand (data ext Word)));
    (fun () -> Move_usp (control_move (), reg ()));
    (fun () ->
      let status = status () in
      Logic_status (pick [Or; And; Eor], status, literal (if status = Sr then Word else Byte)));
    (fun () -> Stop (literal Word));
    (fun () -> Reset);
    (fun () -> Nop);
  ] in
  let op = (pick cases) () in
  { pc; length = Asm.length ~pc op; op }

let constructor = function
  | Move _ -> "Move" | Movea _ -> "Movea" | Moveq _ -> "Moveq" | Alu _ -> "Alu"
  | Adda _ -> "Adda" | Suba _ -> "Suba" | Cmpa _ -> "Cmpa" | Addx _ -> "Addx" | Subx _ -> "Subx"
  | Unary _ -> "Unary" | Ext _ -> "Ext" | Swap _ -> "Swap" | Shift _ -> "Shift"
  | Bit _ -> "Bit" | Bitfield _ -> "Bitfield" | Mul_word _ -> "Mul_word" | Mul_long _ -> "Mul_long"
  | Div_word _ -> "Div_word" | Div_long _ -> "Div_long" | Abcd _ -> "Abcd" | Sbcd _ -> "Sbcd"
  | Pack _ -> "Pack" | Unpk _ -> "Unpk" | Cas _ -> "Cas" | Cas2 _ -> "Cas2"
  | Chk _ -> "Chk" | Bounds _ -> "Bounds" | Bra _ -> "Bra" | Bsr _ -> "Bsr" | Bcc _ -> "Bcc"
  | Dbcc _ -> "Dbcc" | Scc _ -> "Scc" | Jmp _ -> "Jmp" | Jsr _ -> "Jsr"
  | Rts -> "Rts" | Rtd _ -> "Rtd" | Rtr -> "Rtr" | Rte -> "Rte" | Trap _ -> "Trap"
  | Trapcc _ -> "Trapcc" | Trapv -> "Trapv" | Callm _ -> "Callm" | Rtm _ -> "Rtm"
  | Lea _ -> "Lea" | Pea _ -> "Pea" | Link _ -> "Link" | Unlk _ -> "Unlk" | Exg _ -> "Exg"
  | Movem _ -> "Movem" | Movep _ -> "Movep" | Moves _ -> "Moves" | Movec _ -> "Movec"
  | From_status _ -> "From_status" | To_status _ -> "To_status" | Move_usp _ -> "Move_usp"
  | Logic_status _ -> "Logic_status" | Stop _ -> "Stop" | Reset -> "Reset" | Nop -> "Nop"
