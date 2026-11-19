open Cmdliner

let stub name =
  let run () =
    prerr_endline "not implemented";
    2
  in
  Cmd.v (Cmd.info name) Term.(const run $ const ())

let sprite_units =
  let open F3recomp in
  let run config output_dir =
    try
      Config.load config |> Emit_units.of_config |> Emit_units.render
      |> Output.write ~dir:output_dir;
      0
    with
    | Error.Invalid { where; what } ->
        Printf.eprintf "%s: %s\n" where what;
        1
    | Sys_error message ->
        prerr_endline message;
        1
    | Unix.Unix_error (error, operation, path) ->
        Printf.eprintf "%s %s: %s\n" operation path (Unix.error_message error);
        1
  in
  let config =
    Arg.(required & opt (some string) None & info ["config"] ~docv:"FILE")
  in
  let output_dir =
    Arg.(required & opt (some string) None & info ["output-dir"] ~docv:"DIR")
  in
  Cmd.v (Cmd.info "sprite-units" ~doc:"Generate the sprite emit-unit headers.")
    Term.(const run $ config $ output_dir)

let () =
  let commands =
    [ "emit"; "compile-roms"; "enhancements"; "check-game";
      "lower"; "decisions" ]
  in
  exit (Cmd.eval' (Cmd.group (Cmd.info "f3recomp") (sprite_units :: List.map stub commands)))
