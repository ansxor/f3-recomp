type size = Byte | Word | Long
type cond = T | F | Hi | Ls | Cc | Cs | Ne | Eq | Vc | Vs | Pl | Mi | Ge | Lt | Gt | Le
type reg = D of int | A of int
type index = { reg : reg; long : bool; scale : int }
type base = Areg of int | Pc_at of int | Suppressed
type indirect = Direct | Pre of int | Post of int

type ea =
  | Register of reg
  | Ind of int
  | Postinc of int
  | Predec of int
  | Disp of { reg : int; disp : int }
  | Indexed of { base : base; disp : int; index : index option; indirect : indirect }
  | Absolute of int
  | Pc_rel of int
  | Immediate of int

type alu = Add | Sub | And | Or | Eor | Cmp
type unary = Negx | Neg | Not | Clr | Tst | Nbcd | Tas
type extend = Byte_to_word | Word_to_long | Byte_to_long
type direction = To_memory | From_memory
type status = Sr | Ccr
type signedness = Signed | Unsigned
type shift = Asr | Asl | Lsr | Lsl | Roxr | Roxl | Ror | Rol
type bit_op = Btst | Bchg | Bclr | Bset
type count = Constant of int | Data_register of int
type bitfield_op = Bftst | Bfextu of int | Bfchg | Bfexts of int
                 | Bfclr | Bfffo of int | Bfset | Bfins of int
type bounds = Check | Compare
type control = Sfc | Dfc | Cacr | Usp | Vbr | Caar | Msp | Isp
type control_move = From_control | To_control
type cas_registers = { compare : int; update : int }
type cas_operand = { address : reg; registers : cas_registers }
type dividend = Single of int | Double of { high : int; low : int }

type op =
  | Move of size * ea * ea
  | Movea of size * ea * int
  | Moveq of int * int
  | Alu of alu * size * ea * ea
  | Adda of size * ea * int
  | Suba of size * ea * int
  | Cmpa of size * ea * int
  | Addx of size * ea * ea
  | Subx of size * ea * ea
  | Unary of unary * size * ea
  | Ext of extend * int
  | Swap of int
  | Shift of shift * size * count * ea
  | Bit of bit_op * count * ea
  | Bitfield of { operation : bitfield_op; offset : count; width : count; operand : ea }
  | Mul_word of signedness * ea * int
  | Mul_long of { sign : signedness; operand : ea; low : int; high : int option }
  | Div_word of signedness * ea * int
  | Div_long of { sign : signedness; operand : ea; dividend : dividend;
                  quotient : int; remainder : int }
  | Abcd of ea * ea
  | Sbcd of ea * ea
  | Pack of ea * ea * int
  | Unpk of ea * ea * int
  | Cas of size * cas_registers * ea
  | Cas2 of size * cas_operand * cas_operand
  | Chk of size * ea * int
  | Bounds of bounds * size * ea * reg
  | Bra of int
  | Bsr of int
  | Bcc of cond * int
  | Dbcc of cond * int * int
  | Scc of cond * ea
  | Jmp of ea
  | Jsr of ea
  | Rts
  | Rtd of int
  | Rtr
  | Rte
  | Trap of int
  | Trapcc of cond * (size * int) option
  | Trapv
  | Callm of int * ea
  | Rtm of reg
  | Lea of ea * int
  | Pea of ea
  | Link of int * int
  | Unlk of int
  | Exg of reg * reg
  | Movem of direction * size * reg list * ea
  | Movep of size * ea * ea
  | Moves of direction * size * reg * ea
  | Movec of control_move * reg * control
  | From_status of status * ea
  | To_status of status * ea
  | Move_usp of control_move * int
  | Logic_status of alu * status * int
  | Stop of int
  | Reset
  | Nop

type t = { pc : Pc.t; length : int; op : op }

let address value = value land 0xffffffff

let static_address = function
  | Absolute pc | Pc_rel pc -> Some pc
  | Indexed { base = Suppressed; disp; index = None; indirect = Direct } -> Some (address disp)
  | Indexed { base = Pc_at pc; disp; index = None; indirect = Direct } -> Some (address (pc + disp))
  | _ -> None

let auto_step size reg =
  match size with Byte -> if reg = 7 then 2 else 1 | Word -> 2 | Long -> 4

let privileged = function
  | Movec _ | Moves _ | Move_usp _ | From_status (Sr, _) | To_status (Sr, _)
  | Logic_status (_, Sr, _) | Stop _ | Reset | Rte -> true
  | _ -> false
