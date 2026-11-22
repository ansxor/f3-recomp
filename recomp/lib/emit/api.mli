(** The structured analysis API behind the [--json] flags and the [f3recomp mcp] tools.
    A session caches loaded games, discovery results, profiles and analyses, so
    repeated queries against one game are cheap. *)

type session

val session : unit -> session

(** Drop cached state for one game, or everything when no game is given. *)
val close : session -> ?game:string * string -> unit -> unit

type profile = Tiers of string | Slim of string

(** Where instruction bytes come from: a ROM region of a game, or literal bytes
    placed at the requested PC. Sound listings use the sound CPU's 0xc00000
    address space, and a PC below the region size is taken as an offset. *)
type source =
  | Region of { config : string; rom_dir : string; region : string }
  | Bytes of string

val image : session -> config:string -> rom_dir:string -> region:string -> Image.t

val rom_read :
  session -> config:string -> rom_dir:string -> region:string -> addr:int -> len:int -> Json.t

val disassemble :
  session -> source -> pc:int -> ?count:int -> ?stop:int -> unit -> Inspect.line list

val lower :
  session -> source -> pc:int -> count:int -> live_out:Ccr.t -> Report.lowered list

val inspect :
  session -> config:string -> rom_dir:string -> ?profile:profile -> ?a5:int -> ?routines:bool ->
  unit -> Json.t

val xref :
  session -> config:string -> rom_dir:string -> ?profile:profile -> ?a5:int -> ?role:string ->
  ?limit:int -> target:string -> unit -> Json.t

val flow :
  session -> config:string -> rom_dir:string -> ?profile:profile -> ?depth:int -> target:string ->
  unit -> Json.t

val coverage : session -> config:string -> rom_dir:string -> (string * int) list * string
val check_irq : session -> config:string -> rom_dir:string -> Report.irq

val decisions :
  session -> config:string -> rom_dir:string -> ?profile:profile -> ?sprite_writers:string ->
  ?sprite_header:string -> ?sprite_stats:string -> sprite_only:bool -> unit -> string * string

(** Merge profile files into [output]. Returns [{output, inputs, identities,
    executed, cold_misses}]. *)
val profile_merge : output:string -> string list -> Json.t

(** Tab-separated and indented readings of the [inspect], [xref] and [flow] documents. *)
val inspect_text : Json.t -> string
val xref_text : Json.t -> string
val flow_text : Json.t -> string

(** The static analysis tools served by [f3recomp mcp], by name. *)
val methods : string list

(** Run one named call ([rom_read], [disasm], [inspect], [xref], [flow], [coverage], [check_irq],
    [decisions], [lower], [profile_merge]) on a parameter object whose keys are the snake_case option
    names ([config], [rom_dir], [pc], [target], ...). Addresses are integers or hex strings.
    Raises {!Error.Invalid} on bad input. *)
val call : session -> string -> Json.t -> Json.t

(** The message to show for an exception raised by {!call}. *)
val describe_exception : exn -> string
