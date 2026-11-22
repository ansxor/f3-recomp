(** Motorola-syntax text for decoded 68EC020 instructions, and the static
    memory and control references an instruction makes. *)

(** Mnemonic with its size suffix, e.g. ["move.l"], ["adda.w"], ["bne"],
    ["movem.l"], ["cas2.l"]. The decoder does not record whether an
    ADD/SUB/ADDA/SUBA immediate used the quick or the full encoding, so
    immediates from 1 to 8 print as [addq]/[subq] and the rest as [addi]/[subi]. *)
val mnemonic : Insn.op -> string

(** Operands, comma separated, empty when there are none. Branch targets and
    absolute addresses print as ["$009b32"]; PC-relative operands print their
    resolved address as ["$00a1b2(pc)"]. *)
val operands : Insn.op -> string

val ea_to_string : Insn.ea -> string

(** [mnemonic] alone, or [mnemonic ^ " " ^ operands]. *)
val to_string : Insn.t -> string

type role = Read | Write | Rmw | Call | Branch | Lea

val role_name : role -> string

(** One static memory or code address an instruction names. [target] is the
    resolved address, folded to 32 bits. It is known for absolute, PC-relative,
    zero-base and, when [a5] is given, [disp(a5)] operands. [a5_disp] keeps the
    displacement of [disp(a5)] operands even when [a5] is unknown. [size] is
    the access width in bytes (0 for code addresses and LEA/PEA). [indexed]
    marks operands where a run-time index is added to [target]. *)
type reference = {
  role : role;
  target : int option;
  a5_disp : int option;
  size : int;
  indexed : bool;
}

val references : ?a5:int -> Insn.t -> reference list
