type t
type region = Rom | Sprites | Sprites_hi | Tiles | Tiles_hi | Sound | Samples | Eeprom

type headers = { manifest : string; game_config : string option }

val of_configs : Config.t list -> t

(** Assemble a validated region at base zero. The directory supplies the physical chip files. *)
val load_region : t -> game:string -> region -> rom_dir:string -> Image.t
val render : ?game:string -> t -> headers
