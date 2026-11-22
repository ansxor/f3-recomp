type t =
  | Null
  | Bool of bool
  | Int of int
  | Int64 of int64
  | Float of float
  | String of string
  | Array of t list
  | Object of (string * t) list

(** Compact JSON. Non-finite floats become [null]. Strings are escaped byte for
    byte, so valid UTF-8 stays valid. *)
val to_string : t -> string

(** Parse one complete JSON document. Integers that fit become [Int], then
    [Int64]; everything else numeric becomes [Float]. *)
val parse : string -> (t, string) result

val member : string -> t -> t option
(** Field of an [Object], [None] for missing fields and non-objects. *)

val hex : int -> t
(** The ["0x%06x"] spelling used for address-like strings. *)
