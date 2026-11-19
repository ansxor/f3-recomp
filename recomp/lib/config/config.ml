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

module Table = struct
  type t = { where : string; fields : (string * Otoml.t) list }

  let create where = function
    | Otoml.TomlTable fields | TomlInlineTable fields -> { where; fields }
    | _ -> Error.fail ~where "expected a table"

  let reject_unknown table allowed =
    let unknown =
      List.filter_map
        (fun (key, _) -> if List.mem key allowed then None else Some key)
        table.fields
    in
    if unknown <> [] then
      Error.fail ~where:table.where "unknown keys: %s" (String.concat ", " unknown)

  let optional table key decode =
    List.assoc_opt key table.fields |> Option.map (decode (table.where ^ "." ^ key))

  let required table key decode =
    match optional table key decode with
    | Some value -> value
    | None -> Error.fail ~where:table.where "missing '%s'" key

  let default table key decode fallback =
    Option.value (optional table key decode) ~default:fallback

  let section table key =
    default table key create { where = table.where ^ "." ^ key; fields = [] }

  let array where = function
    | Otoml.TomlArray values | TomlTableArray values -> values
    | _ -> Error.fail ~where "expected an array"

  let list decode where value =
    array where value
    |> List.mapi (fun index value -> decode (Printf.sprintf "%s[%d]" where index) value)

  let string where = function
    | Otoml.TomlString value -> value
    | _ -> Error.fail ~where "expected a string"

  let int where = function
    | Otoml.TomlInteger value -> value
    | _ -> Error.fail ~where "expected an integer"

  let bool where = function
    | Otoml.TomlBoolean value -> value
    | _ -> Error.fail ~where "expected a boolean"

  let address where = function
    | Otoml.TomlString value ->
        let text = String.trim value in
        let number =
          if String.starts_with ~prefix:"0x" (String.lowercase_ascii text) then
            int_of_string_opt text
          else
            let digits = String.to_seq text |> Seq.drop
              (if String.starts_with ~prefix:"+" text || String.starts_with ~prefix:"-" text
               then 1 else 0)
            in
            if Seq.for_all (function '0' .. '9' | '_' -> true | _ -> false) digits then
              int_of_string_opt text
            else None
        in
        (match number with
         | Some address -> address
         | None -> Error.fail ~where "expected an integer address")
    | value -> int where value
end

let is_hex = function '0' .. '9' | 'a' .. 'f' | 'A' .. 'F' -> true | _ -> false

let hash digits where value =
  let value = Table.string where value in
  if String.length value <> digits || not (String.for_all is_hex value) then
    Error.fail ~where "expected %d hexadecimal digits" digits;
  String.lowercase_ascii value

let chip table prefix size =
  let crc = Table.required table (prefix ^ "crc") (hash 8) in
  let sha1 = Table.required table (prefix ^ "sha1") (hash 40) in
  { size; crc = Int32.of_string ("0x" ^ crc); sha1 }

let read_lane ~main ~size ~interleave where value =
  let table = Table.create where value in
  if main then Table.reject_unknown table ["file"; "size"; "offset"; "crc"; "sha1"];
  let file = Table.required table "file" Table.string in
  if file = "" then Error.fail ~where "missing 'file' name";
  let physical_size = Table.required table "size" Table.int in
  let source_offset = Table.default table "source_offset" Table.int 0 in
  if physical_size <= 0 || source_offset < 0 || source_offset >= physical_size then
    Error.fail ~where "invalid lane geometry";
  let available = physical_size - source_offset in
  let length = Table.default table "length" Table.int available in
  let offset = Table.default table "offset" Table.int 0 in
  let stride = Table.default table "stride" Table.int interleave in
  let group = Table.default table "group" Table.int 1 in
  let word_swap = Table.default table "word_swap" Table.bool false in
  if length <= 0 || length > available || group <= 0 || stride < group
     || offset < 0 || length mod group <> 0 then
    Error.fail ~where "invalid lane geometry";
  if main && offset >= interleave then Error.fail ~where "invalid byte offset %d" offset;
  if offset > size || group > size - offset
     || length / group - 1 > (size - offset - group) / stride then
    Error.fail ~where "lane exceeds region";
  if main && length <> size / interleave then
    Error.fail ~where "lane does not fill the configured image";
  if word_swap && (group <> 1 || stride <> 1 || length mod 2 <> 0) then
    Error.fail ~where "word_swap requires group=1, stride=1 and even length";
  let chip_spec = chip table "" physical_size in
  let short_dump =
    Table.optional table "short_size" Table.int
    |> Option.map (fun short_size ->
           if short_size <= 0 || short_size >= physical_size then
             Error.fail ~where "invalid short dump size";
           chip table "short_" short_size)
  in
  { file; chip = chip_spec; short_dump; offset; stride; group; source_offset; length; word_swap }

let read_region root name =
  let main = name = "rom" in
  let absent = { size = 0; fill = 0; mirror_size = 0; lanes = [] } in
  let read table =
    let where = table.Table.where in
    if main then Table.reject_unknown table ["size"; "interleave"; "lanes"];
    let size = Table.required table "size" Table.int in
    let fill = Table.default table "fill" Table.int 0 in
    if size < 0 || (fill <> 0 && fill <> 255) then
      Error.fail ~where "invalid region size/fill";
    let interleave = Table.default table "interleave" Table.int (if main then 4 else 1) in
    if main && (size = 0 || interleave <= 0 || size mod interleave <> 0) then
      Error.fail ~where "ROM size must be a positive multiple of its interleave";
    let mirror_size = Table.default table "mirror_size" Table.int size in
    if name = "eeprom" && ((size <> 0 && size <> 128) || mirror_size <> size) then
      Error.fail ~where "EEPROM region must be absent or exactly 128 bytes";
    if mirror_size < size || (if size = 0 then mirror_size <> 0 else mirror_size mod size <> 0) then
      Error.fail ~where "invalid mirrored size";
    let lanes = Table.default table "lanes" (Table.list (read_lane ~main ~size ~interleave)) [] in
    if size > 0 && lanes = [] then Error.fail ~where "missing ROM lanes";
    let identities = Hashtbl.create (List.length lanes) in
    let offsets = Hashtbl.create (List.length lanes) in
    List.iter
      (fun lane ->
        let identity = lane.chip, lane.short_dump in
        (match Hashtbl.find_opt identities lane.file with
         | Some previous when previous <> identity ->
             Error.fail ~where "inconsistent physical chip identity: %s" lane.file
         | _ -> Hashtbl.replace identities lane.file identity);
        if main then (
          if Hashtbl.mem offsets lane.offset then
            Error.fail ~where "duplicate lane offset %d" lane.offset;
          Hashtbl.add offsets lane.offset ()))
      lanes;
    if main && Hashtbl.length offsets <> interleave then
      Error.fail ~where "ROM lanes must cover every byte of the image";
    { size; fill; mirror_size; lanes }
  in
  let decode where value = read (Table.create where value) in
  if name = "eeprom" then Table.default root name decode absent
  else Table.required root name decode

let read_regions root =
  let rom = read_region root "rom" in
  { rom;
    sprites = read_region root "sprites";
    sprites_hi = read_region root "sprites_hi";
    tiles = read_region root "tiles";
    tiles_hi = read_region root "tiles_hi";
    sound = read_region root "sound";
    samples = read_region root "samples";
    eeprom = read_region root "eeprom" }

let read_exclusions root ~rom_size =
  let decode where value =
    let table = Table.create where value in
    let cpu =
      match Table.default table "cpu" Table.string "main" with
      | "main" -> Exclusion.Main
      | "sound" -> Exclusion.Sound
      | _ -> Error.fail ~where "invalid exclusion CPU"
    in
    let fill =
      Table.optional table "fill" (fun where -> function
        | (Otoml.TomlInteger _ | TomlString _) as value -> Table.address where value
        | _ -> Error.fail ~where "fill must be a byte value")
    in
    cpu,
    { Exclusion.start = Table.required table "start" Table.address;
      stop = Table.required table "end" Table.address;
      reason = Table.required table "reason" Table.string;
      evidence = Table.required table "evidence" Table.string;
      fill }
  in
  let records = Table.default root "exclude" (Table.list decode) [] in
  let for_cpu cpu where base size =
    records
    |> List.filter_map (fun (target, exclusion) -> if target = cpu then Some exclusion else None)
    |> Exclusion.validate ~where:(root.Table.where ^ where) ~base ~size
  in
  { main = for_cpu Main ".exclude (main)" 0 rom_size;
    sound = for_cpu Sound ".exclude (sound)" 0xc00000 0x80000 }

let aligned decode where value =
  let pc = decode where value in
  if pc mod 2 <> 0 then Error.fail ~where "address %#x must be even" pc;
  pc

let read_discovery root ~rom_size exclusions =
  let table = Table.section root "discovery" in
  let where = table.where in
  (match Table.default table "coverage" Table.string "all_aligned" with
   | "all_aligned" -> ()
   | "recursive" -> Error.fail ~where "recursive discovery is unsupported"
   | coverage -> Error.fail ~where "unknown discovery coverage mode %S" coverage);
  let included where pc =
    match Exclusion.find exclusions pc with
    | None -> ()
    | Some { start; stop; reason; evidence; _ } ->
        Error.fail ~where "excluded PC %#x in [%#x, %#x): %s; %s"
          pc start stop reason evidence
  in
  let addresses = Table.list (aligned Table.address) in
  let in_rom pc = 0 <= pc && pc < rom_size in
  let entry_points =
    Table.default table "entry_points" addresses [] |> List.filter in_rom
  in
  List.iter (included (where ^ ".entry_points")) entry_points;
  let read_jump_table where value =
    let table = Table.create where value in
    let address = Table.required table "address" (aligned Table.address) in
    let targets = Table.default table "targets" addresses [] |> List.filter in_rom in
    let pointers =
      Table.optional table "table" Table.address
      |> Option.map (fun address ->
             let count = Table.required table "count" Table.address in
             if address < 0 || address > rom_size || count < 0 || count > (rom_size - address) / 4 then
               Error.fail ~where "jump table outside ROM";
             { address; count })
    in
    included where address;
    List.iter (included where) targets;
    { address; targets; pointers }
  in
  let jump_tables = Table.default table "jump_tables" (Table.list read_jump_table) [] in
  { entry_points; jump_tables }

let read_ram_code root ~rom_size =
  let read where value =
    let table = Table.create where value in
    Table.reject_unknown table ["destination"; "source"; "length"; "transform"; "entry"];
    let destination = Table.required table "destination" Table.int in
    let source = Table.required table "source" Table.int in
    let length = Table.required table "length" Table.int in
    let entry = Table.required table "entry" Table.int in
    let fits_window base stop = destination >= base && destination <= stop - length in
    if length <= 0 || length mod 4 <> 0 || source < 0 || source mod 2 <> 0
       || source > rom_size - length || destination mod 2 <> 0 || entry mod 2 <> 0
       || not (fits_window 0x400000 0x440000 || fits_window 0x600000 0x610000)
       || entry < destination || entry >= destination + length then
      Error.fail ~where "invalid RAM code range at %#x or ROM source at %#x" destination source;
    let transform = match Table.required table "transform" Table.string with
      | "negate_be32" -> Negate_be32
      | transform -> Error.fail ~where "unknown RAM code transform %S" transform
    in
    { destination; source; length; transform; entry }
  in
  let specs =
    Table.default root "ram_code" (Table.list read) []
    |> List.sort (fun a b -> Int.compare a.destination b.destination)
  in
  let rec check_neighbors = function
    | first :: (second :: _ as rest) ->
        if first.destination + first.length > second.destination then
          Error.fail ~where:root.where "overlapping RAM code at %#x" second.destination;
        check_neighbors rest
    | _ -> ()
  in
  check_neighbors specs;
  specs

let reserved_unit_names =
  "all digest detail alignas alignof asm auto bool break case catch char class concept const
   constexpr continue default delete do double else enum explicit export extern false float
   for friend goto if inline int long namespace new noexcept nullptr operator private
   protected public register requires return short signed sizeof static struct switch template
   this throw true try typedef typename union unsigned using virtual void volatile while"
  |> String.map (function '\n' -> ' ' | c -> c)
  |> String.split_on_char ' '
  |> List.filter (fun name -> name <> "")

let identifier name =
  let initial = function 'a' .. 'z' | 'A' .. 'Z' | '_' -> true | _ -> false in
  let character = function '0' .. '9' -> true | c -> initial c in
  String.length name > 0 && initial name.[0] && String.for_all character name

let unit_pcs where value =
  let pc where value =
    let address = aligned Table.int where value in
    if address < 0 || address >= 0x1000000 then
      Error.fail ~where "PC %#x outside the 24-bit address space" address;
    address
  in
  match value with
  | Otoml.TomlArray [] -> Error.fail ~where "PC list must not be empty"
  | Otoml.TomlArray _ -> Table.list pc where value
  | value -> [pc where value]

let unit_register where value =
  match Table.string where value with
  | "d0" | "d1" | "d2" | "d3" | "d4" | "d5" | "d6" | "d7" as reg ->
      Data (Char.code reg.[1] - Char.code '0')
  | "a0" | "a1" | "a2" | "a3" | "a4" | "a5" | "a6" as reg ->
      Address (Char.code reg.[1] - Char.code '0')
  | reg -> Error.fail ~where "unit register %S must be one of d0-d7/a0-a6" reg

let read_emit_units video =
  let names = Hashtbl.create 8 in
  let read id where value =
    let table = Table.create where value in
    Table.reject_unknown table ["name"; "start"; "end"; "unit"; "size"];
    let name = Table.required table "name" Table.string in
    if not (identifier name) || List.mem name reserved_unit_names then
      Error.fail ~where "invalid unit name %S" name;
    if Hashtbl.mem names name then Error.fail ~where "duplicate emit unit name %S" name;
    Hashtbl.add names name ();
    let starts = Table.required table "start" unit_pcs in
    let ends = Table.required table "end" unit_pcs in
    let start_count = List.length starts and end_count = List.length ends in
    if start_count <> end_count then
      Error.fail ~where "start has %d entries, end has %d" start_count end_count;
    if Pc.Set.cardinal (Pc.Set.of_list starts) <> start_count
       || Pc.Set.cardinal (Pc.Set.of_list ends) <> end_count then
      Error.fail ~where "duplicate start or end PC";
    List.iter2
      (fun start stop ->
        if start >= stop then Error.fail ~where "start %#x must be below end %#x" start stop)
      starts ends;
    let reg = Table.required table "unit" unit_register in
    let size = Table.default table "size" Table.int 0 in
    if size < 0 then Error.fail ~where "size must be a non-negative integer";
    { id; name; starts; ends; reg; size }
  in
  Table.default video "emit_units"
    (fun where value ->
      Table.array where value
      |> List.mapi (fun id value -> read id (Printf.sprintf "%s[%d]" where id) value))
    []

let read_video root =
  let table = Table.section root "video" in
  let where = table.where in
  let rotation = Table.default table "rotation" Table.int 0 in
  let sprite_lag = Table.default table "sprite_lag" Table.int 1 in
  let visible_y = Table.default table "visible_y" Table.int 24 in
  let visible_height = Table.default table "visible_height" Table.int 232 in
  let extend = Table.default table "extend" Table.bool true in
  let extended_alt_maps = Table.default table "extended_alt_maps" Table.bool false in
  let full_resolution_alt_maps = Table.default table "full_resolution_alt_maps" Table.bool false in
  if not (List.mem rotation [0; 90; 180; 270]) || sprite_lag < 0 || sprite_lag > 2
     || visible_y < 0 || visible_y >= 256 || visible_height <= 0
     || visible_height > 256 - visible_y then
    Error.fail ~where "unsupported video configuration";
  if full_resolution_alt_maps && not extended_alt_maps then
    Error.fail ~where "full_resolution_alt_maps requires extended_alt_maps";
  { rotation; sprite_lag; visible_y; visible_height; extend; extended_alt_maps;
    full_resolution_alt_maps; emit_units = read_emit_units table }

let read_enhancements root =
  let table = Table.section root "enhancements" in
  let names = Hashtbl.create 8 in
  let hold where value =
    let table = Table.create where value in
    let name = Table.required table "name" Table.string in
    if name = "" || String.contains name '=' || Hashtbl.mem names name then
      Error.fail ~where "invalid or repeated flicker hold name %S" name;
    Hashtbl.add names name ();
    let group_writer = Table.required table "group_writer" Table.int in
    let anchor_writer = Table.required table "anchor_writer" Table.int in
    let period = Table.required table "period" Table.int in
    if period < 2 || period > 255 then Error.fail ~where "period must be in 2..255";
    let follow = match Table.required table "follow" Table.string with
      | "self" -> Self
      | "anchor" -> Anchor
      | _ -> Error.fail ~where "follow must be self or anchor"
    in
    { name; group_writer; anchor_writer; period; follow }
  in
  let flicker_holds = Table.default table "flicker_hold" (Table.list hold) [] in
  let game_frame_counter =
    match flicker_holds with
    | [] -> 0
    | _ ->
        Table.default table "game_frame_counter"
          (fun where -> function
            | Otoml.TomlInteger address -> address
            | _ -> Error.fail ~where "flicker_hold requires an aligned game_frame_counter RAM address")
          0
  in
  if flicker_holds <> [] &&
     (game_frame_counter < 0x400000 || game_frame_counter >= 0x420000 || game_frame_counter mod 2 <> 0) then
    Error.fail ~where:table.where "flicker_hold requires an aligned game_frame_counter RAM address";
  { game_frame_counter; flicker_holds }

let read_cpu root =
  let table = Table.section root "cpu" in
  Table.reject_unknown table ["interrupt_observes"; "interrupt_handlers_trusted"];
  let flag where value =
    let name = Table.string where value in
    let flag = match String.uppercase_ascii name with
      | "X" -> Ccr.X | "N" -> N | "Z" -> Z | "V" -> V | "C" -> C
      | _ -> Error.fail ~where "unknown CCR flag %S" name
    in
    Ccr.singleton flag
  in
  let interrupt_observes =
    Table.optional table "interrupt_observes" (Table.list flag)
  in
  let interrupt_handlers_trusted =
    Table.optional table "interrupt_handlers_trusted" (fun where value ->
      let evidence = String.trim (Table.string where value) in
      if evidence = "" then Error.fail ~where "expected non-empty evidence";
      evidence)
  in
  { interrupt_observes = List.fold_left Ccr.union Ccr.none (Option.value ~default:[] interrupt_observes);
    interrupt_observes_explicit = Option.is_some interrupt_observes;
    interrupt_handlers_trusted }

let read_id table = Table.required table "id" Table.string

let decode ~path value =
  let root = Table.create path value in
  let game =
    let table = Table.section root "game" in
    let id = read_id table in
    let pref_dir = Table.default table "pref_dir" Table.string id in
    let platform = Table.optional table "platform" Table.string in
    { id; pref_dir; platform }
  in
  let regions = read_regions root in
  let rom_size = regions.rom.size in
  let exclusions = read_exclusions root ~rom_size in
  let discovery = read_discovery root ~rom_size exclusions.main in
  let ram_code = read_ram_code root ~rom_size in
  let video = read_video root in
  let input =
    let table = Table.section root "input" in
    { dial = Table.default table "dial" Table.bool false;
      six_button = Table.default table "six_button" Table.bool false }
  in
  let sound =
    let table = Table.section root "sound" in
    { bankswitch = Table.default table "bankswitch" Table.bool false }
  in
  let enhancements = read_enhancements root in
  let cpu = read_cpu root in
  { path; game; regions; discovery; exclusions; ram_code; video; input; sound; enhancements; cpu }

let parsed ~path = function
  | Ok value -> decode ~path value
  | Error error -> Error.fail ~where:path "%s" error

let of_string ~path source = Otoml.Parser.from_string_result source |> parsed ~path
let load path = Otoml.Parser.from_file_result path |> parsed ~path
let load_all paths = List.map load paths

let require_matching_id path expected actual =
  if expected <> actual then
    Error.fail "Configured game id does not match directory: %s" path

let require_id id config = require_matching_id config.path id config.game.id

let check_id ~path id =
  match Otoml.Parser.from_file_result path with
  | Error error -> Error.fail ~where:path "%s" error
  | Ok value ->
      let root = Table.create path value in
      require_matching_id path id (read_id (Table.section root "game"))
