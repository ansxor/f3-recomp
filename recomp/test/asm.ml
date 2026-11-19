open F3recomp
include Insn

let invalid message = invalid_arg ("Asm: " ^ message)
let require condition message = if not condition then invalid message
let signed bits value =
  let limit = 1 lsl (bits - 1) in
  -limit <= value && value < limit

let displacement base target =
  let delta = address (target - base) in
  if delta >= 0x80000000 then delta - 0x100000000 else delta

let word value = [value land 0xffff]
let long value = [(value lsr 16) land 0xffff; value land 0xffff]
let literal size value =
  match size with Byte -> word (value land 0xff) | Word -> word value | Long -> long value

let size_code = function Byte -> 0 | Word -> 1 | Long -> 2
let word_long = function Word -> 0 | Long -> 1 | Byte -> invalid "expected word or long"
let reg_code = function D n -> n | A n -> 8 lor n
let flag mask condition = if condition then mask else 0
let cond_code = function
  | T -> 0 | F -> 1 | Hi -> 2 | Ls -> 3 | Cc -> 4 | Cs -> 5 | Ne -> 6 | Eq -> 7
  | Vc -> 8 | Vs -> 9 | Pl -> 10 | Mi -> 11 | Ge -> 12 | Lt -> 13 | Gt -> 14 | Le -> 15

type encoding = { choose : 'a. 'a list -> 'a }

let shortest = { choose = List.hd }

(* Prefer the shortest displacement, with the defined null selector for zero. *)
let full_disps value =
  (if value = 0 then [1, []] else [])
  @ (if signed 16 value then [2, word value] else [])
  @ [3, long value]

let ea encoding ~pc size = function
  | Register reg -> reg_code reg, []
  | Ind reg -> 0x10 lor reg, []
  | Postinc reg -> 0x18 lor reg, []
  | Predec reg -> 0x20 lor reg, []
  | Disp { reg; disp } ->
      require (signed 16 disp) "address displacement out of range";
      0x28 lor reg, word disp
  | Absolute target ->
      let short = displacement 0 target in
      encoding.choose ((if signed 16 short then [0x38, word short] else []) @ [0x39, long target])
  | Pc_rel target ->
      let disp = displacement pc target in
      require (signed 16 disp) "PC-relative displacement out of range";
      0x3a, word disp
  | Immediate value -> 0x3c, literal size value
  | Indexed { base; disp; index; indirect } ->
      let code = match base with
        | Areg reg -> 0x30 lor reg
        | Suppressed -> 0x30
        | Pc_at origin ->
            require (origin = pc) "indexed PC must be the extension word address";
            0x3b
      in
      let index_bits = match index with
        | None -> 0
        | Some { reg; long; scale } ->
            let scale_bits = match scale with
              | 1 -> 0 | 2 -> 1 | 4 -> 2 | 8 -> 3 | _ -> invalid "invalid index scale"
            in
            (reg_code reg lsl 12) lor flag 0x800 long lor (scale_bits lsl 9)
      in
      let brief =
        if base <> Suppressed && Option.is_some index && indirect = Direct && signed 8 disp then
          [word (index_bits lor (disp land 0xff))]
        else []
      in
      let outer_disps = match indirect with
        | Direct -> [0, []]
        | Pre outer -> full_disps outer
        | Post outer ->
            require (Option.is_some index) "postindex requires an index register";
            List.map (fun (selector, words) -> 4 lor selector, words) (full_disps outer)
      in
      (* Prefer brief format. In full format clear ignored register and reserved bits. *)
      let full = List.concat_map (fun (bd, base_words) ->
        List.map (fun (selector, outer_words) ->
          let ext = index_bits lor 0x100 lor flag 0x80 (base = Suppressed)
                    lor flag 0x40 (Option.is_none index) lor (bd lsl 4) lor selector in
          ext :: (base_words @ outer_words)) outer_disps) (full_disps disp)
      in
      code, encoding.choose (brief @ full)

let immediate_group = function Or -> 0 | And -> 1 | Sub -> 2 | Add -> 3 | Eor -> 5 | Cmp -> 6
let arithmetic_group = function Or -> 8 | Sub -> 9 | Cmp | Eor -> 11 | And -> 12 | Add -> 13
let shift_code = function Asr -> 0 | Asl -> 1 | Lsr -> 2 | Lsl -> 3 | Roxr -> 4 | Roxl -> 5 | Ror -> 6 | Rol -> 7
let bit_code = function Btst -> 0 | Bchg -> 1 | Bclr -> 2 | Bset -> 3
let field_count dynamic shift = function
  | Constant value -> (value land 31) lsl shift
  | Data_register reg -> dynamic lor (reg lsl shift)

let words ?(encoding = shortest) ~pc op =
  let at prefix size operand = ea encoding ~pc:(pc + 2 + 2 * List.length prefix) size operand in
  let with_ea ?(prefix = []) opcode size operand =
    let code, extension = at prefix size operand in
    (opcode lor code) :: (prefix @ extension)
  in
  let move size src dst =
    let src_code, src_words = at [] size src in
    let dst_code, dst_words = at src_words size dst in
    let group = match size with Byte -> 1 | Long -> 2 | Word -> 3 in
    ((group lsl 12) lor ((dst_code land 7) lsl 9) lor ((dst_code lsr 3) lsl 6) lor src_code)
    :: (src_words @ dst_words)
  in
  let arithmetic operation = arithmetic_group operation lsl 12 in
  let sized opcode size = opcode lor (size_code size lsl 6) in
  let address_alu operation size src dst =
    (* Prefer ADDQ/SUBQ for address-register immediates 1 through 8. *)
    match operation, src with
    | (Add | Sub), Immediate value when 1 <= value && value <= 8 ->
        let quick = word (sized 0x5008 size lor ((value land 7) lsl 9) lor flag 0x100 (operation = Sub) lor dst) in
        let regular = (arithmetic operation lor (dst lsl 9) lor 0xfc lor (word_long size lsl 8))
                      :: literal size value in
        encoding.choose [quick; regular]
    | _ ->
        with_ea (arithmetic operation lor (dst lsl 9) lor 0xc0 lor (word_long size lsl 8)) size src
  in
  let pair opcode src dst extension =
    let bits = match src, dst with
      | Register (D src), Register (D dst) -> (dst lsl 9) lor src
      | Predec src, Predec dst -> (dst lsl 9) lor 8 lor src
      | _ -> invalid "expected a data-register pair or a predecrement pair"
    in
    (opcode lor bits) :: extension
  in
  let branch cond target =
    let opcode = 0x6000 lor (cond lsl 8) in
    let disp = displacement (pc + 2) target in
    (* Shortest branch, excluding the byte sentinels 0 and -1. *)
    let short =
      if signed 8 disp && disp <> 0 && disp <> -1 then [word (opcode lor (disp land 0xff))] else []
    in
    encoding.choose (short @ (if signed 16 disp then [opcode :: word disp] else [])
                            @ [(opcode lor 0xff) :: long disp])
  in
  let long_muldiv opcode sign operand low high wide =
    let ext = (low lsl 12) lor flag 0x800 (sign = Signed) lor flag 0x400 wide lor high in
    with_ea ~prefix:[ext] opcode Long operand
  in
  let cas_bits { compare; update } = (update lsl 6) lor compare in
  let cas_ext { address; registers } = (reg_code address lsl 12) lor cas_bits registers in
  match op with
  | Move (size, src, dst) -> move size src dst
  | Movea (size, src, dst) -> move size src (Register (A dst))
  | Moveq (value, dst) -> word (0x7000 lor (dst lsl 9) lor (value land 0xff))
  | Alu (Cmp, size, Postinc src, Postinc dst) ->
      word (sized 0xb108 size lor (dst lsl 9) lor src)
  | Alu (operation, size, Immediate value, dst) ->
      let immediate () =
        with_ea ~prefix:(literal size value) (sized (immediate_group operation lsl 9) size) size dst
      in
      (* Prefer quick encodings, then the dedicated immediate opcode to EA-to-Dn. *)
      if (operation = Add || operation = Sub) && 1 <= value && value <= 8 then
        let quick () =
          with_ea (sized 0x5000 size lor ((value land 7) lsl 9) lor flag 0x100 (operation = Sub)) size dst
        in
        (encoding.choose [quick; immediate]) ()
      else immediate ()
  | Alu (operation, size, src, Register (D dst)) when operation <> Eor ->
      (* Prefer the EA-to-register form for two data registers. *)
      with_ea (sized (arithmetic operation lor (dst lsl 9)) size) size src
  | Alu (operation, size, Register (D src), dst) ->
      with_ea (sized (arithmetic operation lor (src lsl 9) lor 0x100) size) size dst
  | Alu _ -> invalid "invalid ALU operands"
  | Adda (size, src, dst) -> address_alu Add size src dst
  | Suba (size, src, dst) -> address_alu Sub size src dst
  | Cmpa (size, src, dst) -> address_alu Cmp size src dst
  | Addx (size, src, dst) -> pair (sized 0xd100 size) src dst []
  | Subx (size, src, dst) -> pair (sized 0x9100 size) src dst []
  | Unary (operation, size, operand) ->
      let opcode = match operation with
        | Negx -> sized 0x4000 size | Clr -> sized 0x4200 size | Neg -> sized 0x4400 size
        | Not -> sized 0x4600 size | Tst -> sized 0x4a00 size | Nbcd -> 0x4800 | Tas -> 0x4ac0
      in
      with_ea opcode size operand
  | Ext (kind, reg) ->
      let opcode = match kind with Byte_to_word -> 0x4880 | Word_to_long -> 0x48c0 | Byte_to_long -> 0x49c0 in
      word (opcode lor reg)
  | Swap reg -> word (0x4840 lor reg)
  | Shift (operation, size, count, Register (D reg)) ->
      let kind = shift_code operation in
      let count = match count with
        | Constant value -> (value land 7) lsl 9
        | Data_register reg -> (reg lsl 9) lor 0x20
      in
      word (sized 0xe000 size lor count lor ((kind land 1) lsl 8) lor ((kind lsr 1) lsl 3) lor reg)
  | Shift (operation, Word, Constant 1, operand) ->
      with_ea (0xe0c0 lor (shift_code operation lsl 8)) Word operand
  | Shift _ -> invalid "memory shifts require a word operand and count of one"
  | Bit (operation, count, operand) ->
      let opcode = bit_code operation lsl 6 in
      (match count with
       | Constant value -> with_ea ~prefix:(literal Byte value) (0x0800 lor opcode) Byte operand
       | Data_register reg -> with_ea (0x0100 lor (reg lsl 9) lor opcode) Byte operand)
  | Bitfield { operation; offset; width; operand } ->
      let selector, reg = match operation with
        | Bftst -> 0, 0 | Bfextu reg -> 1, reg | Bfchg -> 2, 0 | Bfexts reg -> 3, reg
        | Bfclr -> 4, 0 | Bfffo reg -> 5, reg | Bfset -> 6, 0 | Bfins reg -> 7, reg
      in
      let ext = (reg lsl 12) lor field_count 0x800 6 offset lor field_count 0x20 0 width in
      with_ea ~prefix:[ext] (0xe8c0 lor (selector lsl 8)) Long operand
  | Mul_word (sign, operand, dst) | Div_word (sign, operand, dst) ->
      let opcode = match op with Mul_word _ -> 0xc0c0 | _ -> 0x80c0 in
      with_ea (opcode lor flag 0x100 (sign = Signed) lor (dst lsl 9)) Word operand
  | Mul_long { sign; operand; low; high } ->
      long_muldiv 0x4c00 sign operand low (Option.value ~default:0 high) (Option.is_some high)
  | Div_long { sign; operand; dividend; quotient; remainder } ->
      let wide = match dividend with
        | Single low -> require (low = quotient) "dividend and quotient registers differ"; false
        | Double { high; low } ->
            require (low = quotient && high = remainder) "dividend and result registers differ";
            true
      in
      long_muldiv 0x4c40 sign operand quotient remainder wide
  | Abcd (src, dst) -> pair 0xc100 src dst []
  | Sbcd (src, dst) -> pair 0x8100 src dst []
  | Pack (src, dst, adjustment) -> pair 0x8140 src dst (word adjustment)
  | Unpk (src, dst, adjustment) -> pair 0x8180 src dst (word adjustment)
  | Cas (size, registers, operand) ->
      with_ea ~prefix:[cas_bits registers] (0x0ac0 + (size_code size lsl 9)) size operand
  | Cas2 (size, first, second) ->
      [0x0cfc lor (word_long size lsl 9); cas_ext first; cas_ext second]
  | Chk (size, operand, dst) ->
      with_ea (0x4100 lor (dst lsl 9) lor flag 0x80 (word_long size = 0)) size operand
  | Bounds (kind, size, operand, reg) ->
      let ext = (reg_code reg lsl 12) lor flag 0x800 (kind = Check) in
      with_ea ~prefix:[ext] (0x00c0 lor (size_code size lsl 9)) size operand
  | Bra target -> branch 0 target
  | Bsr target -> branch 1 target
  | Bcc (cond, target) ->
      require (cond <> T && cond <> F) "use BRA or BSR for these branch conditions";
      branch (cond_code cond) target
  | Dbcc (cond, reg, target) ->
      let disp = displacement (pc + 2) target in
      require (signed 16 disp) "DBcc displacement out of range";
      (0x50c8 lor (cond_code cond lsl 8) lor reg) :: word disp
  | Scc (cond, operand) -> with_ea (0x50c0 lor (cond_code cond lsl 8)) Byte operand
  | Jmp operand -> with_ea 0x4ec0 Long operand
  | Jsr operand -> with_ea 0x4e80 Long operand
  | Rts -> [0x4e75]
  | Rtd value -> 0x4e74 :: word value
  | Rtr -> [0x4e77]
  | Rte -> [0x4e73]
  | Trap vector -> word (0x4e40 lor vector)
  | Trapcc (cond, value) ->
      let opcode = 0x5000 lor (cond_code cond lsl 8) in
      (match value with
       | None -> [opcode lor 0xfc]
       | Some (Word, value) -> (opcode lor 0xfa) :: word value
       | Some (Long, value) -> (opcode lor 0xfb) :: long value
       | Some (Byte, _) -> invalid "TRAPcc has no byte immediate")
  | Trapv -> [0x4e76]
  | Callm (count, operand) -> with_ea ~prefix:(literal Byte count) 0x06c0 Long operand
  | Rtm reg -> word (0x06c0 lor reg_code reg)
  | Lea (operand, reg) -> with_ea (0x41c0 lor (reg lsl 9)) Long operand
  | Pea operand -> with_ea 0x4840 Long operand
  | Link (reg, disp) ->
      encoding.choose ((if signed 16 disp then [(0x4e50 lor reg) :: word disp] else [])
                      @ [(0x4808 lor reg) :: long disp])
  | Unlk reg -> word (0x4e58 lor reg)
  | Exg (D dst, D src) -> word (0xc140 lor (dst lsl 9) lor src)
  | Exg (A dst, A src) -> word (0xc148 lor (dst lsl 9) lor src)
  | Exg (D dst, A src) -> word (0xc188 lor (dst lsl 9) lor src)
  | Exg (A _, D _) -> invalid "mixed EXG is represented with the data register first"
  | Movem (direction, size, registers, operand) ->
      let position reg = match operand with Predec _ -> 15 - reg_code reg | _ -> reg_code reg in
      let mask = List.fold_left (fun mask reg -> mask lor (1 lsl position reg)) 0 registers in
      let opcode = 0x4880 lor flag 0x400 (direction = From_memory) lor (word_long size lsl 6) in
      with_ea ~prefix:[mask] opcode size operand
  | Movep (size, src, dst) ->
      let reg, base, disp, store = match src, dst with
        | Register (D reg), Disp { reg = base; disp } -> reg, base, disp, true
        | Disp { reg = base; disp }, Register (D reg) -> reg, base, disp, false
        | _ -> invalid "MOVEP needs a data register and an address displacement"
      in
      (0x0108 lor (reg lsl 9) lor flag 0x80 store lor (word_long size lsl 6) lor base) :: word disp
  | Moves (direction, size, reg, operand) ->
      let ext = (reg_code reg lsl 12) lor flag 0x800 (direction = To_memory) in
      with_ea ~prefix:[ext] (sized 0x0e00 size) size operand
  | Movec (direction, reg, control) ->
      let code = match control with
        | Sfc -> 0 | Dfc -> 1 | Cacr -> 2 | Usp -> 0x800
        | Vbr -> 0x801 | Caar -> 0x802 | Msp -> 0x803 | Isp -> 0x804
      in
      [0x4e7a lor flag 1 (direction = To_control); (reg_code reg lsl 12) lor code]
  | From_status (status, operand) -> with_ea (0x40c0 lor flag 0x200 (status = Ccr)) Word operand
  | To_status (status, operand) -> with_ea (0x44c0 lor flag 0x200 (status = Sr)) Word operand
  | Move_usp (direction, reg) -> word (0x4e60 lor flag 8 (direction = From_control) lor reg)
  | Logic_status ((Or | And | Eor as operation), status, value) ->
      let width = if status = Sr then Word else Byte in
      sized ((immediate_group operation lsl 9) lor 0x3c) width :: literal width value
  | Logic_status _ -> invalid "invalid status-register operation"
  | Stop value -> 0x4e72 :: word value
  | Reset -> [0x4e70]
  | Nop -> [0x4e71]

let bytes_of_words words =
  let bytes = Bytes.create (2 * List.length words) in
  List.iteri (fun index word -> Bytes.set_uint16_be bytes (2 * index) word) words;
  Bytes.unsafe_to_string bytes

let length ~pc op = 2 * List.length (words ~pc op)

let encode { pc; length; op } =
  (* Width alternatives are tried in canonical order. An earlier EA can change
     the PC of a later extension, so evaluate each choice with its own layout. *)
  let rec search = function
    | [] -> invalid "no encoding matches the instruction length and operands"
    | path :: pending ->
        let remaining = ref path and prefix = ref [] and alternatives = ref pending in
        let choose options =
          let index = match !remaining with
            | index :: rest -> remaining := rest; index
            | [] ->
                let siblings = List.mapi (fun index _ -> List.rev (index :: !prefix)) options in
                alternatives := List.tl siblings @ !alternatives;
                0
          in
          prefix := index :: !prefix;
          List.nth options index
        in
        let candidate = try Some (words ~encoding:{ choose } ~pc op) with Invalid_argument _ -> None in
        match candidate with
        | Some words when 2 * List.length words = length -> bytes_of_words words
        | _ -> search !alternatives
  in
  search [[]]

type branch = Always | Subroutine | Conditional of cond | Decrement of cond * int

type statement =
  | Instruction of op
  | Label of string
  | Branch of branch * string

let insn op = Instruction op
let label name = Label name
let d reg = Register (D reg)
let a reg = Register (A reg)
let imm value = Immediate value
let moveq value dst =
  match dst with Register (D reg) -> insn (Moveq (value, reg)) | _ -> invalid "MOVEQ needs Dn"
let move size src dst = insn (match dst with Register (A reg) -> Movea (size, src, reg) | _ -> Move (size, src, dst))
let add size src dst = insn (match dst with Register (A reg) -> Adda (size, src, reg) | _ -> Alu (Add, size, src, dst))
let bra name = Branch (Always, name)
let bsr name = Branch (Subroutine, name)
let bcc cond name = Branch (Conditional cond, name)
let dbcc cond reg name = Branch (Decrement (cond, reg), name)

module Labels = Map.Make (String)

let assemble ~base statements =
  (* Label branches use fixed widths so layout and PC-relative operands stay stable. *)
  let branch_words pc branch target =
    let disp = displacement (pc + 2) target in
    match branch with
    | Always -> 0x60ff :: long disp
    | Subroutine -> 0x61ff :: long disp
    | Conditional cond ->
        require (cond <> T && cond <> F) "use BRA or BSR for these branch conditions";
        (0x60ff lor (cond_code cond lsl 8)) :: long disp
    | Decrement (cond, reg) -> words ~pc (Dbcc (cond, reg, target))
  in
  let layout (pc, labels) = function
    | Instruction op -> pc + length ~pc op, labels
    | Label name ->
        require (not (Labels.mem name labels)) ("duplicate label " ^ name);
        pc, Labels.add name pc labels
    | Branch (Decrement _, _) -> pc + 4, labels
    | Branch _ -> pc + 6, labels
  in
  let _, labels = List.fold_left layout (base, Labels.empty) statements in
  let emit (pc, buffer) statement =
    let encoded = match statement with
      | Label _ -> []
      | Instruction op -> words ~pc op
      | Branch (branch, name) ->
          let target = match Labels.find_opt name labels with
            | Some target -> target | None -> invalid ("undefined label " ^ name)
          in
          branch_words pc branch target
    in
    let bytes = bytes_of_words encoded in
    Buffer.add_string buffer bytes;
    pc + String.length bytes, buffer
  in
  let _, buffer = List.fold_left emit (base, Buffer.create 64) statements in
  { Image.base; bytes = Buffer.contents buffer }
