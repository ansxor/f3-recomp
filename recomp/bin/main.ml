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
  let run config_path rom_dir =
    report_error (fun () ->
        Game.load config_path ~rom_dir
        |> Game.discover
        |> Decisions.write stdout)
  in
  Cmd.v (Cmd.info "decisions" ~doc:"Dump baseline discovery decisions.")
    Term.(const run $ config $ rom_dir)

let () =
  let stubs = List.map stub [ "emit"; "enhancements"; "lower" ] in
  exit
    (Cmd.eval'
       (Cmd.group (Cmd.info "f3recomp") (sprite_units :: compile_roms :: check_game :: decisions :: stubs)))
