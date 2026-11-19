open F3recomp

let bounded () =
  let bytes = Bytes.make 0x500 '\x00' in
  let word pc value = Bytes.set_uint16_be bytes pc value in
  let pointer pc value = Bytes.set_int32_be bytes pc (Int32.of_int value) in
  List.iteri (fun offset value -> word (0x400 + offset * 2) value)
    [0x0c80; 0x0000; 0x0001; 0x6208; 0x4efb; 0x0d21; 0x0020];
  pointer 0x42a 0x460;
  pointer 0x42e 0x464;
  pointer 0x432 0x468;
  List.iter (fun pc -> word pc 0x4e75) [0x460; 0x464; 0x468];
  let rom : Image.t = { base = 0; bytes = Bytes.to_string bytes } in
  let space = Code_space.make ~rom ~exclusions:[] ~ram:[] in
  let table =
    match Decode.at rom 0x408 with
    | Insn insn ->
        (match Jump_table.scan space insn with
         | Some table -> table
         | None -> Alcotest.fail "expected a bounded table")
    | Illegal _ -> Alcotest.fail "expected indexed JMP"
  in
  Alcotest.(check (list int)) "only guarded slots" [0x460; 0x464] table.candidates;
  Alcotest.(check bool) "compare and inclusive bound" true
    (table.evidence = Bounded { compare_pc = 0x400; limit = 2 });
  Jump_table.check table space;
  Alcotest.check_raises "invalid bound rejected"
    (Error.Invalid { where = "jump table"; what = "invalid candidates or evidence at 0x408" })
    (fun () -> Jump_table.check { table with evidence = Bounded { compare_pc = 0x400; limit = 3 } } space)

let () =
  Alcotest.run "jump tables" ["evidence", [Alcotest.test_case "compare and branch" `Quick bounded]]
