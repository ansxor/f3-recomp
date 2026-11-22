(** Structured views of a decoded game for the JSON API: ROM bytes, listings,
    discovered routines, cross-references and call flow.

    Addresses are JSON integers. Every [Json.t] returned here has a fixed
    schema, documented in recomp/JSON-API.md. *)

(** {1 Addresses} *)

(** Hex with an optional [0x] or [$] prefix. *)
val parse_address : string -> int

(** The main CPU's fixed address map as named half-open ranges. *)
val regions : (string * int * int) list

(** {1 Raw reads and listings} *)

type region = Rom_manifest.region

val region_of_name : string -> region
val region_name : region -> string

val read_bytes : Image.t -> addr:int -> len:int -> string
val hex_of_bytes : string -> string
val bytes_of_hex : string -> string

(** [{region, addr, length, size, bytes}] with [bytes] as lowercase hex. *)
val rom_read : region:string -> Image.t -> addr:int -> len:int -> Json.t

type line = { pc : Pc.t; length : int; bytes : string; decoded : Decode.decoded }

(** Decode consecutive instructions from [pc]. Stops after [count] lines or
    when [pc] reaches [stop], and at the end of the image. *)
val disassemble : Image.t -> pc:Pc.t -> ?count:int -> ?stop:Pc.t -> unit -> line list

val line_to_text : line -> string
val line_to_json : ?a5:int -> line -> Json.t
val disasm_json : ?a5:int -> line list -> Json.t

(** {1 Whole-game analysis} *)

type t

(** Run discovery, find routines and, when given, attach profile hit counts.
    [a5] overrides the inferred small-data base register. *)
val make : ?profile:Profile.t -> ?a5:int -> Game_image.t -> t

val make_of_program : ?profile:Profile.t -> ?a5:int -> Program.t -> t

(** Game identity, vectors, routines, RAM code and unresolved computed sites.
    With [routines:false] the routine list is replaced by the counts. *)
val inspect_json : ?routines:bool -> t -> Json.t

(** Raise [Error.Invalid] for unknown targets. [role] is one of
    [all|read|write|rmw|call|branch|lea]; [write] also matches [rmw]. *)
val xref_json : ?role:string -> ?limit:int -> t -> target:string -> Json.t

val flow_json : ?depth:int -> t -> target:string -> Json.t
