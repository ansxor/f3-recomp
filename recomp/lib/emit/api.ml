type profile = Tiers of string | Slim of string

type source =
  | Region of { config : string; rom_dir : string; region : string }
  | Bytes of string

type game = {
  loaded : Game_image.t;
  manifest : Rom_manifest.t;
  mutable program : Program.t option;
  images : (Rom_manifest.region, Image.t) Hashtbl.t;
  analyses : (string, Inspect.t) Hashtbl.t;
  mutable coverage : (string * int) list option;
  mutable irq : Report.irq option;
}

type session = {
  games : (string * string, game) Hashtbl.t;
  profiles : (string * float, Profile.t) Hashtbl.t;
}

let session () = { games = Hashtbl.create 4; profiles = Hashtbl.create 4 }

let close session ?game () =
  match game with
  | Some key -> Hashtbl.remove session.games key
  | None -> Hashtbl.reset session.games; Hashtbl.reset session.profiles

let game session ~config ~rom_dir =
  match Hashtbl.find_opt session.games (config, rom_dir) with
  | Some game -> game
  | None ->
      let loaded = Game.load config ~rom_dir in
      let game = { loaded; manifest = Rom_manifest.of_configs [loaded.config]; program = None;
                   images = Hashtbl.create 4; analyses = Hashtbl.create 4; coverage = None;
                   irq = None } in
      Hashtbl.replace session.games (config, rom_dir) game;
      game

let program session ~config ~rom_dir =
  let game = game session ~config ~rom_dir in
  match game.program with
  | Some program -> program
  | None ->
      let program = Game.discover game.loaded in
      game.program <- Some program;
      program

let sound_base = 0xc00000

let image_of session ~config ~rom_dir region =
  let game = game session ~config ~rom_dir in
  match Hashtbl.find_opt game.images region with
  | Some image -> image
  | None ->
      let image =
        match region with
        | Rom_manifest.Rom -> game.loaded.rom
        | region ->
            Rom_manifest.load_region game.manifest ~game:game.loaded.config.game.id region ~rom_dir
      in
      Hashtbl.replace game.images region image;
      image

let image session ~config ~rom_dir ~region =
  image_of session ~config ~rom_dir (Inspect.region_of_name region)

let rom_read session ~config ~rom_dir ~region ~addr ~len =
  Inspect.rom_read ~region (image session ~config ~rom_dir ~region) ~addr ~len

(* The image to decode and the PC inside it. *)
let decode_image session source ~pc =
  match source with
  | Bytes bytes -> { Image.base = pc; bytes }, pc
  | Region { config; rom_dir; region } ->
      let kind = Inspect.region_of_name region in
      if kind <> Rom_manifest.Rom && kind <> Sound then
        Error.fail "only the rom and sound regions hold 68k code";
      let image = image_of session ~config ~rom_dir kind in
      if kind = Sound then
        let pc = if pc < Image.length image then pc + sound_base else pc in
        { image with base = sound_base }, pc
      else image, pc

let disassemble session source ~pc ?count ?stop () =
  let image, pc = decode_image session source ~pc in
  Inspect.disassemble image ~pc ?count ?stop ()

let lower session source ~pc ~count ~live_out =
  let image, pc = decode_image session source ~pc in
  Report.lower image ~pc ~count ~live_out

let profile_file session = function
  | Tiers path | Slim path ->
      let stamp = try (Unix.stat path).st_mtime with Unix.Unix_error _ -> 0. in
      (match Hashtbl.find_opt session.profiles (path, stamp) with
       | Some profile -> profile
       | None ->
           let profile = Profile.load path in
           Hashtbl.replace session.profiles (path, stamp) profile;
           profile)

let selection_profile session = function
  | None -> None
  | Some (Tiers _ as source) -> Some (`Tiers (profile_file session source))
  | Some (Slim _ as source) -> Some (`Slim (profile_file session source))

let analysis session ~config ~rom_dir ?profile ?a5 () =
  let game = game session ~config ~rom_dir in
  let key = Printf.sprintf "%s|%s"
    (match profile with
     | None -> ""
     | Some (Tiers path | Slim path) ->
         Printf.sprintf "%s@%f" path (try (Unix.stat path).st_mtime with Unix.Unix_error _ -> 0.))
    (match a5 with None -> "" | Some value -> string_of_int value) in
  match Hashtbl.find_opt game.analyses key with
  | Some analysis -> analysis
  | None ->
      let program = program session ~config ~rom_dir in
      let profile = Option.map (profile_file session) profile in
      let analysis = Inspect.make_of_program ?profile ?a5 program in
      Hashtbl.replace game.analyses key analysis;
      analysis

let inspect session ~config ~rom_dir ?profile ?a5 ?routines () =
  Inspect.inspect_json ?routines (analysis session ~config ~rom_dir ?profile ?a5 ())

let xref session ~config ~rom_dir ?profile ?a5 ?role ?limit ~target () =
  Inspect.xref_json ?role ?limit (analysis session ~config ~rom_dir ?profile ?a5 ()) ~target

let flow session ~config ~rom_dir ?profile ?depth ~target () =
  Inspect.flow_json ?depth (analysis session ~config ~rom_dir ?profile ()) ~target

let coverage session ~config ~rom_dir =
  let game = game session ~config ~rom_dir in
  let counts = match game.coverage with
    | Some counts -> counts
    | None ->
        let counts = Report.coverage_counts game.loaded in
        game.coverage <- Some counts;
        counts
  in
  counts, game.loaded.config.game.id

let check_irq session ~config ~rom_dir =
  let game = game session ~config ~rom_dir in
  match game.irq with
  | Some irq -> irq
  | None ->
      let irq = Report.check_irq (program session ~config ~rom_dir) in
      game.irq <- Some irq;
      irq

let decisions session ~config ~rom_dir ?profile ?sprite_writers ?sprite_header ?sprite_stats
    ~sprite_only () =
  let program = program session ~config ~rom_dir in
  let writers = Option.fold ~none:[] ~some:Sprite_trace.load_writer_seeds sprite_writers in
  let tsv = Report.decisions_tsv ?sprite_header ?sprite_stats ~writers
    ?profile:(selection_profile session profile) ~sprite_only program in
  tsv, program.game.config.game.id

let profile_merge ~output inputs =
  let canonical path = try Unix.realpath path with Unix.Unix_error _ -> path in
  let paths = List.map canonical inputs in
  if List.length (List.sort_uniq String.compare paths) <> List.length paths then
    Error.fail "duplicate input paths would double-count one run";
  let merged = match List.map Profile.load inputs with
    | [] -> Error.fail "profile-merge needs at least one input"
    | first :: rest -> List.fold_left Profile.merge first rest
  in
  Profile.write output merged;
  let stats = Profile.stats merged in
  Json.Object [
    "output", String output;
    "inputs", Int (List.length inputs);
    "roms", Int stats.identities;
    "ever_executed", Int stats.executed;
    "cold_misses", Int stats.cold_misses;
  ]

(* ---- text forms ---- *)

(* Text views render the JSON documents, so both forms always agree. *)
let get key json = Json.member key json
let list_of = function Some (Json.Array items) -> items | _ -> []
let int_of = function Some (Json.Int n) -> Some n | _ -> None
let text_of = function Some (Json.String text) -> text | _ -> ""
let pc_text = function Some value -> Printf.sprintf "0x%06x" value | None -> "-"
let hits_text json = match get "hits" json with Some (Json.Int64 n) -> Int64.to_string n | _ -> "-"
let inspect_text json =
  let buffer = Buffer.create 4096 in
  let line fmt = Printf.bprintf buffer fmt in
  line "game\t%s\n" (text_of (get "game" json));
  line "rom_size\t%d\n" (Option.value (int_of (get "rom_size" json)) ~default:0);
  line "rom_sha256\t%s\n" (text_of (get "rom_sha256" json));
  line "a5\t%s\n" (pc_text (int_of (get "a5" json)));
  line "routines\t%d\n" (Option.value (int_of (get "routine_count" json)) ~default:0);
  line "\nvector\tname\ttarget\n";
  List.iter (fun vector ->
    line "%d\t%s\t%s\n" (Option.value (int_of (get "number" vector)) ~default:0)
      (text_of (get "name" vector)) (pc_text (int_of (get "target" vector))))
    (list_of (get "vectors" json));
  (match get "routines" json with
   | None -> ()
   | Some _ ->
       line "\nentry\tkind\tinsns\tbytes\tcallers\tcallees\thits\n";
       List.iter (fun routine ->
         line "%s\t%s\t%d\t%d\t%d\t%d\t%s\n" (pc_text (int_of (get "entry" routine)))
           (text_of (get "kind" routine))
           (Option.value (int_of (get "instructions" routine)) ~default:0)
           (Option.value (int_of (get "bytes" routine)) ~default:0)
           (List.length (list_of (get "callers" routine)))
           (List.length (list_of (get "callees" routine))) (hits_text routine))
         (list_of (get "routines" json)));
  line "\nram_code\tentry\tlength\tinstructions\n";
  List.iter (fun ram ->
    line "%s\t%s\t%d\t%d\n" (pc_text (int_of (get "destination" ram)))
      (pc_text (int_of (get "entry" ram)))
      (Option.value (int_of (get "length" ram)) ~default:0)
      (Option.value (int_of (get "instructions" ram)) ~default:0))
    (list_of (get "ram_code" json));
  line "\nunresolved\troutine\ttext\n";
  List.iter (fun site ->
    line "%s\t%s\t%s\n" (pc_text (int_of (get "pc" site))) (pc_text (int_of (get "routine" site)))
      (text_of (get "text" site)))
    (list_of (get "unresolved_indirect_sites" json));
  Buffer.contents buffer

let xref_text json =
  let buffer = Buffer.create 4096 in
  Printf.bprintf buffer "pc\troutine\trole\ttarget\thits\ttext\n";
  List.iter (fun row ->
    Printf.bprintf buffer "%s\t%s\t%s\t%s\t%s\t%s\n" (pc_text (int_of (get "pc" row)))
      (pc_text (int_of (get "routine" row))) (text_of (get "role" row))
      (pc_text (int_of (get "target" row))) (hits_text row) (text_of (get "text" row)))
    (list_of (get "refs" json));
  (match get "truncated" json, int_of (get "total" json) with
   | Some (Json.Bool true), Some total -> Printf.bprintf buffer "# %d references in total\n" total
   | _ -> ());
  Buffer.contents buffer

let flow_text json =
  let buffer = Buffer.create 1024 in
  let line indent fmt = Printf.ksprintf (fun text ->
    Buffer.add_string buffer (String.make indent ' ' ^ text ^ "\n")) fmt in
  line 0 "target %s entry %s kind %s" (text_of (get "target" json))
    (pc_text (int_of (get "entry" json))) (text_of (get "kind" json));
  line 0 "callers";
  List.iter (fun edge ->
    line 2 "%s %s at %s" (pc_text (int_of (get "routine" edge))) (text_of (get "kind" edge))
      (pc_text (int_of (get "site" edge))))
    (list_of (get "callers" json));
  line 0 "callees";
  let rec tree indent edge =
    line indent "%s %s at %s%s" (pc_text (int_of (get "routine" edge))) (text_of (get "kind" edge))
      (pc_text (int_of (get "site" edge)))
      (match get "repeated" edge with Some (Json.Bool true) -> " (repeated)" | _ -> "");
    List.iter (tree (indent + 2)) (list_of (get "children" edge))
  in
  List.iter (tree 2) (list_of (get "callees" json));
  line 0 "unresolved";
  List.iter (fun site ->
    line 2 "%s in %s %s" (pc_text (int_of (get "pc" site))) (pc_text (int_of (get "routine" site)))
      (text_of (get "text" site)))
    (list_of (get "unresolved" json));
  Buffer.contents buffer

(* ---- named calls (the MCP tools of [f3recomp mcp]) ---- *)

let methods = [
  "rom_read"; "disasm"; "inspect"; "xref"; "flow"; "coverage"; "check_irq"; "decisions"; "lower";
  "profile_merge";
]

let field params key = Json.member key params
let missing key = Error.fail "missing parameter %S" key

let string_param params key =
  match field params key with
  | Some (Json.String text) -> Some text
  | None | Some Null -> None
  | Some _ -> Error.fail "parameter %S must be a string" key

let required_string params key =
  match string_param params key with Some text -> text | None -> missing key

(* Addresses may be integers or hex strings. *)
let number_param params key =
  match field params key with
  | Some (Json.Int n) -> Some n
  | Some (Json.String text) -> Some (Inspect.parse_address text)
  | None | Some Null -> None
  | Some _ -> Error.fail "parameter %S must be an integer or hex string" key

let required_number params key =
  match number_param params key with Some n -> n | None -> missing key

let int_param params key =
  match field params key with
  | Some (Json.Int n) -> Some n
  | None | Some Null -> None
  | Some _ -> Error.fail "parameter %S must be an integer" key

let bool_param params key =
  match field params key with
  | Some (Json.Bool value) -> Some value
  | None | Some Null -> None
  | Some _ -> Error.fail "parameter %S must be a boolean" key

let profile_param params =
  match string_param params "profile_tiers", string_param params "profile_slim" with
  | None, None -> None
  | Some path, None -> Some (Tiers path)
  | None, Some path -> Some (Slim path)
  | Some _, Some _ -> Error.fail "profile_tiers and profile_slim are mutually exclusive"

let game_params params = required_string params "config", required_string params "rom_dir"

let source_param params =
  match string_param params "hex", string_param params "config" with
  | Some hex, None -> Bytes (Report.parse_hex hex)
  | None, Some config ->
      Region { config; rom_dir = required_string params "rom_dir";
               region = Option.value (string_param params "region") ~default:"rom" }
  | Some _, Some _ -> Error.fail "pass either hex or config and rom_dir"
  | None, None -> Error.fail "pass hex, or config and rom_dir"

let check_pc pc = if pc < 0 || pc > 0xffffffff || pc land 1 <> 0 then
  Error.fail "pc must be an even 32-bit address"

let call session meth params =
  match meth with
  | "rom_read" ->
      let config, rom_dir = game_params params in
      rom_read session ~config ~rom_dir
        ~region:(Option.value (string_param params "region") ~default:"rom")
        ~addr:(Option.value (number_param params "addr") ~default:0) ~len:(required_number params "len")
  | "disasm" ->
      let source = source_param params in
      let pc = required_number params "pc" in
      check_pc pc;
      let a5 = number_param params "a5" in
      let count, stop = int_param params "count", number_param params "to" in
      let count = match count, stop with None, None -> Some 16 | _ -> count in
      Inspect.disasm_json ?a5 (disassemble session source ~pc ?count ?stop ())
  | "inspect" ->
      let config, rom_dir = game_params params in
      inspect session ~config ~rom_dir ?profile:(profile_param params)
        ?a5:(number_param params "a5") ?routines:(bool_param params "routines") ()
  | "xref" ->
      let config, rom_dir = game_params params in
      xref session ~config ~rom_dir ?profile:(profile_param params) ?a5:(number_param params "a5")
        ?role:(string_param params "role") ?limit:(int_param params "limit")
        ~target:(required_string params "target") ()
  | "flow" ->
      let config, rom_dir = game_params params in
      flow session ~config ~rom_dir ?profile:(profile_param params)
        ?depth:(int_param params "depth") ~target:(required_string params "target") ()
  | "coverage" ->
      let config, rom_dir = game_params params in
      let counts, game = coverage session ~config ~rom_dir in
      Report.coverage_json ~game counts
  | "check_irq" ->
      let config, rom_dir = game_params params in
      Report.check_irq_json (check_irq session ~config ~rom_dir)
  | "decisions" ->
      let config, rom_dir = game_params params in
      let sprite_only = Option.value (bool_param params "sprite_only") ~default:false in
      let tsv, game = decisions session ~config ~rom_dir ?profile:(profile_param params)
        ?sprite_writers:(string_param params "sprite_writers") ~sprite_only () in
      Report.decisions_json ~game ~sprite_only tsv
  | "lower" ->
      let source = source_param params in
      let pc = required_number params "pc" in
      check_pc pc;
      let count = Option.value (int_param params "count") ~default:1 in
      if count < 1 then Error.fail "count must be positive";
      let live_out = Report.parse_live_out
        (Option.value (string_param params "live_out") ~default:"XNZVC") in
      Report.lower_json ~live_out (lower session source ~pc ~count ~live_out)
  | "profile_merge" ->
      let inputs = match field params "inputs" with
        | Some (Json.Array items) ->
            List.map (function Json.String path -> path
                             | _ -> Error.fail "inputs must be strings") items
        | _ -> missing "inputs" in
      profile_merge ~output:(required_string params "output") inputs
  | other -> Error.fail "unknown method %S" other

let describe_exception = function
  | Error.Invalid { where; what } -> if where = "" then what else where ^ ": " ^ what
  | Sys_error message -> message
  | Unix.Unix_error (error, operation, path) ->
      Printf.sprintf "%s %s: %s" operation path (Unix.error_message error)
  | exn -> Printexc.to_string exn
