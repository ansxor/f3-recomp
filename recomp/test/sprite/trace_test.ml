open F3recomp
open Sprite_groups

let rec repo_root directory =
  if Sys.file_exists (Filename.concat directory "games/landmakrj/config.toml") then directory
  else
    let parent = Filename.dirname directory in
    if parent = directory then Alcotest.fail "repository root unavailable";
    repo_root parent

let root = repo_root (Sys.getcwd ())
let config_path id = Filename.concat root ("games/" ^ id ^ "/config.toml")
let get_config id = Config.load (config_path id)
let rom_dir id =
  let root = Option.value (Sys.getenv_opt "F3_RECOMP_ROM_ROOT") ~default:(Filename.concat root "roms") in
  Filename.concat root id

let fixture hex =
  let code = String.init (String.length hex / 2) (fun index ->
    Char.chr (int_of_string ("0x" ^ String.sub hex (index * 2) 2))) in
  let entry = 0x400 in
  let rom = { Image.base = 0; bytes = String.make entry '\000' ^ code } in
  let rec decode pc decoded =
    if Image.contains rom pc then
      match Decode.at rom pc with
      | Decode.Insn insn -> decode (pc + insn.length) (Pc.Map.add pc (Decode.Insn insn) decoded)
      | _ -> Alcotest.failf "invalid fixture instruction at %#x" pc
    else decoded
  in
  let config = get_config "landmakrj" in
  let game = { Game_image.config; rom; exclusions = [] } in
  let functions = Pc.Set.singleton entry in
  let program = {
    Program.game; space = Code_space.of_game game ~ram:[]; decoded = decode entry Pc.Map.empty;
    functions; tables = Pc.Map.empty;
    seeds = { vectors = functions; configured = Pc.Set.empty; ram_entries = Pc.Set.empty; ram_exits = Pc.Set.empty };
  } in
  fun writers -> Sprite_trace.run program ~config ~writers

let check_writers expected groups =
  let actual =
    List.map (fun (w : writer) -> w.pc, (lane_bits w.lane, w.x_offset, w.y_offset)) groups.writers
  in
  let lane_offsets = Alcotest.(triple int int int) in
  Alcotest.(check (list (pair int lane_offsets))) "writer PCs and lanes" expected actual

let check_writer group ~base ~cursor ~class_id ~rigid pc =
  match writer group pc with
  | None -> Alcotest.failf "missing writer at %#x" pc
  | Some w ->
      Alcotest.(check (triple int int int)) "base, cursor, class" (base, cursor, class_id)
        (w.base, w.cursor_reg, w.group_class);
      Alcotest.(check bool) "rigid" rigid w.rigid

let check_string_contains text needle =
  let rec search i =
    i + String.length needle <= String.length text &&
    (String.sub text i (String.length needle) = needle || search (i + 1))
  in
  Alcotest.(check bool) needle true (search 0)

let branch_fixture () =
  let result = fixture "49f900600340425c425c322b000c7404080300006704d2426002924238c14e75" [] in
  check_writers [0x41c, (1, 0x0c, absent)] result;
  check_writer result ~base:11 ~cursor:12 ~class_id:1 ~rigid:true 0x41c;
  Alcotest.(check int) "resolved count" 1 (List.assoc "resolved_writers" result.stats);
  check_string_contains (String.concat "" (List.map snd (decision_rows result))) "0x00041c\tsprite-writer"

let forced_lane () =
  let trace = fixture "322b000c32c14e75" in
  check_writers [] (trace []);
  check_writers [0x404, (1, 0x0c, absent)] (trace [{ Sprite_trace.pc = 0x404; lane = Some X }])

let with_file text f =
  let path = Filename.temp_file ~temp_dir:"." "sprite-" ".json" in
  Fun.protect ~finally:(fun () -> Sys.remove path) (fun () ->
    Out_channel.with_open_bin path (fun out -> output_string out text);
    f path)

let seed_formats () =
  let load input = with_file input Sprite_trace.load_writer_seeds in
  let pcs seeds = List.map (fun (s : Sprite_trace.seed) -> s.pc) seeds in
  Alcotest.(check (list int)) "array PCs" [0x41c; 0x41e] (pcs (load "[1052, 1054]"));
  (match load {|{"writers": [{"pc": 1052, "lane": "xy"}]}|} with
   | [{ pc = 0x41c; lane = Some XY }] -> ()
   | _ -> Alcotest.fail "expected one XY seed");
  let rejected data message =
    match load data with
    | exception Error.Invalid { what; _ } -> check_string_contains what message
    | _ -> Alcotest.fail "invalid seed was accepted"
  in
  rejected "[1049]" "even";
  rejected {|{"writers": ["x"]}|} "invalid sprite writer"

let check_cpp_table result =
  let header = Filename.temp_file ~temp_dir:"." "sprite-groups-" ".hpp" in
  Fun.protect ~finally:(fun () -> Sys.remove header) (fun () ->
    Output.write_file header (render_hpp result);
    let assertions = Buffer.create 256 in
    let emit fmt = Printf.bprintf assertions fmt in
    emit "#include %S\nusing f3rt::sprite_groups::table;\n" header;
    emit "static_assert(table.writes.size() == %d);\n" (List.length result.writers);
    emit "static_assert(table.resets.size() == %d);\n" (List.length result.resets);
    List.iteri (fun i (w : writer) ->
      emit "constexpr auto w%d = table.writes[%d];\n" i i;
      emit "static_assert(w%d.pc == %d && w%d.base == %d && w%d.lane == %d && w%d.group_class == %d && w%d.shift == 8 && w%d.x_offset == %d && w%d.y_offset == %d);\n"
        i w.pc i w.base i (lane_bits w.lane) i w.group_class i i w.x_offset i w.y_offset) result.writers;
    List.iteri (fun i (r : classification) ->
      emit "constexpr auto r%d = table.resets[%d];\n" i i;
      emit "static_assert(r%d.pc == %d && r%d.base == %d && r%d.offset == %d);\n"
        i r.pc i r.base i r.offset) result.resets;
    with_file (Buffer.contents assertions)
      (fun path ->
        let compiler = Option.value (Sys.getenv_opt "CXX") ~default:"c++" in
        let include_path = Filename.concat root "include" in
        let args = [| compiler; "-std=c++20"; "-Wall"; "-Wextra"; "-Werror"; "-fsyntax-only";
                      "-I"; include_path; "-x"; "c++"; path |] in
        match Unix.create_process compiler args Unix.stdin Unix.stdout Unix.stderr with
        | exception Unix.Unix_error (Unix.ENOENT, _, _) -> Alcotest.skip ()
        | pid ->
            Alcotest.(check bool) "valid C++20 header" true (snd (Unix.waitpid [] pid) = Unix.WEXITED 0)))

let empty_header () =
  let groups = make ~game_id:"empty" ~instructions:0 ~analysed:0 ~dispatch_targets:0
    ~writers:[] ~unresolved:[] ~classifications:[] in
  check_cpp_table groups

let trace_rom id =
  lazy (
    let rom_dir = rom_dir id in
    if not (Sys.file_exists rom_dir && Sys.is_directory rom_dir) then Alcotest.skip ();
    let game = Game.load (config_path id) ~rom_dir in
    Sprite_trace.run (Game.discover game) ~config:game.config ~writers:[])

let landmakrj = trace_rom "landmakrj"
let commandw = trace_rom "commandw"

let check_classifications result expected =
  List.iter (fun (pc, offset, kind) ->
    let actual = List.find_opt (fun item -> item.pc = pc && item.offset = offset) result.classifications in
    Alcotest.(check bool) (Printf.sprintf "classification at %#x" pc) true
      (Option.map (fun item -> item.kind) actual = Some kind);
    Alcotest.(check bool) (Printf.sprintf "reset at %#x" pc) (kind = Reset)
      (List.exists (fun item -> item.pc = pc && item.offset = offset) result.resets)) expected

let check_coverage result =
  Alcotest.(check int) "unresolved writer count" (List.length result.unresolved)
    (List.assoc "unresolved_writers" result.stats)

let landmakrj_writers () =
  let result = Lazy.force landmakrj in
  let expected = [
    0xa8f5e, (1, 0x0c, 0xffff);
    0xa8f62, (2, 0xffff, 0x0e);
    0xa8f98, (1, 0x0c, 0xffff);
    0xa8f9e, (2, 0xffff, 0x0e);
    0xa8fb2, (1, 0x0c, 0xffff);
    0xa8fb4, (2, 0xffff, 0x0e);
    0xa8fc4, (1, 0x0c, 0xffff);
    0xa8fc6, (2, 0xffff, 0x0e);
    0xa9052, (1, 0x0c, 0xffff);
    0xa9056, (2, 0xffff, 0x0e);
    0xa9100, (1, 0x0c, 0xffff);
    0xa9102, (2, 0xffff, 0x0e);
    0xa9112, (1, 0x0c, 0xffff);
    0xa9114, (2, 0xffff, 0x0e);
    0xa9124, (1, 0x0c, 0xffff);
    0xa9126, (2, 0xffff, 0x0e);
    0xa9132, (1, 0x0c, 0xffff);
    0xa9134, (2, 0xffff, 0x0e);
    0xa91ea, (3, 0x0c, 0x0e);
    0xa926a, (3, 0x0c, 0x0e);
    0xa92e4, (3, 0x0c, 0x0e);
    0xa9368, (3, 0x0c, 0x0e);
  ] in
  check_writers expected result;
  List.iter (fun (pc, (_, x, y)) ->
    let rigid = not (List.mem pc [0xa91ea; 0xa926a; 0xa92e4; 0xa9368]) in
    check_writer result ~base:11 ~cursor:9 ~class_id:1 ~rigid pc;
    let w = Option.get (writer result pc) in
    let roots = List.filter_map (fun offset ->
      if offset = absent then None else Some Sprite_value.Field.{ base = 11; offset }) [x; y] in
    Alcotest.(check (list int)) "root offsets" (List.map (fun f -> f.Sprite_value.Field.offset) roots)
      (List.map (fun f -> f.Sprite_value.Field.offset) w.root_offsets);
    Alcotest.(check bool) "root base" true (List.for_all (fun f -> f.Sprite_value.Field.base = 11) w.root_offsets))
    expected

let commandw_writers () =
  let result = Lazy.force commandw in
  let expected = [
    0x9c90, (3, 0x44, 0x46);
    0x9e9a, (3, 0x06, 0x08);
    0xa0be, (3, 0x06, 0x08);
  ] in
  check_writers expected result;
  List.iter (fun (pc, _) ->
    check_writer result ~base:14 ~cursor:12 ~class_id:(if pc = 0x9c90 then 2 else 1) ~rigid:true pc)
    expected

let landmakrj_fields () =
  check_classifications (Lazy.force landmakrj) [
    0x95866, 0x0c, Reset; 0x95dac, 0x0c, Reset; 0x9550e, 0x0c, Reset;
    0xa04fa, 0x0c, Reset; 0x96a88, 0x0c, Continuation;
    0x9fade, 0x0e, Continuation; 0xa0200, 0x0c, Continuation;
  ]

let landmakrj_coverage () =
  let result = Lazy.force landmakrj in
  check_coverage result;
  Alcotest.(check int) "resolved" 22 (List.assoc "resolved_writers" result.stats);
  Alcotest.(check bool) "more candidates" true (List.assoc "x_y_writers" result.stats > 22);
  let unresolved pc = List.find (fun (w : unresolved) -> w.pc = pc) result.unresolved in
  ignore (unresolved 0x46a2);
  let stream = unresolved 0x469a in
  check_string_contains stream.reason "stream cursor";
  check_string_contains stream.destination "(a6)+"

let commandw_fields () =
  check_classifications (Lazy.force commandw) [
    0xa25a, 0x06, Continuation; 0xa2e8, 0x44, Continuation; 0xa2fe, 0x44, Reset;
    0xc7fe, 0x08, Continuation; 0xc818, 0x08, Continuation; 0xc830, 0x06, Continuation;
  ]

let commandw_coverage () =
  let result = Lazy.force commandw in
  check_coverage result;
  let w = List.find (fun (w : unresolved) -> w.pc = 0x9aaa) result.unresolved in
  check_string_contains w.reason "constant";
  Alcotest.(check int) "unresolved lane" 3 (lane_bits w.lane)

let compile_header () =
  let result = Lazy.force landmakrj in
  Alcotest.(check int) "writer count" 22 (List.length result.writers);
  check_cpp_table result

let commandw_output () =
  let result = Lazy.force commandw in
  with_file "" (fun header ->
    with_file "" (fun stats ->
      with_file "" (fun rows ->
        let cli = "../../bin/main.exe" in
        let args = [| cli; "decisions"; "--config"; config_path "commandw";
                      "--rom-dir"; rom_dir "commandw"; "--sprite-only";
                      "--sprite-header"; header; "--sprite-stats"; stats |] in
        let output = Unix.openfile rows [Unix.O_WRONLY; Unix.O_TRUNC] 0 in
        let pid = Fun.protect ~finally:(fun () -> Unix.close output) (fun () ->
          Unix.create_process cli args Unix.stdin output Unix.stderr) in
        Alcotest.(check bool) "decisions command" true (snd (Unix.waitpid [] pid) = Unix.WEXITED 0);
        let read path = In_channel.with_open_bin path In_channel.input_all in
        Alcotest.(check string) "emitted header" (render_hpp result) (read header);
        let decision_table = Decisions.header ^ "\n" ^ String.concat "" (List.map snd (decision_rows result)) in
        Alcotest.(check string) "sprite decision rows" decision_table (read rows);
        check_string_contains (read stats) "resolved_writers\t3\n")))

let () =
  let case name run = Alcotest.test_case name `Quick run in
  Alcotest.run "sprite trace" [
    "synthetic", [case "branch and cursor union" branch_fixture; case "forced lane" forced_lane;
                  case "writer seed JSON" seed_formats; case "empty header" empty_header];
    "landmakrj", [case "writer table and rigidity" landmakrj_writers; case "coverage" landmakrj_coverage;
                  case "continuity" landmakrj_fields; case "C++20 header" compile_header];
    "commandw", [case "writer table" commandw_writers; case "continuity" commandw_fields;
                 case "coverage" commandw_coverage; case "CLI output" commandw_output];
  ]
