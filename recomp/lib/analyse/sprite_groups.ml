open Sprite_value

type lane = X | Y | XY
let lane_bits = function X -> 1 | Y -> 2 | XY -> 3
let absent = 0xffff
let shift = 8

type writer = {
  pc : Pc.t;
  cursor_reg : int;
  base : int;
  lane : lane;
  x_offset : int;
  y_offset : int;
  group_class : int;
  rigid : bool;
  root_offsets : Field.t list;
}

type unresolved = {
  pc : Pc.t;
  cursor_reg : int;
  base_reg : int;
  lane : lane;
  destination : string;
  reason : string;
}

type classification_kind = Reset | Continuation
type classification = {
  pc : Pc.t;
  base : int;
  offset : int;
  kind : classification_kind;
  instruction : string;
  evidence : string;
}

type t = {
  game_id : string;
  writers : writer list;
  resets : classification list;
  classifications : classification list;
  unresolved : unresolved list;
  stats : (string * int) list;
  reset_pcs : Pc.Set.t;
}

let writer groups pc = List.find_opt (fun (writer : writer) -> writer.pc = pc) groups.writers

let class_key (writer : writer) =
  let x, y =
    match writer.lane with
    | XY -> writer.x_offset, writer.y_offset
    | X -> writer.x_offset, writer.x_offset + 2
    | Y -> writer.y_offset - 2, writer.y_offset
  in
  writer.base, x, y

module Classes = Map.Make (struct type t = int * int * int let compare = compare end)

let assign_classes writers =
  let classes =
    writers |> List.map class_key |> List.sort_uniq compare
    |> List.mapi (fun index key -> key, index + 1) |> Classes.of_list
  in
  List.map (fun writer -> { writer with group_class = Classes.find (class_key writer) classes }) writers

let fields writers =
  List.fold_left (fun fields (writer : writer) ->
    List.fold_left (fun fields offset ->
      if offset = absent then fields else Fields.add { Field.base = writer.base; offset } fields)
      fields [writer.x_offset; writer.y_offset])
    Fields.empty writers

let make ~game_id ~instructions ~analysed ~dispatch_targets ~writers ~unresolved ~classifications =
  let resets = List.filter (fun item -> item.kind = Reset) classifications in
  let resolved = List.length writers and unproven = List.length unresolved in
  let stats =
    if instructions = 0 then ["instructions", 0; "writers", 0; "resolved", 0]
    else [
      "instructions", instructions;
      "analysed_instructions", analysed;
      "dispatch_targets", dispatch_targets;
      "x_y_writers", resolved + unproven;
      "resolved_writers", resolved;
      "unresolved_writers", unproven;
      "undeclared_x_y_writers", unproven;
      "root_fields", Fields.cardinal (fields writers);
      "group_classes", List.length (List.sort_uniq Int.compare (List.map (fun writer -> writer.group_class) writers));
      "field_writers", List.length classifications;
      "resets", List.length resets;
    ]
  in
  let reset_pcs = List.fold_left (fun pcs reset -> Pc.Set.add reset.pc pcs) Pc.Set.empty resets in
  { game_id; writers; resets; classifications; unresolved; stats; reset_pcs }

let empty =
  make ~game_id:"" ~instructions:0 ~analysed:0 ~dispatch_targets:0
    ~writers:[] ~unresolved:[] ~classifications:[]

let is_reset groups pc = Pc.Set.mem pc groups.reset_pcs

let render_hpp groups =
  let buffer = Buffer.create 1024 in
  let emit format = Printf.bprintf buffer format in
  emit "/* Generated from user-supplied ROM. Do not commit. */\n";
  emit "#pragma once\n#include <span>\n#include \"f3rt/sprite_group.hpp\"\n";
  emit "namespace f3rt::sprite_groups {\n";
  let array name typ rows row =
    if rows = [] then Printf.sprintf "std::span<const %s>{}" typ
    else (
      emit "inline constexpr %s %s[] = {\n" typ name;
      List.iter row rows;
      emit "};\n";
      Printf.sprintf "std::span<const %s>(%s)" typ name)
  in
  let writes = array "writes" "SpriteGroupWrite" groups.writers (fun (writer : writer) ->
    emit "    {0x%08xu, %du, %du, %du, %du, 0x%04xu, 0x%04xu},\n"
      writer.pc writer.base (lane_bits writer.lane) writer.group_class shift writer.x_offset writer.y_offset)
  in
  let resets = array "resets" "SpriteGroupReset" groups.resets (fun reset ->
    emit "    {0x%08xu, %du, 0u, 0x%04xu},\n" reset.pc reset.base reset.offset)
  in
  emit "inline constexpr SpriteGroupTable table{%s, %s};\n" writes resets;
  emit "} // namespace f3rt::sprite_groups\n";
  Buffer.contents buffer

let decision_rows groups =
  let writers = List.map (fun (w : writer) ->
    let roots = List.map (fun (f : Field.t) -> Printf.sprintf "%d:%d" f.base f.offset) w.root_offsets in
    let record = Printf.sprintf "%d,%d,%d,%d,%d,%d,%d,%s"
      w.cursor_reg w.base (lane_bits w.lane) w.x_offset w.y_offset w.group_class
      (Bool.to_int w.rigid) (if roots = [] then "-" else String.concat "+" roots)
    in
    w.pc, 0, "sprite-writer", record) groups.writers
  in
  let resets = List.map (fun r -> r.pc, 1, "sprite-reset", Printf.sprintf "%d,%d" r.base r.offset) groups.resets in
  List.sort compare (writers @ resets)
  |> List.map (fun (pc, _, kind, record) ->
    pc, Printf.sprintf "0x%06x\t%s\t-\t%s\t-\t-\t-\t-\t-\n" pc kind record)

let render_json groups =
  let string = Quoted.json in
  let number = string_of_int in
  let array rows render = "[" ^ String.concat ", " (List.map render rows) ^ "]" in
  let obj fields =
    "{" ^ String.concat ", " (List.map (fun (key, value) -> string key ^ ": " ^ value) fields) ^ "}" in
  let pc address = string (Printf.sprintf "0x%06x" address) in
  let reg index = string (Field.register_name index) in
  let names = function X -> ["X"] | Y -> ["Y"] | XY -> ["X"; "Y"] in
  let writes = array groups.writers (fun (w : writer) ->
    obj ["pc", pc w.pc; "cursor_register", reg w.cursor_reg; "base", number w.base;
         "base_register", reg w.base; "lane", number (lane_bits w.lane);
         "lane_names", array (names w.lane) string; "group_class", number w.group_class;
         "shift", number shift; "x_offset", number w.x_offset; "y_offset", number w.y_offset;
         "rigid", string_of_bool w.rigid;
         "root_fields", array w.root_offsets (fun f -> string (Field.describe f))]) in
  let unresolved = array groups.unresolved (fun (w : unresolved) ->
    obj ["pc", pc w.pc; "cursor_register", reg w.cursor_reg; "base_register", reg w.base_reg;
         "lane", number (lane_bits w.lane); "destination", string w.destination;
         "reason", string w.reason]) in
  let classification (item : classification) extra =
    obj (["pc", pc item.pc; "base", number item.base; "base_register", reg item.base;
          "offset", number item.offset; "instruction", string item.instruction;
          "evidence", string item.evidence] @ extra) in
  obj ["game", string groups.game_id;
       "summary", obj (List.map (fun (key, count) -> key, number count) groups.stats);
       "writes", writes; "unresolved", unresolved;
       "resets", array groups.resets (fun item -> classification item []);
       "classifications", array groups.classifications (fun item ->
         classification item ["kind", string (match item.kind with Reset -> "reset" | Continuation -> "continuation")])]
  ^ "\n"
