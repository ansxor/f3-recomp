(* The static analysis tools of [f3recomp mcp]: thin MCP wrappers over [F3recomp.Api.call], which also
   backs the [--json] flags. A game can be named by [game] (the repository layout: games/<game>/config.toml,
   roms/<game>, profiles/<game>.profile) or by explicit [config] and [rom_dir]. *)

open Tool_support

type t = {
  name : string;
  description : string;
  schema : Yojson.Safe.t;
  run : root:string -> F3recomp.Api.session -> Yojson.Safe.t -> Mcp.Request.Tools.Call.result;
}

let game_prop =
  string_prop
    "Game id: resolves games/<game>/config.toml, roms/<game> and (for profile-aware tools) \
     profiles/<game>.profile under the repository root. Alternative to config + rom_dir."

let config_prop = string_prop "Path to the game's config.toml (alternative to game)."
let rom_dir_prop = string_prop "Directory holding the ROM files (default roms/<game> with game)."

let profile_props =
  [
    ("profile_tiers", string_prop "Block profile file whose hit counts are attached (default profiles/<game>.profile with game, if it exists).");
    ("profile_slim", string_prop "Same as profile_tiers (a slim profile).");
    ("no_profile", bool_prop "With game, do not attach the default profile.");
  ]

let game_props = [ ("game", game_prop); ("config", config_prop); ("rom_dir", rom_dir_prop) ]
let region_prop = enum_prop "ROM region." [ "rom"; "sound" ]

let safe_game game =
  if game = "" || String.contains game '/' || game = ".." || game = "." then fail "invalid game id %S" game

(* Fills config, rom_dir and the default profile from [game] and returns the parameters for [Api.call]. *)
let resolve_game ~root ~with_profile args =
  let args = fields args in
  let has key = List.mem_assoc key args && List.assoc key args <> `Null in
  let args =
    match List.assoc_opt "game" args with
    | None | Some `Null -> args
    | Some (`String game) ->
        safe_game game;
        let add key value args = if has key then args else (key, `String value) :: args in
        let args = add "config" (Filename.concat root (Printf.sprintf "games/%s/config.toml" game)) args in
        let args = add "rom_dir" (Filename.concat root ("roms/" ^ game)) args in
        let profile = Filename.concat root (Printf.sprintf "profiles/%s.profile" game) in
        if with_profile && (not (has "profile_tiers")) && (not (has "profile_slim"))
           && List.assoc_opt "no_profile" args <> Some (`Bool true)
           && Sys.file_exists profile
        then ("profile_tiers", `String profile) :: args
        else args
    | Some _ -> fail "\"game\" must be a string"
  in
  F3recomp.Json.Object
    (List.filter_map
       (fun (key, value) -> if key = "game" || key = "no_profile" then None else Some (key, json_of_yojson value))
       args)

let call_api ~root ~with_profile session name args =
  protect (fun () ->
      let params = resolve_game ~root ~with_profile args in
      structured (yojson_of_json (F3recomp.Api.call session name params)))

let tool ?(with_profile = false) name description properties ?required () =
  let schema = object_schema ?required properties in
  {
    name;
    description;
    schema;
    run =
      (fun ~root session args ->
        protect (fun () ->
            check_known ~schema args;
            call_api ~root ~with_profile session name args));
  }

let all =
  [
    tool "rom_read"
      "Read bytes from an assembled ROM region (rom, sound, sprites, sprites_hi, tiles, tiles_hi, samples, eeprom). \
       Returns the bytes as hex with the region size."
      (game_props
       @ [
           ("region", enum_prop "ROM region." [ "rom"; "sound"; "sprites"; "sprites_hi"; "tiles"; "tiles_hi"; "samples"; "eeprom" ]);
           ("addr", address_prop "Byte offset inside the region.");
           ("len", address_prop "Number of bytes.");
         ])
      ~required:[ "len" ] ();
    tool "disasm"
      "List decoded 68020 instructions in Motorola syntax, with operands, branch targets, memory references (resolved \
       through a5 when given) and escapes. Reads a ROM region (game or config + rom_dir) or literal hex bytes placed at pc."
      (game_props
       @ [
           ("pc", address_prop "First address; must be even. Sound PCs use the 0xc00000 space.");
           ("hex", string_prop "Decode these bytes at pc instead of reading a ROM.");
           ("region", region_prop);
           ("count", int_prop "Instructions to list (default 16).");
           ("to", address_prop "List instructions starting below this address instead of using count.");
           ("a5", address_prop "Small-data base register value for a5-relative references.");
         ])
      ~required:[ "pc" ] ();
    tool ~with_profile:true "inspect"
      "Summarise the program: vectors, routines (with why each is believed live and profile hit counts), RAM code, \
       unresolved computed jumps and the inferred a5 base."
      (game_props @ profile_props
       @ [
           ("a5", address_prop "Small-data base register value (inferred from the ROM when omitted).");
           ("routines", bool_prop "Include the routine list (default true).");
         ])
      ();
    tool ~with_profile:true "xref"
      "List the instructions that touch an address, range or named region, with role (read, write, rmw, call, branch, lea), \
       the containing routine and hit counts."
      (game_props @ profile_props
       @ [
           ("target", string_prop "A hex address, a5+OFFSET / a5-OFFSET, a range START-END, or a region such as ram, palette, sprites, pf0, text, shared, control.");
           ("role", enum_prop "Reference kind to list (default all)." [ "all"; "read"; "write"; "rmw"; "call"; "branch"; "lea" ]);
           ("limit", int_prop "Most references to list (default 500).");
           ("a5", address_prop "Small-data base register value (inferred when omitted).");
         ])
      ~required:[ "target" ] ();
    tool ~with_profile:true "flow"
      "Show the callers and callees of a routine as a tree, plus unresolved computed jumps."
      (game_props @ profile_props
       @ [
           ("target", string_prop "A hex PC, a vector name (reset, irq2, trap5...) or ?dispatch for unresolved dispatch sites.");
           ("depth", int_prop "Callee tree depth (default 2).");
         ])
      ~required:[ "target" ] ();
    tool "coverage"
      "Count the decoded ROM instructions the semantics cannot describe yet, per instruction family."
      game_props ();
    tool "check_irq"
      "Report the flag observations and verdict (proven, unproven) of each ROM interrupt handler."
      game_props ();
    tool ~with_profile:true "decisions"
      "Dump the discovery, selection and sprite decisions as rows. The unfiltered table is very large (hundreds of \
       thousands of rows): prefer sprite_only."
      (game_props @ profile_props
       @ [
           ("sprite_writers", string_prop "JSON file of additional sprite writer PCs and optional lanes.");
           ("sprite_only", bool_prop "Only the sprite decision rows.");
         ])
      ();
    tool "lower"
      "Decode instructions and show the C statements the recompiler would emit for them, with the flags live after each."
      (game_props
       @ [
           ("pc", address_prop "First address; must be even.");
           ("hex", string_prop "Instruction bytes (alternative to a ROM).");
           ("region", region_prop);
           ("count", int_prop "Instructions to lower (default 1).");
           ("live_out", string_prop "Flags live at each exit: a subset of XNZVC, or none (default XNZVC).");
         ])
      ~required:[ "pc" ] ();
    tool "profile_merge"
      "Sum block profile files into one, saturating at the uint64 maximum. Returns the identities and counts."
      [
        ("output", string_prop "Path of the merged profile to write.");
        ("inputs", `Assoc [ ("type", `String "array"); ("items", `Assoc [ ("type", `String "string") ]); ("description", `String "Profile files to merge.") ]);
      ]
      ~required:[ "output"; "inputs" ] ();
  ]
