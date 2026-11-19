open Insn

type mame_default = Breakpoint | F_line | Movec_control of int
type reason = Unassigned | Illegal_opcode | A_line | Truncated | Mame_default of mame_default
type illegal = { vector : int; reason : reason; length : int }
type decoded = Insn of Insn.t | Illegal of illegal

exception Reject of int * reason

let reject ?(vector = 4) reason = raise_notrace (Reject (vector, reason))
let require legal = if not legal then reject Unassigned

module Cursor = struct
  type t = { image : Image.t; pc : Pc.t; mutable length : int }

  let position input = input.pc + input.length

  let read input bytes read =
    let pc = position input in
    if pc < input.pc || not (Image.contains_range input.image pc ~length:bytes) then
      reject Truncated;
    let value = read input.image pc in
    input.length <- input.length + bytes;
    value

  let word input = read input 2 Image.u16
  let signed_word input = read input 2 Image.s16
  let long input = read input 4 Image.u32
  let signed_long input = read input 4 Image.s32

  let immediate input = function
    | Byte -> word input land 0xff
    | Word -> word input
    | Long -> long input
end

let field word shift mask = (word lsr shift) land mask
let bit word shift = field word shift 1 <> 0
let signed_byte word = (word land 127) - (word land 128)
let register code = if bit code 3 then A (code land 7) else D (code land 7)
let size = function 0 -> Byte | 1 -> Word | 2 -> Long | _ -> reject Unassigned
let word_or_long long = if long then Long else Word
let nonzero_quick = function 0 -> 8 | n -> n
let condition = function
  | 0 -> T | 1 -> F | 2 -> Hi | 3 -> Ls | 4 -> Cc | 5 -> Cs | 6 -> Ne | 7 -> Eq
  | 8 -> Vc | 9 -> Vs | 10 -> Pl | 11 -> Mi | 12 -> Ge | 13 -> Lt | 14 -> Gt | _ -> Le

(* Mame_default: reserved full-format bits are interpreted field by field. *)
let indexed input base =
  let ext = Cursor.word input in
  let index = { reg = register (ext lsr 12); long = bit ext 11;
                scale = 1 lsl field ext 9 3 } in
  if not (bit ext 8) then
    Indexed { base; disp = signed_byte ext; index = Some index; indirect = Direct }
  else
    let base = if bit ext 7 then Suppressed else base in
    let index = if bit ext 6 then None else Some index in
    let disp =
      match field ext 4 3 with
      | 2 -> Cursor.signed_word input
      | 3 -> Cursor.signed_long input
      | _ -> 0
    in
    let indirect =
      match ext land 7, index with
      | 0, _ -> Direct
      | selector, _ ->
          let outer = match selector land 3 with
            | 2 -> Cursor.signed_word input
            | 3 -> Cursor.signed_long input
            | _ -> 0
          in
          (* When the index is suppressed, both indirect orders resolve identically. *)
          if bit selector 2 && Option.is_some index then Post outer else Pre outer
    in
    Indexed { base; disp; index; indirect }

(* A six-bit EA field is kept intact until its mode has been validated. *)
let ea input size code =
  let reg = code land 7 in
  match code lsr 3 with
  | 0 -> Register (D reg)
  | 1 -> Register (A reg)
  | 2 -> Ind reg
  | 3 -> Postinc reg
  | 4 -> Predec reg
  | 5 -> Disp { reg; disp = Cursor.signed_word input }
  | 6 -> indexed input (Areg reg)
  | _ ->
      match reg with
      | 0 -> Absolute (address (Cursor.signed_word input))
      | 1 -> Absolute (Cursor.long input)
      | 2 ->
          let base = Cursor.position input in
          Pc_rel (address (base + Cursor.signed_word input))
      | 3 -> indexed input (Pc_at (Cursor.position input))
      | 4 -> Immediate (Cursor.immediate input size)
      | _ -> reject Unassigned

let data_register code = code < 8
let address_register code = 8 <= code && code < 16
let memory_alterable code = 16 <= code && code <= 57
let data_alterable code = data_register code || memory_alterable code
let pc_relative code = code = 58 || code = 59
let data code = data_alterable code || pc_relative code || code = 60
let control_alterable code = code lsr 3 = 2 || (40 <= code && code <= 57)
let control code = control_alterable code || pc_relative code
let any_ea code = code <= 60
let pair ~memory src dst =
  if memory then Predec src, Predec dst else Register (D src), Register (D dst)

let move input opcode width =
  let src = opcode land 63 in
  let dst = (field opcode 6 7 lsl 3) lor field opcode 9 7 in
  let address_allowed = width <> Byte in
  require (data src || (address_allowed && address_register src));
  require (data_alterable dst || (address_allowed && address_register dst));
  let src = ea input width src in
  if address_register dst then Movea (width, src, dst land 7)
  else Move (width, src, ea input width dst)

let bit_operation input opcode code =
  let operation = match field opcode 6 3 with
    | 0 -> Btst | 1 -> Bchg | 2 -> Bclr | _ -> Bset
  in
  let dynamic = bit opcode 8 in
  require (data_alterable code || (operation = Btst && (pc_relative code || (dynamic && code = 60))));
  let number =
    if dynamic then Data_register (field opcode 9 7)
    else Constant (Cursor.immediate input Byte)
  in
  (* The bit index wraps at 32 for Dn and at 8 for all other EAs. *)
  let width = if data_register code then Long else Byte in
  Bit (operation, number, ea input width code)

let cas_registers ext = { compare = ext land 7; update = field ext 6 7 }

let cas_operand input =
  let ext = Cursor.word input in
  { address = register (ext lsr 12); registers = cas_registers ext }

let immediate_group input opcode =
  let code = opcode land 63 in
  let opmode = field opcode 6 3 in
  let group = field opcode 9 7 in
  if bit opcode 8 then
    if address_register code then
      let width = word_or_long (bit opcode 6) in
      let mem = Disp { reg = code land 7; disp = Cursor.signed_word input } in
      let reg = Register (D group) in
      if bit opcode 7 then Movep (width, reg, mem) else Movep (width, mem, reg)
    else bit_operation input opcode code
  else if group = 4 then bit_operation input opcode code
  else if opmode = 3 then
    match group with
    | 0 | 1 | 2 ->
        require (control code);
        let ext = Cursor.word input in
        let width = size group in
        Bounds ((if bit ext 11 then Check else Compare), width,
                ea input width code, register (ext lsr 12))
    | 3 ->
        if code < 16 then Rtm (register code)
        else (
          require (control code);
          let count = Cursor.immediate input Byte in
          Callm (count, ea input Long code))
    | 5 | 6 | 7 ->
        let width = size (group - 5) in
        if code = 60 && width <> Byte then
          let first = cas_operand input in
          Cas2 (width, first, cas_operand input)
        else (
          require (memory_alterable code);
          let registers = cas_registers (Cursor.word input) in
          Cas (width, registers, ea input width code))
    | _ -> reject Unassigned
  else
    let width = size opmode in
    if group = 7 then (
      require (memory_alterable code);
      let ext = Cursor.word input in
      let direction = if bit ext 11 then To_memory else From_memory in
      Moves (direction, width, register (ext lsr 12), ea input width code))
    else
      let operation = match group with
        | 0 -> Or | 1 -> And | 2 -> Sub | 3 -> Add | 5 -> Eor | 6 -> Cmp
        | _ -> reject Unassigned
      in
      if code = 60 && (operation = Or || operation = And || operation = Eor) then (
        require (width <> Long);
        Logic_status (operation, (if width = Byte then Ccr else Sr), Cursor.immediate input width))
      else (
        require (data_alterable code || (operation = Cmp && pc_relative code));
        let value = Cursor.immediate input width in
        Alu (operation, width, Immediate value, ea input width code))

let movec input opcode =
  let ext = Cursor.word input in
  let code = ext land 0xfff in
  (* Mame_default: only the EC020 control registers are assigned. *)
  let control = match code with
    | 0x000 -> Sfc | 0x001 -> Dfc | 0x002 -> Cacr | 0x800 -> Usp
    | 0x801 -> Vbr | 0x802 -> Caar | 0x803 -> Msp | 0x804 -> Isp
    | _ -> reject (Mame_default (Movec_control code))
  in
  Movec ((if bit opcode 0 then To_control else From_control), register (ext lsr 12), control)

let multiply_divide input opcode code =
  require (data code);
  let ext = Cursor.word input in
  let low = field ext 12 7 in
  let high = ext land 7 in
  let sign = if bit ext 11 then Signed else Unsigned in
  let operand = ea input Long code in
  let wide = bit ext 10 in
  if bit opcode 6 then
    let dividend = if wide then Double { high; low } else Single low in
    Div_long { sign; operand; dividend; quotient = low; remainder = high }
  else Mul_long { sign; operand; low; high = (if wide then Some high else None) }

let movem input opcode code =
  let load = bit opcode 10 in
  let predecrement = code lsr 3 = 4 in
  let postincrement = code lsr 3 = 3 in
  require (control_alterable code || (if load then pc_relative code || postincrement else predecrement));
  let width = word_or_long (bit opcode 6) in
  let mask = Cursor.word input in
  let regs = List.init 16 Fun.id |> List.filter_map (fun position ->
    if bit mask position then
      Some (register (if predecrement then 15 - position else position))
    else None)
  in
  Movem ((if load then From_memory else To_memory), width, regs, ea input width code)

let system_group input opcode =
  let code = opcode land 63 in
  let reg = code land 7 in
  let upper_reg = field opcode 9 7 in
  let is mask value = opcode land mask = value in
  match opcode with
  | 0x4afc -> reject Illegal_opcode
  | 0x4e70 -> Reset
  | 0x4e71 -> Nop
  | 0x4e72 -> Stop (Cursor.word input)
  | 0x4e73 -> Rte
  | 0x4e74 -> Rtd (Cursor.signed_word input)
  | 0x4e75 -> Rts
  | 0x4e76 -> Trapv
  | 0x4e77 -> Rtr
  | 0x4e7a | 0x4e7b -> movec input opcode
  | _ when is 0xfff0 0x4e40 -> Trap (opcode land 15)
  | _ when is 0xfff8 0x4808 -> Link (reg, Cursor.signed_long input)
  | _ when is 0xfff8 0x4e50 -> Link (reg, Cursor.signed_word input)
  | _ when is 0xfff8 0x4e58 -> Unlk reg
  | _ when is 0xfff0 0x4e60 ->
      Move_usp ((if bit opcode 3 then From_control else To_control), reg)
  | _ when is 0xff80 0x4e80 ->
      require (control code);
      let operand = ea input Long code in
      if bit opcode 6 then Jmp operand else Jsr operand
  | _ when is 0xfff8 0x49c0 -> Ext (Byte_to_long, reg)
  | _ when is 0xf1c0 0x41c0 ->
      require (control code);
      Lea (ea input Long code, upper_reg)
  | _ when is 0xf1c0 0x4100 || is 0xf1c0 0x4180 ->
      require (data code);
      let width = if bit opcode 7 then Word else Long in
      Chk (width, ea input width code, upper_reg)
  | _ when is 0xfff8 0x4840 -> Swap reg
  | _ when is 0xfff8 0x4848 -> reject (Mame_default Breakpoint)
  | _ when is 0xffc0 0x4840 ->
      require (control code);
      Pea (ea input Long code)
  | _ when is 0xfff8 0x4880 -> Ext (Byte_to_word, reg)
  | _ when is 0xfff8 0x48c0 -> Ext (Word_to_long, reg)
  | _ when is 0xfb80 0x4880 -> movem input opcode code
  | _ when is 0xff80 0x4c00 -> multiply_divide input opcode code
  | _ when is 0xffc0 0x4800 || is 0xffc0 0x4ac0 ->
      require (data_alterable code);
      Unary ((if bit opcode 9 then Tas else Nbcd), Byte, ea input Byte code)
  | _ ->
      require (not (bit opcode 8));
      let group = field opcode 9 7 in
      let opmode = field opcode 6 3 in
      if opmode = 3 then (
        require (group <= 3);
        let status = if group = 0 || group = 3 then Sr else Ccr in
        if group < 2 then (
          require (data_alterable code);
          From_status (status, ea input Word code))
        else (
          require (data code);
          To_status (status, ea input Word code)))
      else
        let operation = match group with
          | 0 -> Negx | 1 -> Clr | 2 -> Neg | 3 -> Not | 5 -> Tst
          | _ -> reject Unassigned
        in
        let width = size opmode in
        require (if operation = Tst then data code || (width <> Byte && address_register code)
                 else data_alterable code);
        Unary (operation, width, ea input width code)

let quick_group input opcode =
  let code = opcode land 63 in
  let opmode = field opcode 6 3 in
  if opmode = 3 then
    let cond = condition (field opcode 8 15) in
    if address_register code then
      let base = Cursor.position input in
      Dbcc (cond, code land 7, address (base + Cursor.signed_word input))
    else if 58 <= code && code <= 60 then
      let operand = match code with
        | 58 -> Some (Word, Cursor.word input)
        | 59 -> Some (Long, Cursor.long input)
        | _ -> None
      in
      Trapcc (cond, operand)
    else (
      require (data_alterable code);
      Scc (cond, ea input Byte code))
  else
    let width = size opmode in
    let value = nonzero_quick (field opcode 9 7) in
    let subtract = bit opcode 8 in
    if address_register code then (
      require (width <> Byte);
      if subtract then Suba (width, Immediate value, code land 7)
      else Adda (width, Immediate value, code land 7))
    else (
      require (data_alterable code);
      Alu ((if subtract then Sub else Add), width, Immediate value, ea input width code))

let branch input opcode =
  let base = Cursor.position input in
  let disp = match opcode land 255 with
    | 0 -> Cursor.signed_word input
    | 255 -> Cursor.signed_long input
    | byte -> signed_byte byte
  in
  let target = address (base + disp) in
  match field opcode 8 15 with
  | 0 -> Bra target | 1 -> Bsr target | cond -> Bcc (condition cond, target)

let arithmetic_group input opcode group =
  let code = opcode land 63 in
  let src = code land 7 in
  let dst = field opcode 9 7 in
  let mode = field opcode 6 7 in
  let is mask value = opcode land mask = value in
  let operands () = pair ~memory:(bit opcode 3) src dst in
  let data_reg = Register (D dst) in
  if is 0xf1f0 0x8100 then
    let src, dst = operands () in Sbcd (src, dst)
  else if is 0xf1f0 0x8140 then
    let src, dst = operands () in Pack (src, dst, Cursor.signed_word input)
  else if is 0xf1f0 0x8180 then
    let src, dst = operands () in Unpk (src, dst, Cursor.signed_word input)
  else if is 0xf1f0 0xc100 then
    let src, dst = operands () in Abcd (src, dst)
  else if is 0xf1f8 0xc140 then Exg (D dst, D src)
  else if is 0xf1f8 0xc148 then Exg (A dst, A src)
  else if is 0xf1f8 0xc188 then Exg (D dst, A src)
  else if mode = 3 || mode = 7 then
    let width = word_or_long (mode = 7) in
    match group with
    | 8 | 12 ->
        require (data code);
        let sign = if mode = 7 then Signed else Unsigned in
        let operand = ea input Word code in
        if group = 8 then Div_word (sign, operand, dst)
        else Mul_word (sign, operand, dst)
    | _ ->
        require (any_ea code);
        let operand = ea input width code in
        if group = 9 then Suba (width, operand, dst)
        else if group = 11 then Cmpa (width, operand, dst)
        else Adda (width, operand, dst)
  else
    let width = size (mode land 3) in
    let operation = match group with
      | 8 -> Or | 9 -> Sub | 11 -> if mode < 4 then Cmp else Eor
      | 12 -> And | _ -> Add
    in
    if mode < 4 then (
      require (data code || (width <> Byte && address_register code
                             && (operation = Add || operation = Sub || operation = Cmp)));
      Alu (operation, width, ea input width code, data_reg))
    else if (group = 9 || group = 13) && code < 16 then
      let src, dst = operands () in
      if group = 9 then Subx (width, src, dst) else Addx (width, src, dst)
    else if group = 11 && address_register code then
      Alu (Cmp, width, Postinc src, Postinc dst)
    else (
      require (memory_alterable code || (group = 11 && data_register code));
      Alu (operation, width, data_reg, ea input width code))

let shift = function
  | 0 -> Asr | 1 -> Asl | 2 -> Lsr | 3 -> Lsl | 4 -> Roxr | 5 -> Roxl | 6 -> Ror | _ -> Rol

let shift_group input opcode =
  let code = opcode land 63 in
  let opmode = field opcode 6 3 in
  let upper = field opcode 9 7 in
  if opmode <> 3 then
    let operation = shift ((field opcode 3 3 lsl 1) lor field opcode 8 1) in
    let count = if bit opcode 5 then Data_register upper else Constant (nonzero_quick upper) in
    Shift (operation, size opmode, count, Register (D (opcode land 7)))
  else if not (bit opcode 11) then (
    require (memory_alterable code);
    Shift (shift (field opcode 8 7), Word, Constant 1, ea input Word code))
  else
    let selector = field opcode 8 7 in
    let readonly = List.mem selector [0; 1; 3; 5] in
    require (data_register code || control_alterable code || (readonly && pc_relative code));
    let ext = Cursor.word input in
    let reg = field ext 12 7 in
    let operation = match selector with
      | 0 -> Bftst | 1 -> Bfextu reg | 2 -> Bfchg | 3 -> Bfexts reg
      | 4 -> Bfclr | 5 -> Bfffo reg | 6 -> Bfset | _ -> Bfins reg
    in
    let offset =
      if bit ext 11 then Data_register (field ext 6 7) else Constant (field ext 6 31)
    in
    let width =
      if bit ext 5 then Data_register (ext land 7) else Constant (((ext - 1) land 31) + 1)
    in
    Bitfield { operation; offset; width; operand = ea input Long code }

let primary input opcode =
  match opcode lsr 12 with
  | 0 -> immediate_group input opcode
  | 1 -> move input opcode Byte
  | 2 -> move input opcode Long
  | 3 -> move input opcode Word
  | 4 -> system_group input opcode
  | 5 -> quick_group input opcode
  | 6 -> branch input opcode
  | 7 ->
      require (not (bit opcode 8));
      Moveq (signed_byte opcode, field opcode 9 7)
  | 10 -> reject ~vector:10 A_line
  | 14 -> shift_group input opcode
  | 15 -> reject ~vector:11 (Mame_default F_line)
  | group -> arithmetic_group input opcode group

let at image pc =
  let input : Cursor.t = { image; pc; length = 0 } in
  try
    let opcode = Cursor.word input in
    let op = primary input opcode in
    Insn { pc; length = input.length; op }
  with Reject (vector, reason) -> Illegal { vector; reason; length = input.length }

(* Privilege applies before MOVEC's extension validation. *)
let exception_vector ~supervisor = function
  | Illegal { reason = Mame_default (Movec_control _); _ } when not supervisor -> Some 8
  | Illegal { vector; _ } -> Some vector
  | Insn { op; _ } when not supervisor && privileged op -> Some 8
  | Insn _ -> None
