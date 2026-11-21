type t
type region = Exclusion.cpu = Main | Sound
type record_kind = Hit | Miss

val load : string -> t
val merge : t -> t -> t
val count : t -> record_kind -> region -> crc:int32 -> pc:int -> int64 option
(** Counts are unsigned 64-bit values. *)

val hot : t -> region -> Image.t -> exclusions:Exclusion.t list -> Pc.Set.t
