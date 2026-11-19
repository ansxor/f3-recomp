open F3recomp

let config = Config.of_string ~path:"fixture/config.toml" {|
[game]
id = "fixture"
[rom]
size = 0x1000
interleave = 1
[[rom.lanes]]
file = "program"
size = 0x1000
crc = "12345678"
sha1 = "0123456789abcdef0123456789abcdef01234567"
[sprites]
size = 0
[sprites_hi]
size = 0
[tiles]
size = 0
[tiles_hi]
size = 0
[sound]
size = 0
[samples]
size = 0
|}

let program ?(units = []) ?(exclusions = []) ?(ram = []) bytes =
  let config = { config with video = { config.video with emit_units = units } } in
  let rom : Image.t = { base = 0; bytes } in
  let game : Game_image.t = { config; rom; exclusions } in
  let space = Code_space.of_game game ~ram in
  let rec decode pc entries =
    if not (Image.contains rom pc) then entries
    else match Code_space.region space pc with
      | Rom -> decode (pc + 2) (Pc.Map.add pc (Decode.at rom pc) entries)
      | _ -> decode (pc + 2) entries
  in
  let decoded = List.fold_left (fun decoded routine ->
    Pc.Map.union (fun _ _ _ -> Alcotest.fail "overlapping fixture") decoded routine.Ram_code.decoded)
    (decode 0 Pc.Map.empty) ram in
  let empty = Pc.Set.empty in
  { Program.game; space; decoded; functions = empty; tables = Pc.Map.empty;
    seeds = { vectors = empty; configured = empty; ram_entries = empty; ram_exits = empty } }

let profile ?(misses = []) program hits =
  let rom = program.Program.game.rom in
  let crc = Image.crc32 rom in
  let path = Filename.temp_file "f3-selection" ".profile" in
  Fun.protect ~finally:(fun () -> Sys.remove path) (fun () ->
    Out_channel.with_open_text path (fun out ->
      Printf.fprintf out "F3-BLOCK-PROFILE 1\nrom main %08lx 00000000 %08x\n" crc (Image.length rom);
      let write kind = List.iter (Printf.fprintf out "%s main %08lx %08x 1\n" kind crc) in
      write "hit" hits;
      write "miss" misses);
    Profile.load path)

let check_valid selection =
  match Selection.check selection with
  | Ok () -> ()
  | Error violations ->
      Alcotest.fail (String.concat "\n" (List.map Selection.describe_violation violations))

let tier selection pc = Option.map snd (Pc.Map.find_opt pc selection.Selection.entries)
let check_tier label expected selection pc =
  Alcotest.(check bool) label true (tier selection pc = expected)

let cross_tier () =
  let program = program "\x70\x01\x72\x02\x74\x03" in
  let profile = profile program [0; 4] in
  let insn = Option.get (Program.insn program 0) in
  List.iter (fun (request, cold, candidates) ->
    let selected = Program.select program ~profile:request in
    check_valid selected;
    check_tier "first hot" (Some Selection.Hot) selected 0;
    check_tier "middle" cold selected 2;
    check_tier "last hot" (Some Selection.Hot) selected 4;
    Alcotest.(check (list int)) "chain to retained successor" candidates
      (Selection.chain_candidates selected insn (Flow.of_insn insn)))
    [None, Some Selection.Hot, [2]; Some (`Tiers profile), Some Cold, [2]; Some (`Slim profile), None, []]

let indirect () =
  let targets = List.init 258 (fun index -> 2 + 2 * index) in
  let program = program ("\x4e\xd0" ^ String.concat "" (List.map (fun _ -> "\x4e\x71") targets)) in
  let table : Jump_table.t = { site = 0; candidates = targets; evidence = Configured } in
  let program = { program with tables = Pc.Map.singleton 0 table } in
  let hits = profile program (0 :: List.drop 2 targets) in
  let insn = Option.get (Program.insn program 0) in
  let candidates mode =
    let selection = Program.select program ~profile:(Some (mode hits)) in
    Selection.chain_candidates selection insn (Flow.of_insn insn)
  in
  Alcotest.(check (list int)) "tiers retains the first 256 indirect targets"
    (List.take 256 targets) (candidates (fun profile -> `Tiers profile));
  Alcotest.(check (list int)) "slim filters before applying the cap"
    (List.drop 2 targets) (candidates (fun profile -> `Slim profile))

let exceptions () =
  let exclusion : Exclusion.t =
    { start = 4; stop = 6; reason = "fixture data"; evidence = "fixture"; fill = None } in
  let destination = 0x400000 in
  let spec : Config.ram_code =
    { destination; source = 0; length = 4; transform = Negate_be32; entry = destination } in
  let ram = Ram_code.decode [spec] { base = 0; bytes = "\xb1\x8a\x60\x00" } in
  let program = program ~exclusions:[exclusion] ~ram
    "\x70\x01\x72\x02\xa0\x00\xa0\x00\x71\x00\xf0\x00" in
  let profile = profile program [0; 6; 8] in
  let check_vector selected pc vector =
    match Pc.Map.find_opt pc selected.Selection.entries with
    | Some (Exception_entry actual, _) -> Alcotest.(check int) "vector" vector actual
    | _ -> Alcotest.failf "expected ROM exception at %#x" pc
  in
  List.iter (fun (request, count, f_line) ->
    let selected = Program.select program ~profile:request in
    check_valid selected;
    Alcotest.(check int) "retained entries including RAM" count (Pc.Map.cardinal selected.entries);
    check_vector selected 6 10;
    check_vector selected 8 4;
    Option.iter (fun tier -> check_vector selected 10 11; check_tier "F-line tier" (Some tier) selected 10) f_line;
    check_tier "excluded" None selected 4;
    check_tier "RAM instruction always hot" (Some Selection.Hot) selected destination;
    check_tier "RAM exception always hot" (Some Selection.Hot) selected (destination + 2))
    [None, 7, Some Selection.Hot; Some (`Tiers profile), 7, Some Cold; Some (`Slim profile), 5, None];
  let bad = { program with decoded = Pc.Map.add 4 (Decode.at program.game.rom 4) program.decoded } in
  let bad = Program.select bad ~profile:None in
  Alcotest.(check bool) "excluded discovery entry rejected" true
    (Selection.check bad = Error [Inaccessible_entry 4])

let hooks () =
  let unit : Config.emit_unit = { id = 0; name = "fixture"; starts = [0]; ends = [2]; reg = Data 0; size = 1 } in
  let program = program ~units:[unit] "\x70\x01\x72\x02\xa0\x00" in
  let hits = profile program [0] in
  let select request = Program.select program ~profile:(Some (request hits)) in
  let tiers = select (fun hits -> `Tiers hits) in
  check_valid tiers;
  let starts, ends = Emit_units.hooks tiers.units in
  Alcotest.(check (list int)) "start hook" [0] (Pc.Map.find 0 starts);
  Alcotest.(check (list int)) "cold end hook" [0] (Pc.Map.find 2 ends);
  let slim = select (fun hits -> `Slim hits) in
  Alcotest.(check bool) "slim drops end hook" true
    (Selection.check slim = Error [Dropped_hook { unit_name = "fixture"; kind = "end"; pc = 2 }]);
  let retained = Program.select program ~profile:(Some (`Slim (profile program [0; 2]))) in
  check_valid retained;
  let missing = Program.select program ~profile:(Some (`Tiers (profile ~misses:[0] program []))) in
  Alcotest.(check bool) "miss-only profile rejected" true
    (Selection.check missing = Error [No_executable_hits]);
  let bad = { program with decoded = Pc.Map.remove 2 program.decoded } in
  Alcotest.(check bool) "undecoded hook rejected" true
    (Selection.check (Program.select bad ~profile:None) =
     Error [Undecoded_hook { unit_name = "fixture"; kind = "end"; pc = 2 }])

let () =
  Alcotest.run "profile selection" ["selection", [
    Alcotest.test_case "cross-tier and slim chains" `Quick cross_tier;
    Alcotest.test_case "indirect cap after retention" `Quick indirect;
    Alcotest.test_case "exception subsets, exclusions and RAM" `Quick exceptions;
    Alcotest.test_case "required unit hooks and executable hits" `Quick hooks;
  ]]
