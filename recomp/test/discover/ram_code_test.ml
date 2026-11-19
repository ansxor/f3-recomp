open F3recomp

let destination = 0x400002
let spec : Config.ram_code =
  { destination; source = 2; length = 8; transform = Negate_be32; entry = destination }

(* JMP (A0), BRA.S to destination+8, RTS, and a truncated BRA.W. *)
let rom : Image.t = { base = 0; bytes = "\x00\x00\xb1\x2f\x9f\xfc\xb1\x8a\xa0\x00" }

let computed_entry () =
  let routines = Ram_code.decode [spec] rom in
  Ram_code.check [spec] rom routines;
  let routine = List.hd routines in
  Alcotest.(check string) "negated words" "\x4e\xd0\x60\x04\x4e\x75\x60\x00" routine.payload.bytes;
  Alcotest.(check (list int)) "all aligned PCs"
    [destination; destination + 2; destination + 4; destination + 6]
    (List.map fst (Pc.Map.bindings routine.decoded));
  begin match Pc.Map.find destination routine.decoded with
  | Decode.Insn insn ->
      begin match (Flow.of_insn insn).escape with
      | Some (Flow.Computed _) -> ()
      | _ -> Alcotest.fail "expected a computed jump"
      end
  | Decode.Illegal _ -> Alcotest.fail "expected a decoded jump"
  end;
  begin match Pc.Map.find (destination + 4) routine.decoded with
  | Decode.Insn { op = Insn.Rts; _ } -> ()
  | _ -> Alcotest.fail "expected RTS at the computed entry"
  end;
  begin match Pc.Map.find (destination + 6) routine.decoded with
  | Decode.Illegal { reason = Decode.Truncated; _ } -> ()
  | _ -> Alcotest.fail "expected a truncated branch at the payload boundary"
  end;
  Alcotest.(check (list int)) "outside direct target" [destination + 8]
    (Pc.Set.elements (Ram_code.exits routines))

let invalid_specs () =
  let rejects specs message =
    Alcotest.check_raises message
      (Error.Invalid { where = "RAM code"; what = message })
      (fun () -> ignore (Ram_code.decode specs rom))
  in
  let rejects_range (spec : Config.ram_code) =
    rejects [spec] (Printf.sprintf "invalid RAM code range at %#x or ROM source at %#x"
      spec.destination spec.source)
  in
  rejects [spec; { spec with destination = destination + 2; entry = destination + 2 }]
    (Printf.sprintf "overlapping RAM code at %#x" (destination + 2));
  List.iter rejects_range [
    { spec with source = 4 };
    { spec with source = -2 };
    { spec with source = 1 };
    { spec with length = 0 };
    { spec with length = 6 };
    { spec with destination = 0x43fffc; entry = 0x43fffc };
    { spec with destination = 0x500000; entry = 0x500000 };
    { spec with destination = destination - 1 };
    { spec with entry = destination + 1 };
    { spec with entry = destination + spec.length };
  ]

let regions () =
  let ram = Ram_code.decode [spec] rom in
  let exclusion : Exclusion.t =
    { start = 0; stop = 2; reason = "test header"; evidence = "fixture"; fill = Some 0 }
  in
  let space = Code_space.make ~rom ~exclusions:[exclusion] ~ram in
  let check_region label expected pc =
    Alcotest.(check bool) label true (Code_space.region space pc = expected)
  in
  check_region "excluded ROM" (Excluded exclusion) 0;
  check_region "ROM after exclusion" Rom 2;
  check_region "RAM payload" (Ram (List.hd ram)) (destination + 6);
  check_region "end of payload" Outside (destination + 8);
  Alcotest.(check (option string)) "ROM image" (Some rom.bytes)
    (Option.map (fun image -> image.Image.bytes) (Code_space.image_at space 2));
  Alcotest.(check (option int)) "RAM image base" (Some destination)
    (Option.map (fun image -> image.Image.base) (Code_space.image_at space destination));
  Alcotest.(check bool) "excluded and outside have no image" true
    (Code_space.image_at space 0 = None && Code_space.image_at space (Image.length rom) = None)

let dump config_path rom_dir =
  let config = Config.load config_path in
  let manifest = Rom_manifest.of_configs [config] in
  let rom = Rom_manifest.load_region manifest ~game:config.game.id Rom ~rom_dir in
  let routines = Ram_code.decode config.ram_code rom in
  Ram_code.check config.ram_code rom routines;
  List.iter (fun (routine : Ram_code.routine) ->
    Printf.printf "payload\t%d\t" routine.destination;
    String.iter (fun byte -> Printf.printf "%02x" (Char.code byte)) routine.payload.bytes;
    print_newline ();
    Pc.Map.iter (fun pc decoded ->
      let length = match decoded with
        | Decode.Insn insn -> insn.length
        | Decode.Illegal illegal -> illegal.length
      in
      Printf.printf "entry\t%d\t%d\n" pc length)
      routine.decoded)
    routines

let () =
  match Array.to_list Sys.argv with
  | [_; "--dump"; config_path; rom_dir] -> dump config_path rom_dir
  | _ ->
      Alcotest.run "RAM code" [
        "payload", [
          Alcotest.test_case "computed entry and exits" `Quick computed_entry;
          Alcotest.test_case "invalid specs" `Quick invalid_specs;
        ];
        "code space", [Alcotest.test_case "regions and images" `Quick regions];
      ]
