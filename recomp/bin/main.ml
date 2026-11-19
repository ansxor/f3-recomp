open Cmdliner

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
  | F3recomp.Error.Invalid { where; what } ->
      prerr_endline (if where = "" then what else where ^ ": " ^ what);
      1
  | Sys_error message ->
      prerr_endline message;
      1

let check_game =
  let config = Arg.(required & pos 0 (some string) None & info [] ~docv:"CONFIG") in
  let id = Arg.(required & pos 1 (some string) None & info [] ~docv:"GAME") in
  let run path id = report_error (fun () -> F3recomp.Config.check_id ~path id) in
  Cmd.v (Cmd.info "check-game") Term.(const run $ config $ id)

let compile_roms =
  let open F3recomp in
  (* Cmdliner consumes the first config as the option value and the rest as positionals. *)
  let configs =
    Arg.(required & opt (some string) None
         & info ["configs"] ~docv:"CONFIG" ~doc:"Read CONFIG followed by any additional config paths.")
  in
  let more_configs = Arg.(value & pos_all string [] & info [] ~docv:"CONFIG") in
  let output = Arg.(required & opt (some string) None & info ["output"] ~docv:"PATH") in
  let game = Arg.(value & opt (some string) None & info ["game"] ~docv:"GAME") in
  let game_output =
    Arg.(value & opt (some string) None & info ["game-output"] ~docv:"PATH")
  in
  let write_if_changed path contents =
    let unchanged =
      Sys.file_exists path && not (Sys.is_directory path)
      && In_channel.with_open_bin path In_channel.input_all = contents
    in
    if not unchanged then (
      let ensure_directory dir =
        if not (Sys.is_directory dir) then
          Error.fail ~where:dir "expected a directory"
      in
      let rec mkdir dir =
        if Sys.file_exists dir then ensure_directory dir
        else (
          mkdir (Filename.dirname dir);
          try Sys.mkdir dir 0o777 with Sys_error _ as error ->
            if Sys.file_exists dir then ensure_directory dir else raise error)
      in
      mkdir (Filename.dirname path);
      Out_channel.with_open_bin path (fun file -> Out_channel.output_string file contents))
  in
  let run config more_configs output game game_output =
    report_error (fun () ->
        if Option.is_some game <> Option.is_some game_output then
          Error.fail "--game and --game-output go together";
        let headers =
          Config.load_all (config :: more_configs)
          |> Rom_manifest.of_configs
          |> Rom_manifest.render ?game
        in
        write_if_changed output headers.manifest;
        match game_output, headers.game_config with
        | Some path, Some contents -> write_if_changed path contents
        | _ -> ())
  in
  Cmd.v (Cmd.info "compile-roms")
    Term.(const run $ configs $ more_configs $ output $ game $ game_output)

let () =
  let commands =
    [ "emit"; "sprite-units"; "enhancements";
      "lower"; "decisions" ]
  in
  let commands = compile_roms :: check_game :: List.map stub commands in
  exit (Cmd.eval' (Cmd.group (Cmd.info "f3recomp") commands))
