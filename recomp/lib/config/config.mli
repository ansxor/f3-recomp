type game = { id : string; pref_dir : string; platform : string option }

type chip = { size : int; crc : int32; sha1 : string }
type lane = {
  file : string;
  chip : chip;
  short_dump : chip option;
  offset : int;
  stride : int;
  group : int;
  source_offset : int;
  length : int;
  word_swap : bool;
}

type region = { size : int; fill : int; mirror_size : int; lanes : lane list }
type regions = {
  rom : region;
  sprites : region;
  sprites_hi : region;
  tiles : region;
  tiles_hi : region;
  sound : region;
  samples : region;
  eeprom : region;
}

type pointer_table = { address : int; count : int }
type jump_table = { address : Pc.t; targets : Pc.t list; pointers : pointer_table option }
type discovery = { entry_points : Pc.t list; jump_tables : jump_table list }

type exclusions = { main : Exclusion.t list; sound : Exclusion.t list }
type ram_transform = Negate_be32
type ram_code = {
  destination : int;
  source : int;
  length : int;
  transform : ram_transform;
  entry : Pc.t;
}

type register = Data of int | Address of int
type emit_unit = {
  id : int;
  name : string;
  starts : Pc.t list;
  ends : Pc.t list;
  reg : register;
  size : int;
}

type video = {
  rotation : int;
  sprite_lag : int;
  visible_y : int;
  visible_height : int;
  extend : bool;
  extended_alt_maps : bool;
  full_resolution_alt_maps : bool;
  emit_units : emit_unit list;
}

type input = { dial : bool; six_button : bool }
type sound = { bankswitch : bool }
type follow = Self | Anchor
type flicker_hold = {
  name : string;
  group_writer : Pc.t;
  anchor_writer : Pc.t;
  period : int;
  follow : follow;
}

type enhancements = { game_frame_counter : int; flicker_holds : flicker_hold list }
type cpu = {
  interrupt_observes : Ccr.t;
  interrupt_observes_explicit : bool;
  interrupt_handlers_trusted : string option;
}

type t = {
  path : string;
  game : game;
  regions : regions;
  discovery : discovery;
  exclusions : exclusions;
  ram_code : ram_code list;
  video : video;
  input : input;
  sound : sound;
  enhancements : enhancements;
  cpu : cpu;
}

val of_string : path:string -> string -> t
val load : string -> t
val load_all : string list -> t list
val require_id : string -> t -> unit
val check_id : path:string -> string -> unit
