open Cmdliner
open F3recomp

let report_error run =
  try
    run ();
    0
  with
  | Error.Invalid { where; what } ->
      prerr_endline (if where = "" then what else where ^ ": " ^ what);
      1
  | Sys_error message ->
      prerr_endline message;
      1
  | Unix.Unix_error (error, operation, path) ->
      Printf.eprintf "%s %s: %s\n" operation path (Unix.error_message error);
      1

let sprite_units =
  let config = Arg.(required & opt (some string) None & info [ "config" ] ~docv:"FILE") in
  let output_dir = Arg.(required & opt (some string) None & info [ "output-dir" ] ~docv:"DIR") in
  let run config output_dir =
    report_error (fun () ->
        Config.load config |> Emit_units.of_config |> Emit_units.render
        |> Output.write ~dir:output_dir)
  in
  Cmd.v
    (Cmd.info "sprite-units" ~doc:"Generate the sprite emit-unit headers.")
    Term.(const run $ config $ output_dir)

let enhancements =
  let config = Arg.(required & opt (some string) None & info [ "config" ] ~docv:"FILE") in
  let output_dir = Arg.(required & opt (some string) None & info [ "output-dir" ] ~docv:"DIR") in
  let run config output_dir =
    report_error (fun () ->
        Config.load config |> Enhancements.default |> Enhancements.render
        |> Output.write ~dir:output_dir)
  in
  Cmd.v
    (Cmd.info "enhancements" ~doc:"Generate the default enhancements header.")
    Term.(const run $ config $ output_dir)

let check_game =
  let config = Arg.(required & pos 0 (some string) None & info [] ~docv:"CONFIG") in
  let id = Arg.(required & pos 1 (some string) None & info [] ~docv:"GAME") in
  let run path id = report_error (fun () -> Config.check_id ~path id) in
  Cmd.v (Cmd.info "check-game") Term.(const run $ config $ id)

let compile_roms =
  (* Cmdliner consumes the first config as the option value and the rest as positionals. *)
  let configs =
    Arg.(
      required
      & opt (some string) None
      & info [ "configs" ] ~docv:"CONFIG"
          ~doc:"Read CONFIG followed by any additional config paths.")
  in
  let more_configs = Arg.(value & pos_all string [] & info [] ~docv:"CONFIG") in
  let output = Arg.(required & opt (some string) None & info [ "output" ] ~docv:"PATH") in
  let game = Arg.(value & opt (some string) None & info [ "game" ] ~docv:"GAME") in
  let game_output = Arg.(value & opt (some string) None & info [ "game-output" ] ~docv:"PATH") in
  let run config more_configs output game game_output =
    report_error (fun () ->
        if Option.is_some game <> Option.is_some game_output then
          Error.fail "--game and --game-output go together";
        let headers =
          Config.load_all (config :: more_configs)
          |> Rom_manifest.of_configs |> Rom_manifest.render ?game
        in
        Output.write_file output headers.manifest;
        match (game_output, headers.game_config) with
        | Some path, Some contents -> Output.write_file path contents
        | _ -> ())
  in
  Cmd.v (Cmd.info "compile-roms")
    Term.(const run $ configs $ more_configs $ output $ game $ game_output)

let load_profile tiers slim =
  match tiers, slim with
  | None, None -> None
  | Some path, None -> Some (`Tiers (Profile.load path))
  | None, Some path -> Some (`Slim (Profile.load path))
  | Some _, Some _ -> Error.fail "--profile-tiers and --profile-slim are mutually exclusive"

let profile_source tiers slim =
  match tiers, slim with
  | None, None -> None
  | Some path, None -> Some (Api.Tiers path)
  | None, Some path -> Some (Api.Slim path)
  | Some _, Some _ -> Error.fail "--profile-tiers and --profile-slim are mutually exclusive"

(* Hexadecimal with an optional 0x or $ prefix. *)
let address =
  let parse text =
    match Inspect.parse_address text with
    | value -> Ok value
    | exception Error.Invalid { what; _ } -> Error (`Msg what)
  in
  Arg.conv (parse, fun ppf value -> Stdlib.Format.fprintf ppf "%#x" value)

let config_arg = Arg.(required & opt (some string) None & info [ "config" ] ~docv:"FILE")
let rom_dir_arg = Arg.(required & opt (some string) None & info [ "rom-dir" ] ~docv:"DIR")
let optional_config = Arg.(value & opt (some string) None & info [ "config" ] ~docv:"FILE")
let optional_rom_dir = Arg.(value & opt (some string) None & info [ "rom-dir" ] ~docv:"DIR")
let json_flag = Arg.(value & flag & info [ "json" ] ~doc:"Print JSON instead of text.")
let profile_tiers_arg =
  Arg.(value & opt (some string) None & info [ "profile-tiers" ] ~docv:"FILE"
         ~doc:"Attach hit counts from a block profile.")
let profile_slim_arg =
  Arg.(value & opt (some string) None & info [ "profile-slim" ] ~docv:"FILE"
         ~doc:"Attach hit counts from a block profile (same as --profile-tiers).")
let a5_arg =
  Arg.(value & opt (some address) None & info [ "a5" ] ~docv:"ADDRESS"
         ~doc:"Small-data base register value. Inferred from the ROM when omitted.")

let print_json json = print_endline (Json.to_string json)

let decisions =
  let config = config_arg in
  let rom_dir = rom_dir_arg in
  let profile_tiers = profile_tiers_arg in
  let profile_slim = profile_slim_arg in
  let sprite_writers =
    Arg.(value & opt (some string) None & info [ "sprite-writers" ] ~docv:"JSON" ~doc:"Load additional sprite writer PCs and optional lanes.")
  in
  let sprite_only = Arg.(value & flag & info [ "sprite-only" ] ~doc:"Print only sprite decision rows.") in
  let sprite_header =
    Arg.(value & opt (some string) None & info [ "sprite-header" ] ~docv:"PATH" ~doc:"Write the generated sprite_groups.hpp to PATH.")
  in
  let sprite_stats =
    Arg.(value & opt (some string) None & info [ "sprite-stats" ] ~docv:"PATH" ~doc:"Write sprite trace counters as key/value TSV.")
  in
  let run config rom_dir tiers slim sprite_writers sprite_only sprite_header sprite_stats json =
    report_error (fun () ->
        let session = Api.session () in
        let tsv, game =
          Api.decisions session ~config ~rom_dir ?profile:(profile_source tiers slim)
            ?sprite_writers ?sprite_header ?sprite_stats ~sprite_only ()
        in
        if json then print_json (Report.decisions_json ~game ~sprite_only tsv)
        else print_string tsv)
  in
  Cmd.v (Cmd.info "decisions" ~doc:"Dump discovery, selection and sprite decisions.")
    Term.(const run $ config $ rom_dir $ profile_tiers $ profile_slim
          $ sprite_writers $ sprite_only $ sprite_header $ sprite_stats $ json_flag)

let check_irq =
  let run config rom_dir json =
    report_error (fun () ->
        let irq = Api.check_irq (Api.session ()) ~config ~rom_dir in
        if json then print_json (Report.check_irq_json irq)
        else print_string (Report.check_irq_text irq))
  in
  Cmd.v (Cmd.info "check-irq" ~doc:"Report the flag observations of each ROM interrupt handler.")
    Term.(const run $ config_arg $ rom_dir_arg $ json_flag)

let lower =
  let pc = Arg.(required & opt (some int) None & info [ "pc" ] ~docv:"ADDRESS") in
  let hex =
    Arg.(value & opt (some string) None & info [ "hex" ] ~docv:"BYTES"
         ~doc:"Instruction bytes. Alternatively read them from a ROM with --config and --rom-dir.")
  in
  let region =
    Arg.(value & opt string "rom" & info [ "region" ] ~docv:"REGION"
         ~doc:"ROM region to read with --config: rom or sound.")
  in
  let count = Arg.(value & opt int 1 & info [ "count" ] ~docv:"N") in
  let live_out =
    Arg.(value & opt string "XNZVC" & info [ "live-out" ] ~docv:"FLAGS"
         ~doc:"Live flags at each instruction exit, as a subset of XNZVC or none.")
  in
  let run pc hex config rom_dir region count live_out json =
    report_error (fun () ->
        if pc < 0 || pc > 0xffffffff || pc mod 2 <> 0 then Error.fail "--pc must be an even 32-bit address";
        if count < 1 then Error.fail "--count must be positive";
        let live_out = Report.parse_live_out live_out in
        let source =
          match hex, config, rom_dir with
          | Some hex, None, None -> Api.Bytes (Report.parse_hex hex)
          | None, Some config, Some rom_dir -> Api.Region { config; rom_dir; region }
          | None, None, _ | None, _, None -> Error.fail "pass --hex, or --config with --rom-dir"
          | Some _, _, _ -> Error.fail "--hex cannot be combined with --config"
        in
        let lowered = Api.lower (Api.session ()) source ~pc ~count ~live_out in
        if json then print_json (Report.lower_json ~live_out lowered)
        else print_string (Report.lower_text ~count lowered))
  in
  Cmd.v (Cmd.info "lower" ~doc:"Decode instructions and print their C statements.")
    Term.(const run $ pc $ hex $ optional_config $ optional_rom_dir $ region $ count $ live_out
          $ json_flag)

let coverage =
  let run config rom_dir json =
    report_error (fun () ->
        let counts, game = Api.coverage (Api.session ()) ~config ~rom_dir in
        if json then print_json (Report.coverage_json ~game counts)
        else print_string (Report.coverage_text counts))
  in
  Cmd.v (Cmd.info "coverage" ~doc:"Count decoded ROM instructions the semantics cannot describe yet.")
    Term.(const run $ config_arg $ rom_dir_arg $ json_flag)

let rom_read =
  let region =
    Arg.(required & opt (some string) None & info [ "region" ] ~docv:"REGION"
         ~doc:"One of rom, sound, sprites, sprites_hi, tiles, tiles_hi, samples, eeprom.")
  in
  let addr = Arg.(value & opt address 0 & info [ "addr" ] ~docv:"ADDRESS"
                    ~doc:"Hexadecimal byte offset inside the region.") in
  let len = Arg.(required & opt (some int) None & info [ "len" ] ~docv:"N" ~doc:"Number of bytes.") in
  let format =
    Arg.(value & opt (enum [ "json", `Json; "hex", `Hex; "raw", `Raw ]) `Json
         & info [ "format" ] ~docv:"FORMAT" ~doc:"json (default), hex or raw.")
  in
  let run config rom_dir region addr len format =
    report_error (fun () ->
        let session = Api.session () in
        match format with
        | `Json -> print_json (Api.rom_read session ~config ~rom_dir ~region ~addr ~len)
        | `Hex ->
            let image = Api.image session ~config ~rom_dir ~region in
            print_endline (Inspect.hex_of_bytes (Inspect.read_bytes image ~addr ~len))
        | `Raw ->
            let image = Api.image session ~config ~rom_dir ~region in
            set_binary_mode_out stdout true;
            print_string (Inspect.read_bytes image ~addr ~len))
  in
  Cmd.v (Cmd.info "rom-read" ~doc:"Read bytes from an assembled ROM region.")
    Term.(const run $ config_arg $ rom_dir_arg $ region $ addr $ len $ format)

let disasm =
  let pc = Arg.(required & opt (some address) None & info [ "pc" ] ~docv:"ADDRESS") in
  let hex =
    Arg.(value & opt (some string) None & info [ "hex" ] ~docv:"BYTES"
         ~doc:"Decode these bytes placed at --pc instead of reading a ROM.")
  in
  let region =
    Arg.(value & opt string "rom" & info [ "region" ] ~docv:"REGION"
         ~doc:"rom or sound. Sound PCs are in the 0xc00000 address space.")
  in
  let count = Arg.(value & opt (some int) None & info [ "count" ] ~docv:"N"
                     ~doc:"Instructions to list (default 16).") in
  let stop = Arg.(value & opt (some address) None & info [ "to" ] ~docv:"ADDRESS"
                    ~doc:"List instructions starting below this address.") in
  let run pc hex config rom_dir region count stop a5 json =
    report_error (fun () ->
        if Option.is_some count && Option.is_some stop then Error.fail "--count and --to are exclusive";
        if (match count with Some n -> n < 1 | None -> false) then Error.fail "--count must be positive";
        if pc land 1 <> 0 then Error.fail "--pc must be even";
        let source =
          match hex, config, rom_dir with
          | Some hex, None, None -> Api.Bytes (Report.parse_hex hex)
          | None, Some config, Some rom_dir -> Api.Region { config; rom_dir; region }
          | None, None, _ | None, _, None -> Error.fail "pass --hex, or --config with --rom-dir"
          | Some _, _, _ -> Error.fail "--hex cannot be combined with --config"
        in
        let count = match count, stop with None, None -> Some 16 | _ -> count in
        let lines = Api.disassemble (Api.session ()) source ~pc ?count ?stop () in
        if json then print_json (Inspect.disasm_json ?a5 lines)
        else List.iter (fun line -> print_endline (Inspect.line_to_text line)) lines)
  in
  Cmd.v (Cmd.info "disasm" ~doc:"List decoded instructions in Motorola syntax.")
    Term.(const run $ pc $ hex $ optional_config $ optional_rom_dir $ region $ count $ stop
          $ a5_arg $ json_flag)

let inspect =
  let summary = Arg.(value & flag & info [ "summary" ] ~doc:"Omit the routine list.") in
  let run config rom_dir tiers slim a5 summary json =
    report_error (fun () ->
        let result =
          Api.inspect (Api.session ()) ~config ~rom_dir ?profile:(profile_source tiers slim) ?a5
            ~routines:(not summary) ()
        in
        if json then print_json result else print_string (Api.inspect_text result))
  in
  Cmd.v (Cmd.info "inspect" ~doc:"Summarise vectors, routines, RAM code and unresolved computed jumps.")
    Term.(const run $ config_arg $ rom_dir_arg $ profile_tiers_arg $ profile_slim_arg $ a5_arg
          $ summary $ json_flag)

let xref =
  let target =
    Arg.(required & opt (some string) None & info [ "target" ] ~docv:"TARGET"
         ~doc:"A hex address, a5+OFFSET or a5-OFFSET, a hex range START-END, or a named F3 region \
               such as ram, palette, sprites, pf0, text, shared.")
  in
  let role =
    Arg.(value & opt string "all" & info [ "role" ] ~docv:"ROLE"
         ~doc:"all, read, write (includes rmw), rmw, call, branch or lea.")
  in
  let limit = Arg.(value & opt int 500 & info [ "limit" ] ~docv:"N" ~doc:"Most references to list.") in
  let run config rom_dir tiers slim a5 target role limit json =
    report_error (fun () ->
        if limit < 0 then Error.fail "--limit must not be negative";
        let result =
          Api.xref (Api.session ()) ~config ~rom_dir ?profile:(profile_source tiers slim) ?a5 ~role
            ~limit ~target ()
        in
        if json then print_json result else print_string (Api.xref_text result))
  in
  Cmd.v (Cmd.info "xref" ~doc:"List the instructions that touch an address, range or region.")
    Term.(const run $ config_arg $ rom_dir_arg $ profile_tiers_arg $ profile_slim_arg $ a5_arg
          $ target $ role $ limit $ json_flag)

let flow =
  let target =
    Arg.(required & opt (some string) None & info [ "target" ] ~docv:"TARGET"
         ~doc:"A hex PC, a vector name such as reset or irq2, or ?dispatch.")
  in
  let depth = Arg.(value & opt int 2 & info [ "depth" ] ~docv:"N" ~doc:"Callee tree depth.") in
  let run config rom_dir tiers slim target depth json =
    report_error (fun () ->
        let result =
          Api.flow (Api.session ()) ~config ~rom_dir ?profile:(profile_source tiers slim) ~depth
            ~target ()
        in
        if json then print_json result else print_string (Api.flow_text result))
  in
  Cmd.v (Cmd.info "flow" ~doc:"Show the callers and callees of a routine.")
    Term.(const run $ config_arg $ rom_dir_arg $ profile_tiers_arg $ profile_slim_arg $ target
          $ depth $ json_flag)

let profile_merge =
  let output = Arg.(required & opt (some string) None & info [ "output" ] ~docv:"PATH") in
  let inputs = Arg.(non_empty & pos_all string [] & info [] ~docv:"INPUT") in
  let run output inputs json =
    report_error (fun () ->
        let result = Api.profile_merge ~output inputs in
        if json then print_json result
        else Printf.printf "merged %d profiles into %s\n" (List.length inputs) output)
  in
  Cmd.v (Cmd.info "profile-merge" ~doc:"Sum block profiles, saturating at the uint64 maximum.")
    Term.(const run $ output $ inputs $ json_flag)

let mcp =
  let root =
    Arg.(value & opt string (Sys.getcwd ()) & info [ "root" ] ~docv:"DIR"
           ~doc:"Repository root: games/<game>/config.toml, roms/<game>, profiles/ and build/<game>/f3rt-tool \
                 resolve under it (default: the current directory).")
  in
  let game =
    Arg.(value & opt (some string) None & info [ "game" ] ~docv:"GAME"
           ~doc:"Discover build/<game>/f3rt-tool's tools at startup and list them as rt_<name> from the start, \
                 before any session_open.")
  in
  let run root game = report_error (fun () -> Mcp_command.run ~root ~game) in
  Cmd.v
    (Cmd.info "mcp"
       ~doc:"Serve static analysis and live runtime sessions over the Model Context Protocol (stdio)."
       ~man:[ `S Manpage.s_description;
              `P "Static tools (rom_read, disasm, inspect, xref, flow, coverage, check_irq, decisions, lower, \
                  profile_merge) run in this process. session_open spawns build/<game>/f3rt-tool mcp and registers \
                  its tools as rt_<name>, sending notifications/tools/list_changed; session_close removes them. A \
                  rebuilt f3rt-tool makes rt_* calls fail until the session is reopened." ])
    Term.(const run $ root $ game)

let emit =
  let required_path name = Arg.(required & opt (some string) None & info [name]) in
  let optional_path name = Arg.(value & opt (some string) None & info [name]) in
  let config = required_path "config" in
  let rom_dir = required_path "rom-dir" in
  let output = required_path "output" in
  let config_output = optional_path "config-output-dir" in
  let tiers = optional_path "profile-tiers" in
  let slim = optional_path "profile-slim" in
  let writers = optional_path "sprite-writers" in
  let parity = optional_path "parity-from" in
  let flags = Arg.(value & opt (enum ["lazy", `Lazy; "eager", `Eager]) `Lazy & info ["flags"]) in
  let max_block = Arg.(value & opt int 32 & info ["max-block-instructions"]) in
  let allow_unproven =
    Arg.(value & flag & info ["allow-unproven-irq"] ~doc:"Temporarily warn about unknown interrupt handlers.") in
  let run config rom_dir output config_output tiers slim writers parity flags max_block allow_unproven =
    report_error (fun () ->
      if max_block < 1 then Error.fail "--max-block-instructions must be positive";
      if Option.is_some parity && flags = `Eager then
        Error.fail "--parity-from and --flags eager are mutually exclusive";
      let profile = load_profile tiers slim in
      let writers = Option.fold ~none:[] ~some:Sprite_trace.load_writer_seeds writers in
      let analysis =
        Game.load config ~rom_dir |> Game.discover |> Program.select ~profile
        |> Analysis.run ~parity ~allow_unproven ~writers in
      let layout =
        Analysis.lower ~flags analysis
        |> Layout.make ~max_block ~selection:analysis.selection ~idle:analysis.idle in
      let sources = Analysis.sources analysis layout in
      let groups = analysis.groups in
      let enhancements = Enhancements.of_config analysis.selection.config groups |> Enhancements.render in
      let summary = Summary.of_layout layout analysis in
      Program_c.write ~dir:output
        (sources @ ["sprite_groups.hpp", Sprite_groups.render_hpp groups;
                    "sprite_groups.json", Sprite_groups.render_json groups]);
      Output.write ~dir:(Option.value config_output ~default:output) enhancements;
      Summary.print summary)
  in
  Cmd.v (Cmd.info "emit" ~doc:"Generate the native main CPU program.")
    Term.(const run $ config $ rom_dir $ output $ config_output $ tiers $ slim $ writers
          $ parity $ flags $ max_block $ allow_unproven)

let () =
  exit
    (Cmd.eval'
       (Cmd.group (Cmd.info "f3recomp")
          [sprite_units; compile_roms; check_game; check_irq; decisions; lower; coverage; enhancements; emit;
           rom_read; disasm; inspect; xref; flow; profile_merge; mcp]))
