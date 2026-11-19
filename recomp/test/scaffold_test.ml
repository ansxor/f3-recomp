let () =
  Alcotest.run "f3recomp"
    [ ("scaffold", [ Alcotest.test_case "runs" `Quick (fun () -> ()) ]) ]
