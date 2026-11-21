include module type of struct include F3recomp.Insn end

(** Encode a decoded instruction at its recorded PC and length. When several
    encodings fit, prefer brief indexes, short displacements, quick arithmetic,
    and zero in unused fields. *)
val encode : t -> string

(** Size of the preferred encoding at this PC. *)
val length : pc:int -> op -> int

type statement

val insn : op -> statement
val label : string -> statement
val d : int -> ea
val a : int -> ea
val imm : int -> ea
val moveq : int -> ea -> statement
val move : size -> ea -> ea -> statement
val add : size -> ea -> ea -> statement
val bra : string -> statement
val bsr : string -> statement
val bcc : cond -> string -> statement
val dbcc : cond -> int -> string -> statement

(** Assemble at [base], resolving labels in both directions. Symbolic BRA, BSR
    and Bcc use long displacements. Symbolic DBcc uses a word displacement. *)
val assemble : base:int -> statement list -> F3recomp.Image.t
