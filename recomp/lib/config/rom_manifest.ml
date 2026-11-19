type t = Config.t list
type region = Rom | Sprites | Sprites_hi | Tiles | Tiles_hi | Sound | Samples | Eeprom
type headers = { manifest : string; game_config : string option }

let of_configs configs =
  let ids = Hashtbl.create (List.length configs) in
  List.iter (fun (config : Config.t) ->
      let id = config.game.id in
      if Hashtbl.mem ids id then Error.fail ~where:config.path "Duplicate game id: %s" id;
      Hashtbl.add ids id ()) configs;
  configs

let find_game manifest game =
  match List.find_opt (fun (config : Config.t) -> config.game.id = game) manifest with
  | Some config -> config
  | None -> Error.fail "Unknown game: %s" game

let region_spec (regions : Config.regions) = function
  | Rom -> "rom", regions.rom
  | Sprites -> "sprites", regions.sprites
  | Sprites_hi -> "sprites_hi", regions.sprites_hi
  | Tiles -> "tiles", regions.tiles
  | Tiles_hi -> "tiles_hi", regions.tiles_hi
  | Sound -> "sound", regions.sound
  | Samples -> "samples", regions.samples
  | Eeprom -> "eeprom", regions.eeprom

module Chip = struct
  let verify (chip : Config.chip) (image : Image.t) ~path =
    if Image.length image <> chip.size then
      Error.fail ~where:path "size mismatch: expected %d bytes, got %d"
        chip.size (Image.length image);
    if Image.crc32 image <> chip.crc then Error.fail ~where:path "CRC32 mismatch";
    if Sha1.hex_digest image.bytes <> chip.sha1 then Error.fail ~where:path "SHA1 mismatch"

  let read (lane : Config.lane) ~rom_dir =
    let path =
      if Filename.is_relative lane.file then Filename.concat rom_dir lane.file else lane.file
    in
    let bytes = In_channel.with_open_bin path In_channel.input_all in
    let bytes =
      match lane.short_dump with
      | Some short when String.length bytes = short.size ->
          verify short { base = 0; bytes } ~path;
          bytes ^ String.make (lane.chip.size - short.size) '\xff'
      | _ -> bytes
    in
    verify lane.chip { base = 0; bytes } ~path;
    bytes
end

module Lane = struct
  let place (lane : Config.lane) ~chip ~image =
    if lane.word_swap then
      for byte = 0 to lane.length - 1 do
        Bytes.set image (lane.offset + byte) chip.[lane.source_offset + (byte lxor 1)]
      done
    else if lane.stride = lane.group then
      Bytes.blit_string chip lane.source_offset image lane.offset lane.length
    else
      for group = 0 to lane.length / lane.group - 1 do
        Bytes.blit_string chip (lane.source_offset + group * lane.group)
          image (lane.offset + group * lane.stride) lane.group
      done
end

module Region = struct
  let load (region : Config.region) ~rom_dir =
    let image = Bytes.make region.mirror_size (Char.chr region.fill) in
    let remaining = Hashtbl.create (List.length region.lanes) in
    let chips = Hashtbl.create (List.length region.lanes) in
    List.iter (fun (lane : Config.lane) ->
        let count = Option.value (Hashtbl.find_opt remaining lane.file) ~default:0 in
        Hashtbl.replace remaining lane.file (count + 1)) region.lanes;
    List.iter (fun (lane : Config.lane) ->
        let chip =
          match Hashtbl.find_opt chips lane.file with
          | Some chip -> chip
          | None -> Chip.read lane ~rom_dir
        in
        Lane.place lane ~chip ~image;
        let count = Hashtbl.find remaining lane.file - 1 in
        if count = 0 then Hashtbl.remove chips lane.file
        else Hashtbl.replace chips lane.file chip;
        Hashtbl.replace remaining lane.file count) region.lanes;
    if region.size > 0 then
      for copy = 1 to region.mirror_size / region.size - 1 do
        Bytes.blit image 0 image (copy * region.size) region.size
      done;
    { Image.base = 0; bytes = Bytes.unsafe_to_string image }
end

let load_region manifest ~game region ~rom_dir =
  let config = find_game manifest game in
  let _, spec = region_spec config.regions region in
  Region.load spec ~rom_dir

(* Match the ASCII JSON string literals used by the Python generator. *)
let quoted text =
  let buffer = Buffer.create (String.length text + 2) in
  Buffer.add_char buffer '"';
  let rec escape index =
    if index < String.length text then (
      let decoded = String.get_utf_8_uchar text index in
      let code = Uchar.to_int (Uchar.utf_decode_uchar decoded) in
      (match code with
       | 0x22 -> Buffer.add_string buffer "\\\""
       | 0x5c -> Buffer.add_string buffer "\\\\"
       | 8 -> Buffer.add_string buffer "\\b"
       | 9 -> Buffer.add_string buffer "\\t"
       | 10 -> Buffer.add_string buffer "\\n"
       | 12 -> Buffer.add_string buffer "\\f"
       | 13 -> Buffer.add_string buffer "\\r"
       | code when code >= 32 && code < 127 -> Buffer.add_char buffer (Char.chr code)
       | code when code <= 0xffff -> Printf.bprintf buffer "\\u%04x" code
       | code ->
           let pair = code - 0x10000 in
           Printf.bprintf buffer "\\u%04x\\u%04x"
             (0xd800 + (pair lsr 10)) (0xdc00 + (pair land 0x3ff)));
      escape (index + Uchar.utf_decode_length decoded))
  in
  escape 0;
  Buffer.add_char buffer '"';
  Buffer.contents buffer

let video_settings (video : Config.video) =
  Printf.sprintf "{ %d, %d, %d, %d, %b, %b, %b }"
    video.rotation video.sprite_lag video.visible_y video.visible_height video.extend
    video.extended_alt_maps video.full_resolution_alt_maps

let input_settings (input : Config.input) =
  Printf.sprintf "{ %b, %b }" input.dial input.six_button

let render_game (config : Config.t) =
  Printf.sprintf {|// Generated by recomp/compile/roms.py from games/%s/config.toml. No ROM data.
#pragma once
#include <string_view>
#include "f3rt/rom.hpp"

namespace f3rt::game_config {
inline constexpr std::string_view id = %s;
inline constexpr std::string_view pref_dir = %s;
inline constexpr InputConfig input%s;
inline constexpr VideoConfig video%s;
inline constexpr unsigned visible_end = video.visible_y + video.visible_height;
static_assert(video.visible_y + video.visible_height <= 256 && video.visible_height > 0,
              "visible window must fit the 256-line frame");
static_assert(!video.full_resolution_alt_maps || video.extended_alt_maps,
              "full_resolution_alt_maps requires extended_alt_maps");
// Startup consistency check: a loaded set must carry the geometry this build was compiled for.
constexpr bool matches(const VideoConfig &v) {
    return v.rotation == video.rotation && v.sprite_lag == video.sprite_lag &&
           v.visible_y == video.visible_y && v.visible_height == video.visible_height &&
           v.extend == video.extend && v.extended_alt_maps == video.extended_alt_maps &&
           v.full_resolution_alt_maps == video.full_resolution_alt_maps;
}
} // namespace f3rt::game_config
|} config.game.id (quoted config.game.id) (quoted config.game.pref_dir)
    (input_settings config.input) (video_settings config.video)

let render_lane buffer (lane : Config.lane) =
  let short_size, short_crc, short_sha1 =
    match lane.short_dump with
    | None -> 0, "0", ""
    | Some chip -> chip.size, Printf.sprintf "%08lx" chip.crc, chip.sha1
  in
  Printf.bprintf buffer "    { %s, %d, 0x%08lxu, %s, %d, 0x%su, %s, %d, %d, %d, %d, %d, %b },\n"
    (quoted lane.file) lane.chip.size lane.chip.crc (quoted lane.chip.sha1)
    short_size short_crc (quoted short_sha1)
    lane.offset lane.stride lane.group lane.source_offset lane.length lane.word_swap

let render_manifest manifest =
  let buffer = Buffer.create 4096 in
  Buffer.add_string buffer {|// Generated by recomp/compile/roms.py. No ROM data.
#pragma once
#include "f3rt/rom.hpp"
#include <array>
#include <span>
#include <string_view>
namespace f3rt::rom_manifest {
struct Chip { const char *file; size_t size; uint32_t crc; const char *sha1; size_t short_size; uint32_t short_crc; const char *short_sha1; size_t offset, stride, group, source_offset, length; bool word_swap; };
struct Region { size_t size, mapped_size; uint8_t fill; std::span<const Chip> chips; };
struct Game { std::string_view id; std::string_view pref_dir; VideoConfig video; InputConfig input; bool sound_bankswitch; std::array<Region, 8> regions; };
|};
  let regions = [Rom; Sprites; Sprites_hi; Tiles; Tiles_hi; Sound; Samples; Eeprom] in
  let games =
    List.mapi (fun index (config : Config.t) ->
        let tables = List.map (fun region ->
            let name, spec = region_spec config.regions region in
            let table = Printf.sprintf "chips_%d_%s" index name in
            Printf.bprintf buffer "inline constexpr std::array<Chip, %d> %s = {{\n"
              (List.length spec.lanes) table;
            List.iter (render_lane buffer) spec.lanes;
            Buffer.add_string buffer "}};\n";
            Printf.sprintf "{ %d, %d, %d, %s }" spec.size spec.mirror_size spec.fill table)
          regions
        in
        Printf.sprintf "    { %s, %s, %s, %s, %b, {{ %s }} },\n"
          (quoted config.game.id) (quoted config.game.pref_dir)
          (video_settings config.video) (input_settings config.input)
          config.sound.bankswitch (String.concat ", " tables))
      manifest
  in
  Printf.bprintf buffer "inline constexpr std::array<Game, %d> games = {{\n"
    (List.length games);
  List.iter (Buffer.add_string buffer) games;
  Buffer.add_string buffer "}};\n} // namespace f3rt::rom_manifest\n";
  Buffer.contents buffer

let render ?game manifest =
  let game_config = Option.map (fun game -> find_game manifest game |> render_game) game in
  { manifest = render_manifest manifest; game_config }
