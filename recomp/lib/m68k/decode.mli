type mame_default = Breakpoint | F_line | Movec_control of int
type reason = Unassigned | Illegal_opcode | A_line | Truncated | Mame_default of mame_default
type illegal = { vector : int; reason : reason; length : int }
type decoded = Insn of Insn.t | Illegal of illegal

(** Decode one instruction. Length on rejection is the number of bytes fetched.
    MOVEC validation describes supervisor-mode decoding. *)
val at : Image.t -> Pc.t -> decoded

(** The entry-time exception, if any, with the current privilege applied. *)
val exception_vector : supervisor:bool -> decoded -> int option
