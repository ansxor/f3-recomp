type flicker_hold = {
  name : string;
  group_class : int;
  anchor_class : int;
  period : int;
  follow : Config.follow;
}

type t = { game_frame_counter : int; flicker_holds : flicker_hold list }

let default (_ : Config.t) = { game_frame_counter = 0; flicker_holds = [] }

let of_config (config : Config.t) groups =
  let resolve (hold : Config.flicker_hold) =
    let writer pc =
      match Sprite_groups.writer groups pc with
      | Some writer -> writer
      | None -> Error.fail ~where:config.path "%s: no sprite-group write at 0x%x" hold.name pc
    in
    let group = writer hold.group_writer in
    let anchor = writer hold.anchor_writer in
    if group.base <> anchor.base then
      Error.fail ~where:config.path "%s: sprite-group writer base registers differ" hold.name;
    { name = hold.name; group_class = group.group_class; anchor_class = anchor.group_class;
      period = hold.period; follow = hold.follow }
  in
  { game_frame_counter = config.enhancements.game_frame_counter;
    flicker_holds = List.map resolve config.enhancements.flicker_holds }

(* Match the reference's json.dumps string literal, including ASCII-only output. *)
let quote_name name =
  let buffer = Buffer.create (String.length name + 2) in
  Buffer.add_char buffer '"';
  let rec add offset =
    if offset < String.length name then (
      let decoded = String.get_utf_8_uchar name offset in
      let codepoint = Uchar.to_int (Uchar.utf_decode_uchar decoded) in
      (match codepoint with
      | 0x22 -> Buffer.add_string buffer {|\"|}
      | 0x5c -> Buffer.add_string buffer {|\\|}
      | 0x08 -> Buffer.add_string buffer {|\b|}
      | 0x0c -> Buffer.add_string buffer {|\f|}
      | 0x0a -> Buffer.add_string buffer {|\n|}
      | 0x0d -> Buffer.add_string buffer {|\r|}
      | 0x09 -> Buffer.add_string buffer {|\t|}
      | n when n >= 0x20 && n < 0x7f -> Buffer.add_char buffer (Char.chr n)
      | n when n <= 0xffff -> Printf.bprintf buffer "\\u%04x" n
      | n ->
          let scalar = n - 0x10000 in
          Printf.bprintf buffer "\\u%04x\\u%04x" (0xd800 + (scalar lsr 10))
            (0xdc00 + (scalar land 0x3ff)));
      add (offset + Uchar.utf_decode_length decoded))
  in
  add 0;
  Buffer.add_char buffer '"';
  Buffer.contents buffer

let render spec =
  let buffer = Buffer.create 512 in
  Buffer.add_string buffer {|#pragma once
#include <array>
#include <cstdint>

namespace f3rt::game_config {
struct FlickerHold {
    const char *name;
    uint8_t group_class, anchor_class, period;
    enum class Follow : uint8_t { Self, Anchor } follow;
};
|};
  Printf.bprintf buffer "inline constexpr uint32_t game_frame_counter = 0x%x;\n"
    spec.game_frame_counter;
  (match spec.flicker_holds with
  | [] -> Buffer.add_string buffer "inline constexpr std::array<FlickerHold, 0> flicker_holds{};\n"
  | holds ->
      Buffer.add_string buffer "inline constexpr FlickerHold flicker_holds[] = {\n";
      List.iter (fun hold ->
        let follow = match hold.follow with Config.Self -> "Self" | Anchor -> "Anchor" in
        Printf.bprintf buffer "    {%s, %d, %d, %d, FlickerHold::Follow::%s},\n"
          (quote_name hold.name) hold.group_class hold.anchor_class hold.period follow) holds;
      Buffer.add_string buffer "};\n");
  Buffer.add_string buffer "}\n";
  ["enhancements.hpp", Buffer.contents buffer]
