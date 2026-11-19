open Cmdliner
open F3recomp

let stub name =
  let run () =
    prerr_endline "not implemented";
    2
  in
  Cmd.v (Cmd.info name) Term.(const run $ const ())

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

let decisions =
  let config = Arg.(required & opt (some string) None & info [ "config" ] ~docv:"FILE") in
  let rom_dir = Arg.(required & opt (some string) None & info [ "rom-dir" ] ~docv:"DIR") in
  let profile_option name = Arg.(value & opt (some string) None & info [ name ] ~docv:"FILE") in
  let profile_tiers = profile_option "profile-tiers" in
  let profile_slim = profile_option "profile-slim" in
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
  let run config_path rom_dir tiers slim sprite_writers sprite_only sprite_header sprite_stats =
    report_error (fun () ->
        let profile = match tiers, slim with
          | None, None -> None
          | Some path, None -> Some (`Tiers (Profile.load path))
          | None, Some path -> Some (`Slim (Profile.load path))
          | Some _, Some _ -> Error.fail "--profile-tiers and --profile-slim are mutually exclusive"
        in
        let writers = Option.fold ~none:[] ~some:Sprite_trace.load_writer_seeds sprite_writers in
        let game = Game.load config_path ~rom_dir in
        let program = Game.discover game in
        let groups = Sprite_trace.run program ~config:game.config ~writers in
        Option.iter (fun path -> Output.write_file path (Sprite_groups.render_hpp groups)) sprite_header;
        Option.iter (fun path ->
            groups.stats
            |> List.map (fun (key, count) -> Printf.sprintf "%s\t%d\n" key count)
            |> String.concat "" |> Output.write_file path) sprite_stats;
        let rows = Sprite_groups.decision_rows groups in
        if sprite_only then (
          print_endline Decisions.header;
          List.iter (fun (_, row) -> print_string row) rows)
        else Program.select ~profile program |> Decisions.write ~sprite_rows:rows stdout)
  in
  Cmd.v (Cmd.info "decisions" ~doc:"Dump discovery, selection and sprite decisions.")
    Term.(const run $ config $ rom_dir $ profile_tiers $ profile_slim
          $ sprite_writers $ sprite_only $ sprite_header $ sprite_stats)

let lower =
  let pc = Arg.(required & opt (some int) None & info [ "pc" ] ~docv:"ADDRESS") in
  let hex = Arg.(required & opt (some string) None & info [ "hex" ] ~docv:"BYTES") in
  let count = Arg.(value & opt int 1 & info [ "count" ] ~docv:"N") in
  let live_out =
    Arg.(value & opt string "XNZVC" & info [ "live-out" ] ~docv:"FLAGS"
         ~doc:"Live flags at each instruction exit, as a subset of XNZVC or none.")
  in
  let parse_live_out value =
    let value = String.uppercase_ascii value in
    if value = "NONE" then Ccr.none
    else
      String.fold_left
        (fun set char ->
          let flag = match char with
            | 'X' -> Ccr.X | 'N' -> N | 'Z' -> Z | 'V' -> V | 'C' -> C
            | _ -> Error.fail "invalid --live-out %S" value
          in
          Ccr.union set (Ccr.singleton flag))
        Ccr.none value
  in
  let parse_hex hex =
    let digits = Buffer.create (String.length hex) in
    String.iter
      (function
        | ' ' | '\t' | '\r' | '\n' -> ()
        | ('0' .. '9' | 'a' .. 'f' | 'A' .. 'F') as digit -> Buffer.add_char digits digit
        | _ -> Error.fail "invalid --hex byte string")
      hex;
    let digits = Buffer.contents digits in
    if String.length digits mod 2 <> 0 then Error.fail "--hex requires whole bytes";
    String.init (String.length digits / 2) (fun i ->
        Char.chr (int_of_string ("0x" ^ String.sub digits (2 * i) 2)))
  in
  let run pc hex count live_out =
    report_error (fun () ->
        if pc < 0 || pc > 0xffffffff || pc mod 2 <> 0 then Error.fail "--pc must be an even 32-bit address";
        if count < 1 then Error.fail "--count must be positive";
        let live_out = parse_live_out live_out in
        let image = { Image.base = pc; bytes = parse_hex hex } in
        let rec listing pc remaining acc =
          if remaining = 0 then List.rev acc
          else
            let decoded = Decode.at image pc in
            let length = match decoded with Insn insn -> insn.length | Illegal illegal -> illegal.length in
            let stmts =
              decoded |> Semantics.of_decoded ~pc |> Lower.entry ~memory:Memory_path.Inline ~live_out
              |> function
              | Ok entry -> entry.stmts
              | Error reason -> Error.fail ~where:(Printf.sprintf "0x%x" pc) "%s" reason
            in
            let c = if count = 1 then C.print_stmts stmts
              else Printf.sprintf "/* 0x%x */\n%s" pc (C.print_stmt (Block stmts)) in
            listing (pc + length) (remaining - 1) (c :: acc)
        in
        listing pc count [] |> List.iter print_string)
  in
  Cmd.v (Cmd.info "lower" ~doc:"Decode instructions and print their C statements.")
    Term.(const run $ pc $ hex $ count $ live_out)

let coverage =
  let config = Arg.(required & opt (some string) None & info [ "config" ] ~docv:"FILE") in
  let rom_dir = Arg.(required & opt (some string) None & info [ "rom-dir" ] ~docv:"DIR") in
  let run config_path rom_dir =
    report_error (fun () ->
        let config = Config.load config_path in
        let manifest = Rom_manifest.of_configs [config] in
        let rom = Rom_manifest.load_region manifest ~game:config.game.id Rom ~rom_dir in
        let exclusions = Exclusion.of_config ~image:rom config.exclusions.main in
        Coverage.unsupported ~rom ~exclusions |> Coverage.write stdout)
  in
  Cmd.v (Cmd.info "coverage" ~doc:"Count decoded ROM instructions the semantics cannot describe yet.")
    Term.(const run $ config $ rom_dir)

let () =
  exit
    (Cmd.eval'
       (Cmd.group (Cmd.info "f3recomp")
          [sprite_units; compile_roms; check_game; decisions; lower; coverage; enhancements; stub "emit"]))
