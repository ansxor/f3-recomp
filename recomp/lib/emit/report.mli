(** Shared bodies of the analysis subcommands. The command line prints the
    text forms and [Json] forms from the same data; the JSON server returns
    the [Json] forms. *)

(** {1 coverage} *)

val coverage_counts : Game_image.t -> (string * int) list

val coverage_text : (string * int) list -> string

(** [{game, unsupported: {op_name: count}, total}] *)
val coverage_json : game:string -> (string * int) list -> Json.t

(** {1 check-irq} *)

type irq = {
  game : string;
  interrupt_observes : Ccr.t;
  handlers : (int * Irq_contract.verdict) list;
  undeclared : int -> bool;
}

val check_irq : Program.t -> irq
val check_irq_text : irq -> string
val check_irq_json : irq -> Json.t

(** {1 decisions} *)

(** The TSV dump, or only the sprite rows when [sprite_only]. [sprite_header]
    and [sprite_stats] name files to write as a side effect. *)
val decisions_tsv :
  ?sprite_header:string -> ?sprite_stats:string -> ?writers:Sprite_trace.seed list ->
  ?profile:Selection.profile -> sprite_only:bool -> Program.t -> string

(** [{game, sprite_only, columns, rows: [{pc, disposition, length, flow, ...}]}] *)
val decisions_json : game:string -> sprite_only:bool -> string -> Json.t

(** {1 lower} *)

val parse_hex : string -> string
val parse_live_out : string -> Ccr.t

type lowered = {
  line : Inspect.line;
  c : (C.stmt list, string) result;  (** The C statements, or why they cannot be lowered. *)
}

val lower : Image.t -> pc:Pc.t -> count:int -> live_out:Ccr.t -> lowered list

(** The text printed by [f3recomp lower]. Raises [Error.Invalid] for the first
    instruction that cannot be lowered. *)
val lower_text : count:int -> lowered list -> string

(** [{pc, count, live_out, instructions: [{...disasm fields, c | error}]}] *)
val lower_json : live_out:Ccr.t -> lowered list -> Json.t
