type t
type region = Exclusion.cpu = Main | Sound
type record_kind = Hit | Miss

val load : string -> t

(** Write the text format that [load] reads. ROM identities sort by region then
    unsigned CRC, then hits and misses by identity and address. *)
val write : string -> t -> unit

type stats = { identities : int; executed : int; cold_misses : int }

(** Number of ROM identities, distinct executed addresses and distinct miss addresses. *)
val stats : t -> stats
val merge : t -> t -> t
val count : t -> record_kind -> region -> crc:int32 -> pc:int -> int64 option
(** Counts are unsigned 64-bit values. *)

val hot : t -> region -> Image.t -> exclusions:Exclusion.t list -> Pc.Set.t
