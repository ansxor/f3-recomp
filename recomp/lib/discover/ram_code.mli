type routine = private {
  destination : Pc.t;
  payload : Image.t;
  entry : Pc.t;
  decoded : Decode.decoded Pc.Map.t;
}

(** Validate the specs against the ROM image and decode every even PC in each
    transformed payload, including illegal and truncated instructions.
    Routines are sorted by destination. *)
val decode : Config.ram_code list -> Image.t -> routine list

(** Direct targets outside the payload of the instruction that names them. *)
val exits : routine list -> Pc.Set.t

(** Check the ranges, transformed payload bytes, and aligned PC coverage. *)
val check : Config.ram_code list -> Image.t -> routine list -> unit
