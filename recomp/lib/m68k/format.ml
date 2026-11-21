open Insn

let size_suffix = function Byte -> ".b" | Word -> ".w" | Long -> ".l"
let size_bytes = function Byte -> 1 | Word -> 2 | Long -> 4

let cond_name = function
  | T -> "t" | F -> "f" | Hi -> "hi" | Ls -> "ls" | Cc -> "cc" | Cs -> "cs" | Ne -> "ne"
  | Eq -> "eq" | Vc -> "vc" | Vs -> "vs" | Pl -> "pl" | Mi -> "mi" | Ge -> "ge" | Lt -> "lt"
  | Gt -> "gt" | Le -> "le"

let reg_name = function D n -> Printf.sprintf "d%d" n | A n -> Printf.sprintf "a%d" n
let dreg n = reg_name (D n)
let areg n = reg_name (A n)

let hex value = Printf.sprintf "$%x" value
let signed_hex value = if value < 0 then Printf.sprintf "-$%x" (-value) else hex value
let address value =
  let value = value land 0xffffffff in
  if value < 0x1000000 then Printf.sprintf "$%06x" value else Printf.sprintf "$%08x" value

let number value = if value >= 0 && value < 10 then string_of_int value else hex value
let signed_number value = if value > -10 && value < 10 then string_of_int value else signed_hex value
let immediate value = "#" ^ number value
let signed_immediate value = "#" ^ signed_number value

let index_text { reg; long; scale } =
  Printf.sprintf "%s.%c%s" (reg_name reg) (if long then 'l' else 'w')
    (if scale = 1 then "" else Printf.sprintf "*%d" scale)

let base_text = function
  | Areg n -> Some (areg n)
  | Pc_at _ -> Some "pc"
  | Suppressed -> None

let ea_to_string = function
  | Register reg -> reg_name reg
  | Ind n -> Printf.sprintf "(%s)" (areg n)
  | Postinc n -> Printf.sprintf "(%s)+" (areg n)
  | Predec n -> Printf.sprintf "-(%s)" (areg n)
  | Disp { reg; disp } ->
      Printf.sprintf "%s(%s)" (if disp = 0 then "0" else signed_hex disp) (areg reg)
  | Indexed { base; disp; index; indirect } ->
      let base = base_text base in
      let index = Option.map index_text index in
      let join parts = String.concat "," (List.filter_map Fun.id parts) in
      let displacement value = if value = 0 then None else Some (signed_hex value) in
      (match indirect with
       | Direct ->
           (match base with
            | Some base ->
                let inside = join (Some base :: [index]) in
                Printf.sprintf "%s(%s)" (if disp = 0 then "0" else signed_hex disp) inside
            | None -> Printf.sprintf "(%s)" (join [Some (signed_hex disp); index]))
       | Pre outer ->
           Printf.sprintf "([%s]%s)" (join [displacement disp; base; index])
             (match displacement outer with Some od -> "," ^ od | None -> "")
       | Post outer ->
           Printf.sprintf "([%s]%s)" (join [displacement disp; base])
             (String.concat "" (List.filter_map (Option.map (fun part -> "," ^ part))
                                  [index; displacement outer])))
  | Absolute value -> address value
  | Pc_rel value -> Printf.sprintf "%s(pc)" (address value)
  | Immediate value -> immediate value

let register_list regs =
  let ordinal = function D n -> n | A n -> 8 + n in
  let sorted = List.sort_uniq Int.compare (List.map ordinal regs) in
  let name n = if n < 8 then dreg n else areg (n - 8) in
  let rec runs = function
    | [] -> []
    | first :: rest ->
        let rec extend last = function
          | next :: more when next = last + 1 && next <> 8 -> extend next more
          | more -> last, more
        in
        let last, more = extend first rest in
        (if last = first then name first else name first ^ "-" ^ name last) :: runs more
  in
  String.concat "/" (runs sorted)

let alu_name = function
  | Add -> "add" | Sub -> "sub" | And -> "and" | Or -> "or" | Eor -> "eor" | Cmp -> "cmp"

let shift_name = function
  | Asr -> "asr" | Asl -> "asl" | Lsr -> "lsr" | Lsl -> "lsl"
  | Roxr -> "roxr" | Roxl -> "roxl" | Ror -> "ror" | Rol -> "rol"

let bit_name = function Btst -> "btst" | Bchg -> "bchg" | Bclr -> "bclr" | Bset -> "bset"

let bitfield_name = function
  | Bftst -> "bftst" | Bfextu _ -> "bfextu" | Bfchg -> "bfchg" | Bfexts _ -> "bfexts"
  | Bfclr -> "bfclr" | Bfffo _ -> "bfffo" | Bfset -> "bfset" | Bfins _ -> "bfins"

let control_name = function
  | Sfc -> "sfc" | Dfc -> "dfc" | Cacr -> "cacr" | Usp -> "usp" | Vbr -> "vbr"
  | Caar -> "caar" | Msp -> "msp" | Isp -> "isp"

let status_name = function Sr -> "sr" | Ccr -> "ccr"
let sign_letter = function Signed -> "s" | Unsigned -> "u"

let unary_name = function
  | Negx -> "negx" | Neg -> "neg" | Not -> "not" | Clr -> "clr" | Tst -> "tst"
  | Nbcd -> "nbcd" | Tas -> "tas"

let quick = function Immediate value -> value >= 1 && value <= 8 | _ -> false

let div_long_name sign dividend ~quotient ~remainder =
  let base = "div" ^ sign_letter sign in
  match dividend with
  | Single _ when quotient <> remainder -> base ^ "l.l"
  | _ -> base ^ ".l"

let mnemonic = function
  | Move (size, _, _) -> "move" ^ size_suffix size
  | Movea (size, _, _) -> "movea" ^ size_suffix size
  | Moveq _ -> "moveq"
  | Alu (Cmp, size, Postinc _, Postinc _) -> "cmpm" ^ size_suffix size
  | Alu (((Add | Sub) as op), size, src, _) when quick src ->
      alu_name op ^ "q" ^ size_suffix size
  | Alu (op, size, Immediate _, _) -> alu_name op ^ "i" ^ size_suffix size
  | Alu (op, size, _, _) -> alu_name op ^ size_suffix size
  | Adda (size, src, _) -> (if quick src then "addq" else "adda") ^ size_suffix size
  | Suba (size, src, _) -> (if quick src then "subq" else "suba") ^ size_suffix size
  | Cmpa (size, _, _) -> "cmpa" ^ size_suffix size
  | Addx (size, _, _) -> "addx" ^ size_suffix size
  | Subx (size, _, _) -> "subx" ^ size_suffix size
  | Unary ((Nbcd | Tas) as op, _, _) -> unary_name op
  | Unary (op, size, _) -> unary_name op ^ size_suffix size
  | Ext (Byte_to_word, _) -> "ext.w"
  | Ext (Word_to_long, _) -> "ext.l"
  | Ext (Byte_to_long, _) -> "extb.l"
  | Swap _ -> "swap"
  | Shift (op, size, _, _) -> shift_name op ^ size_suffix size
  | Bit (op, _, _) -> bit_name op
  | Bitfield { operation; _ } -> bitfield_name operation
  | Mul_word (sign, _, _) -> "mul" ^ sign_letter sign ^ ".w"
  | Mul_long { sign; _ } -> "mul" ^ sign_letter sign ^ ".l"
  | Div_word (sign, _, _) -> "div" ^ sign_letter sign ^ ".w"
  | Div_long { sign; dividend; quotient; remainder; _ } ->
      div_long_name sign dividend ~quotient ~remainder
  | Abcd _ -> "abcd"
  | Sbcd _ -> "sbcd"
  | Pack _ -> "pack"
  | Unpk _ -> "unpk"
  | Cas (size, _, _) -> "cas" ^ size_suffix size
  | Cas2 (size, _, _) -> "cas2" ^ size_suffix size
  | Chk (size, _, _) -> "chk" ^ size_suffix size
  | Bounds (Check, size, _, _) -> "chk2" ^ size_suffix size
  | Bounds (Compare, size, _, _) -> "cmp2" ^ size_suffix size
  | Bra _ -> "bra"
  | Bsr _ -> "bsr"
  | Bcc (cond, _) -> "b" ^ cond_name cond
  | Dbcc (F, _, _) -> "dbra"
  | Dbcc (cond, _, _) -> "db" ^ cond_name cond
  | Scc (cond, _) -> "s" ^ cond_name cond
  | Jmp _ -> "jmp"
  | Jsr _ -> "jsr"
  | Rts -> "rts"
  | Rtd _ -> "rtd"
  | Rtr -> "rtr"
  | Rte -> "rte"
  | Trap _ -> "trap"
  | Trapcc (cond, None) -> "trap" ^ cond_name cond
  | Trapcc (cond, Some (size, _)) -> "trap" ^ cond_name cond ^ size_suffix size
  | Trapv -> "trapv"
  | Callm _ -> "callm"
  | Rtm _ -> "rtm"
  | Lea _ -> "lea"
  | Pea _ -> "pea"
  | Link (_, disp) -> if disp < -0x8000 || disp > 0x7fff then "link.l" else "link"
  | Unlk _ -> "unlk"
  | Exg _ -> "exg"
  | Movem (_, size, _, _) -> "movem" ^ size_suffix size
  | Movep (size, _, _) -> "movep" ^ size_suffix size
  | Moves (_, size, _, _) -> "moves" ^ size_suffix size
  | Movec _ -> "movec"
  | From_status _ | To_status _ | Move_usp _ -> "move"
  | Logic_status (op, _, _) -> alu_name op ^ "i"
  | Stop _ -> "stop"
  | Reset -> "reset"
  | Nop -> "nop"

let count_text = function Constant n -> immediate n | Data_register r -> dreg r
let field_text = function Constant n -> string_of_int n | Data_register r -> dreg r

let pair first second = first ^ "," ^ second

let operands = function
  | Move (_, src, dst) | Alu (_, _, src, dst) | Addx (_, src, dst) | Subx (_, src, dst)
  | Abcd (src, dst) | Sbcd (src, dst) | Movep (_, src, dst) ->
      pair (ea_to_string src) (ea_to_string dst)
  | Movea (_, src, reg) | Adda (_, src, reg) | Suba (_, src, reg) | Cmpa (_, src, reg) ->
      pair (ea_to_string src) (areg reg)
  | Moveq (value, reg) -> pair (signed_immediate value) (dreg reg)
  | Unary (_, _, ea) | Scc (_, ea) | Jmp ea | Jsr ea | Pea ea -> ea_to_string ea
  | Ext (_, reg) | Swap reg -> dreg reg
  | Shift (_, _, count, (Register _ as ea)) -> pair (count_text count) (ea_to_string ea)
  | Shift (_, _, _, ea) -> ea_to_string ea
  | Bit (_, count, ea) -> pair (count_text count) (ea_to_string ea)
  | Bitfield { operation; offset; width; operand } ->
      let field =
        Printf.sprintf "%s{%s:%s}" (ea_to_string operand) (field_text offset) (field_text width)
      in
      (match operation with
       | Bfextu reg | Bfexts reg | Bfffo reg -> pair field (dreg reg)
       | Bfins reg -> pair (dreg reg) field
       | Bftst | Bfchg | Bfclr | Bfset -> field)
  | Mul_word (_, ea, reg) | Div_word (_, ea, reg) | Chk (_, ea, reg) ->
      pair (ea_to_string ea) (dreg reg)
  | Mul_long { operand; low; high; _ } ->
      let result = match high with Some high -> dreg high ^ ":" ^ dreg low | None -> dreg low in
      pair (ea_to_string operand) result
  | Div_long { operand; dividend; quotient; remainder; _ } ->
      let result =
        match dividend with
        | Single _ when quotient = remainder -> dreg quotient
        | _ -> dreg remainder ^ ":" ^ dreg quotient
      in
      pair (ea_to_string operand) result
  | Pack (src, dst, adjust) | Unpk (src, dst, adjust) ->
      String.concat "," [ea_to_string src; ea_to_string dst; signed_immediate adjust]
  | Cas (_, { compare; update }, ea) ->
      String.concat "," [dreg compare; dreg update; ea_to_string ea]
  | Cas2 (_, first, second) ->
      let compare = dreg first.registers.compare ^ ":" ^ dreg second.registers.compare in
      let update = dreg first.registers.update ^ ":" ^ dreg second.registers.update in
      let pointers =
        Printf.sprintf "(%s):(%s)" (reg_name first.address) (reg_name second.address)
      in
      String.concat "," [compare; update; pointers]
  | Bounds (_, _, ea, reg) -> pair (ea_to_string ea) (reg_name reg)
  | Bra target | Bsr target | Bcc (_, target) -> address target
  | Dbcc (_, reg, target) -> pair (dreg reg) (address target)
  | Rts | Rtr | Rte | Trapv | Reset | Nop | Trapcc (_, None) -> ""
  | Rtd disp -> signed_immediate disp
  | Trap vector -> "#" ^ string_of_int vector
  | Trapcc (_, Some (_, value)) -> immediate value
  | Callm (count, ea) -> pair (immediate count) (ea_to_string ea)
  | Rtm reg -> reg_name reg
  | Lea (ea, reg) -> pair (ea_to_string ea) (areg reg)
  | Link (reg, disp) -> pair (areg reg) (signed_immediate disp)
  | Unlk reg -> areg reg
  | Exg (left, right) -> pair (reg_name left) (reg_name right)
  | Movem (To_memory, _, regs, ea) -> pair (register_list regs) (ea_to_string ea)
  | Movem (From_memory, _, regs, ea) -> pair (ea_to_string ea) (register_list regs)
  | Moves (To_memory, _, reg, ea) -> pair (reg_name reg) (ea_to_string ea)
  | Moves (From_memory, _, reg, ea) -> pair (ea_to_string ea) (reg_name reg)
  | Movec (From_control, reg, control) -> pair (control_name control) (reg_name reg)
  | Movec (To_control, reg, control) -> pair (reg_name reg) (control_name control)
  | From_status (status, ea) -> pair (status_name status) (ea_to_string ea)
  | To_status (status, ea) -> pair (ea_to_string ea) (status_name status)
  | Move_usp (From_control, reg) -> pair "usp" (areg reg)
  | Move_usp (To_control, reg) -> pair (areg reg) "usp"
  | Logic_status (_, status, value) -> pair (immediate value) (status_name status)
  | Stop value -> immediate value

let to_string (insn : Insn.t) =
  match operands insn.op with
  | "" -> mnemonic insn.op
  | operands -> mnemonic insn.op ^ " " ^ operands

type role = Read | Write | Rmw | Call | Branch | Lea

let role_name = function
  | Read -> "read" | Write -> "write" | Rmw -> "rmw" | Call -> "call" | Branch -> "branch"
  | Lea -> "lea"

type reference = {
  role : role;
  target : int option;
  a5_disp : int option;
  size : int;
  indexed : bool;
}

let fold32 value = value land 0xffffffff

(* The static address of an operand and whether an index register is added. *)
let static_target ?a5 = function
  | Absolute value | Pc_rel value -> Some (fold32 value), None, false
  | Disp { reg = 5; disp } -> Option.map (fun a5 -> fold32 (a5 + disp)) a5, Some disp, false
  | Indexed { indirect = Direct; base; disp; index } ->
      let indexed = Option.is_some index in
      (match base with
       | Suppressed -> Some (fold32 disp), None, indexed
       | Pc_at pc -> Some (fold32 (pc + disp)), None, indexed
       | Areg 5 -> Option.map (fun a5 -> fold32 (a5 + disp)) a5, Some disp, indexed
       | Areg _ -> None, None, indexed)
  | _ -> None, None, false

let reference_of ?a5 role size ea =
  match static_target ?a5 ea with
  | None, None, _ -> None
  | target, a5_disp, indexed -> Some { role; target; a5_disp; size; indexed }

(* Memory operands with how the instruction uses them. *)
let accesses : Insn.op -> (role * int * ea) list = function
  | Move (size, src, dst) -> [Read, size_bytes size, src; Write, size_bytes size, dst]
  | Movea (size, src, _) | Adda (size, src, _) | Suba (size, src, _) | Cmpa (size, src, _) ->
      [Read, size_bytes size, src]
  | Alu (Cmp, size, src, dst) -> [Read, size_bytes size, src; Read, size_bytes size, dst]
  | Alu (_, size, src, dst) -> [Read, size_bytes size, src; Rmw, size_bytes size, dst]
  | Addx (size, src, dst) | Subx (size, src, dst) ->
      [Read, size_bytes size, src; Rmw, size_bytes size, dst]
  | Unary (Tst, size, ea) -> [Read, size_bytes size, ea]
  | Unary (Clr, size, ea) -> [Write, size_bytes size, ea]
  | Unary ((Nbcd | Tas), _, ea) -> [Rmw, 1, ea]
  | Unary (_, size, ea) -> [Rmw, size_bytes size, ea]
  | Shift (_, _, _, ea) -> [Rmw, 2, ea]
  | Bit (Btst, _, ea) -> [Read, 1, ea]
  | Bit (_, _, ea) -> [Rmw, 1, ea]
  | Bitfield { operation = Bftst | Bfextu _ | Bfexts _ | Bfffo _; operand; _ } -> [Read, 1, operand]
  | Bitfield { operand; _ } -> [Rmw, 1, operand]
  | Mul_word (_, ea, _) | Div_word (_, ea, _) -> [Read, 2, ea]
  | Mul_long { operand; _ } | Div_long { operand; _ } -> [Read, 4, operand]
  | Abcd (src, dst) | Sbcd (src, dst) -> [Read, 1, src; Rmw, 1, dst]
  | Pack (src, dst, _) | Unpk (src, dst, _) -> [Read, 1, src; Write, 1, dst]
  | Cas (size, _, ea) -> [Rmw, size_bytes size, ea]
  | Chk (size, ea, _) | Bounds (_, size, ea, _) -> [Read, size_bytes size, ea]
  | Scc (_, ea) -> [Write, 1, ea]
  | Callm (_, ea) -> [Read, 1, ea]
  | Bra target | Bcc (_, target) | Dbcc (_, _, target) -> [Branch, 0, Absolute target]
  | Bsr target -> [Call, 0, Absolute target]
  | Jmp ea -> [Branch, 0, ea]
  | Jsr ea -> [Call, 0, ea]
  | Lea (ea, _) | Pea ea -> [Lea, 0, ea]
  | Movem (To_memory, size, regs, ea) -> [Write, size_bytes size * List.length regs, ea]
  | Movem (From_memory, size, regs, ea) -> [Read, size_bytes size * List.length regs, ea]
  | Movep (size, src, dst) -> [Read, size_bytes size, src; Write, size_bytes size, dst]
  | Moves (To_memory, size, _, ea) -> [Write, size_bytes size, ea]
  | Moves (From_memory, size, _, ea) -> [Read, size_bytes size, ea]
  | From_status (_, ea) -> [Write, 2, ea]
  | To_status (_, ea) -> [Read, 2, ea]
  | Moveq _ | Ext _ | Swap _ | Cas2 _ | Rts | Rtd _ | Rtr | Rte | Trap _ | Trapcc _
  | Trapv | Rtm _ | Link _ | Unlk _ | Exg _ | Movec _ | Move_usp _ | Logic_status _
  | Stop _ | Reset | Nop -> []

let references ?a5 (insn : Insn.t) =
  List.filter_map (fun (role, size, ea) ->
    let physical = match role with
      | Call | Branch -> Option.map Pc.physical
      | _ -> Fun.id
    in
    Option.map (fun reference -> { reference with target = physical reference.target })
      (reference_of ?a5 role size ea))
    (accesses insn.op)
