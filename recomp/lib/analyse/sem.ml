type width = Insn.size = Byte | Word | Long
type temp = int
type register = D of int | A of int | Usp
  | Sfc | Dfc | Cacr | Vbr | Caar | Msp | Isp
(* Shr is logical, Sar arithmetic (on the 32-bit value); Mul keeps the low 32 bits. *)
type binop = Add | Sub | And | Or | Xor | Shl | Shr | Sar | Mul | Eq | Ne | Lt
type expr =
  | Const of int
  | Temp of temp
  | Reg of register
  | Sr
  | Ccr
  | Supervisor
  | Bin of binop * expr * expr
  | Sext of width * expr
  | Cond of Insn.cond
  | Flag of Ccr.flag

type arithmetic = { src : expr; dst : expr; result : expr; width : width }
type producer =
  | Logic of { result : expr; width : width }
  | Add of arithmetic
  | Sub of arithmetic
  | Cmp of arithmetic

type stmt =
  | Let of temp * expr
  | Load of temp * width * expr
  | Store of width * expr * expr
  | Store_long_low_first of expr * expr
  | Set_reg of register * width * expr
  | Flags of producer
  | Set_flags of (Ccr.flag * expr) list
  | Set_ccr of expr
  | If of expr * stmt list * stmt list
  | Helper of expr Helper.t
  | Runtime of expr Runtime_call.t
  | Raise of { vector : int; return_pc : Pc.t }
  | Goto of expr
  | Stop of Pc.t
  | Repeat of stmt list (* Falling through the body starts the next iteration. *)

type t = Body of stmt list | Unsupported of string

let bytes = function Byte -> 1 | Word -> 2 | Long -> 4
let mask = function Byte -> 0xff | Word -> 0xffff | Long -> 0xffffffff
let sign width = (mask width lsr 1) + 1
let truncate width expr = Bin (And, expr, Const (mask width))
