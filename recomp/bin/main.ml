open Cmdliner

let stub name =
  let run () =
    prerr_endline "not implemented";
    2
  in
  Cmd.v (Cmd.info name) Term.(const run $ const ())

let () =
  let commands =
    [ "emit"; "compile-roms"; "sprite-units"; "enhancements"; "check-game";
      "lower"; "decisions" ]
  in
  exit (Cmd.eval' (Cmd.group (Cmd.info "f3recomp") (List.map stub commands)))
