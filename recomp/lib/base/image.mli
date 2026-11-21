type t = { base : int; bytes : string }

val length : t -> int
val contains : t -> int -> bool
val contains_range : t -> int -> length:int -> bool
val u8 : t -> int -> int
val u16 : t -> int -> int
val u32 : t -> int -> int
val s8 : t -> int -> int
val s16 : t -> int -> int
val s32 : t -> int -> int
val crc32 : t -> int32
val slice : t -> int -> length:int -> t
