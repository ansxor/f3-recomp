open F3recomp

let rec root dir =
  if Sys.file_exists (Filename.concat dir "include/f3rt/cpu_abi.h") then dir
  else if Filename.dirname dir = dir then Alcotest.fail "Cannot locate the runtime headers"
  else root (Filename.dirname dir)

let with_dir test =
  let dir = Filename.temp_file "f3-generated" "" in
  Sys.remove dir;
  Unix.mkdir dir 0o700;
  let rec remove path =
    if Sys.is_directory path then begin
      Sys.readdir path |> Array.iter (fun name -> remove (Filename.concat path name));
      Unix.rmdir path
    end else Sys.remove path
  in
  Fun.protect ~finally:(fun () -> remove dir) (fun () -> test dir)

let run ?(input = Unix.stdin) ?(output = Unix.stdout) args =
  match args with
  | [] -> invalid_arg "empty command"
  | exe :: _ ->
      let pid = Unix.create_process exe (Array.of_list args) input output Unix.stderr in
      match snd (Unix.waitpid [] pid) with
      | Unix.WEXITED 0 -> ()
      | _ -> Alcotest.failf "Command failed: %s" (String.concat " " (List.map Filename.quote args))

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
[cpu]
interrupt_handlers_trusted = "synthetic fixtures do not deliver interrupts"
|}

let program ?(aligned = false) ?(tables = []) ?(exclusions = []) ?(units = []) chunks =
  let bytes = Bytes.make 0x1000 '\000' in
  let chunks = List.map (fun (start, hex) ->
    let size = String.length hex / 2 in
    for offset = 0 to size - 1 do
      let byte = int_of_string ("0x" ^ String.sub hex (offset * 2) 2) in
      Bytes.set_uint8 bytes (start + offset) byte
    done;
    start, start + size) chunks in
  let rom : Image.t = { base = 0; bytes = Bytes.to_string bytes } in
  let config = { config with video = { config.video with emit_units = units } } in
  let game : Game_image.t = { config; rom; exclusions } in
  let space = Code_space.of_game game ~ram:[] in
  let decoded = List.fold_left (fun decoded (start, stop) ->
    let rec decode entries pc =
      if pc >= stop then entries
      else
        let insn = Decode.at rom pc in
        decode (Pc.Map.add pc insn entries) (pc + if aligned then 2 else match insn with Insn i -> i.length | Illegal i -> i.length)
    in
    decode decoded start) Pc.Map.empty chunks in
  let tables = List.fold_left (fun tables (site, candidates) ->
    Pc.Map.add site { Jump_table.site; candidates; evidence = Configured } tables)
    Pc.Map.empty tables in
  let empty = Pc.Set.empty in
  { Program.game; space; decoded; functions = empty; tables;
    seeds = { vectors = empty; configured = empty; ram_entries = empty; ram_exits = empty } }

let profile dir program hits =
  let rom = program.Program.game.rom in
  let crc = Image.crc32 rom in
  let path = Filename.concat dir "fixture.profile" in
  Out_channel.with_open_text path (fun out ->
    Printf.fprintf out "F3-BLOCK-PROFILE 1\nrom main %08lx 00000000 %08x\n" crc (Image.length rom);
    List.iter (Printf.fprintf out "hit main %08lx %08x 1\n" crc) hits);
  Profile.load path

let execute ?(profile_mode = `Baseline) ?(hits = []) ?(check_layout = ignore) ?(instrument = false) ?(defines = [])
    ?(run_fixture = fun binary -> run [binary]) name program =
  with_dir (fun dir ->
    let request = match profile_mode with
      | `Baseline -> None
      | `Slim -> Some (`Slim (profile dir program hits))
      | `Tiers -> Some (`Tiers (profile dir program hits))
    in
    let selection = Program.select program ~profile:request in
    let analysis = Analysis.run ~parity:None ~allow_unproven:false ~writers:[] selection in
    let lowered = Analysis.lower ~flags:`Lazy analysis in
    let layout = Layout.make ~max_block:130 ~selection ~idle:analysis.idle lowered in
    check_layout layout;
    let sources = Analysis.sources analysis layout in
    Output.write ~dir (sources @ Emit_units.render selection.units);
    let root = root (Sys.getcwd ()) in
    let binary = Filename.concat dir "fixture" in
    let generated = List.filter_map (fun (name, _) ->
      if Filename.check_suffix name ".c" then Some (Filename.concat dir name) else None) sources in
    let compiler = Option.value (Sys.getenv_opt "F3_TEST_CC") ~default:"gcc" in
    let profile_defines = if instrument then
        ["-DF3_PROFILE_INSTRUMENT=1"; "-DSLIM=" ^ (if profile_mode = `Slim then "1" else "0")]
      else [] in
    run ([compiler; "-std=c11"; "-O2"; "-Wall"; "-Wextra"; "-Werror";
          "-I"; Filename.concat root "include"; "-I"; root; "-I"; dir;
          name ^ ".c"] @ defines @ profile_defines @ generated @ ["-o"; binary]);
    run_fixture binary)

let deadline () = execute "deadline" (program [0x400, "70ff42c3524042c4720142c5"])
let overlap () = execute "overlap" (program ~aligned:true [0x400, "203c700172024e717403"])
let cmpm () = execute "cmpm" (program [0x400, "b10a56c8fffcb14942c6b18842c6bf0f42c6"])
let loop = program [0x400, "70044e7151c8fffc7207"]
let backward () = execute "backward" loop
let resume () = execute "resume" loop
let caller = 0x400, "610000fe7409"
let callee = 0x500, "72074e71"
let cross () = execute "cross" (program [caller; callee; 0x600, "6000fefe"])
let unretained () =
  execute "unretained" (program [caller]);
  execute ~profile_mode:`Slim ~hits:[0x400] "unretained" (program [caller; callee])

let indexes = program ~tables:[0x402, [0x406; 0x500]; 0x604, [0x606; 0x500]]
  [0x3fe, "303b00f04efb00ec72214e714e71"; 0x600, "207b00084ed072214e71";
   0x500, "7427"]

let indexed () =
  let check_layout (layout : Layout.t) =
    let targets pc = (Pc.Map.find pc layout.transfers).indirect in
    Alcotest.(check (list int)) "PC-index cases" [0x406; 0x500] (targets 0x402);
    Alcotest.(check (list int)) "register-staged cases" [0x500; 0x606] (targets 0x604)
  in
  execute ~check_layout "indexed" indexes

let unknown () = execute "unknown" indexes

let idle_equivalence () =
  let program = program [
    0x400, "60fe"; 0x410, "4a2d001056c8fffa4e75"; 0x420, "4a6d002066fa4e75";
    0x440, "4eb90000046060f8"; 0x460, "4eb804704e75";
    0x470, "202d00304c3c0800000000075a802b40003048404e75";
  ] in
  with_dir (fun dir ->
    let stops = Filename.concat dir "stops" in
    let capture path ?(input = Unix.stdin) binary =
      let output = Unix.openfile path [O_WRONLY; O_CREAT; O_TRUNC] 0o600 in
      Fun.protect ~finally:(fun () -> Unix.close output)
        (fun () -> run ~input ~output [binary]) in
    let yielded = Filename.concat dir "yielded" in
    let plain = Filename.concat dir "plain" in
    let check_layout (layout : Layout.t) =
      Alcotest.(check (list (pair int string))) "idle shapes"
        [0x400, "spin"; 0x420, "poll"; 0x440, "affine"]
        (Pc.Map.bindings layout.idle |> List.map (fun (pc, loop) -> pc, Idle_loop.to_tsv loop.Idle_loop.shape))
    in
    execute ~check_layout ~defines:["-DYIELD=1"] ~run_fixture:(capture yielded ~input:Unix.stdin) "idle" program;
    let read path = In_channel.with_open_text path In_channel.input_all in
    let states = read yielded in
    let rounds = String.split_on_char '\n' states |> List.filter ((<>) "") in
    Alcotest.(check int) "completed comparisons" 6000 (List.length rounds);
    Output.write_file stops (String.concat "\n" (List.map (fun line ->
      String.sub line 0 (String.index line ' ')) rounds) ^ "\n");
    let input = Unix.openfile stops [O_RDONLY] 0 in
    Fun.protect ~finally:(fun () -> Unix.close input) (fun () ->
      execute ~instrument:true ~defines:["-DYIELD=0"] ~run_fixture:(capture plain ~input) "idle" program);
    Alcotest.(check bool) "idle and normal execution leave identical state" true (read plain = states))

let profile_cases () =
  let program = program [0x400, "700172027403"] in
  List.iter (fun mode ->
    execute ~profile_mode:mode ~hits:[0x400; 0x404] ~instrument:true "tiers" program)
    [`Baseline; `Tiers; `Slim]

let exceptions () =
  let program = program [0, "a000f0007100"] in
  List.iter (fun mode ->
    execute ~profile_mode:mode ~hits:[0; 4] ~instrument:true "exceptions" program)
    [`Baseline; `Tiers; `Slim]

let excluded () =
  let exclusion : Exclusion.t =
    { start = 4; stop = 6; reason = "fixture data"; evidence = "fixture"; fill = None } in
  let program = program ~exclusions:[exclusion] [0, "70017202"; 6, "a000a001f000"] in
  List.iter (fun mode ->
    let check_layout (layout : Layout.t) =
      let expected = if mode = `Slim then [0; 6] else [0; 2; 6; 8; 10] in
      Alcotest.(check (list int)) "retained entries" expected
        (List.map fst (Pc.Map.bindings layout.selection.entries));
      Alcotest.(check (list int)) "hot entries" [0; 6]
        (Pc.Map.bindings layout.selection.entries
         |> List.filter_map (fun (pc, (_, tier)) -> if tier = Selection.Hot then Some pc else None))
    in
    execute ~check_layout ~profile_mode:mode ~hits:[0; 6] ~instrument:true "excluded" program)
    [`Tiers; `Slim]

let hooks () =
  let units : Config.emit_unit list = [
    { id = 0; name = "objects"; starts = [0x402]; ends = [0x406]; reg = Address 1; size = 0 };
    { id = 1; name = "second"; starts = [0x406]; ends = [0x408]; reg = Data 1; size = 0 };
  ] in
  let code = [0x400, "70ff5240720174027603"] in
  execute ~instrument:true ~defines:["-DHAS_UNITS=1"] "hooks" (program ~units code);
  execute ~instrument:true ~defines:["-DHAS_UNITS=0"] "hooks" (program code)

let persistence () = with_dir (fun dir ->
  let root = root (Sys.getcwd ()) in
  let exe = Filename.concat dir "persistence" in
  let runtime = Filename.concat root "runtime" in
  run ["g++"; "-std=c++20"; "-O2"; "-Wall"; "-Wextra"; "-Werror";
       "-DF3_PROFILE_INSTRUMENT=1"; "-I"; Filename.concat root "include";
       "-I"; runtime; "persistence.cpp"; Filename.concat runtime "debug/profile.cpp"; "-o"; exe];
  run [exe; dir];
  Alcotest.(check bool) "no profile under the temporary cwd" false
    (Sys.file_exists (Filename.concat dir "moved/profiles/run.profile"));
  let profile = Profile.load (Filename.concat dir "profiles/run.profile") in
  let crc = Image.crc32 { base = 0; bytes = "\x4e\x71" } in
  let hits region pc count =
    Alcotest.(check (option int64)) "persisted hits" (Some count)
      (Profile.count profile Hit region ~crc ~pc)
  in
  hits Main 0 9L;
  hits Sound 0xc00000 4L)

let abi () = with_dir (fun dir ->
  let root = root (Sys.getcwd ()) in
  let header = Filename.concat dir "program.h" in
  Output.write_file header Program_c.header;
  (* Inclusion evaluates the generated ABI guard against the runtime macro. *)
  run ["gcc"; "-std=c11"; "-I"; Filename.concat root "include";
       "-fsyntax-only"; "-x"; "c"; header])

let () =
  let case name test = Alcotest.test_case name `Quick test in
  Alcotest.run "generated C" [
    "execution", [
      case "deadline flags and resumption" deadline;
      case "overlapping entries" overlap;
      case "CMPM operand order and aliasing" cmpm;
      case "backward loop and step count" backward;
      case "chain deadline and resumption" resume;
      case "cross-function transfer and trace" cross;
      case "unretained target returns" unretained;
      case "indexed and staged chains" indexed;
      case "unknown index and traced jump" unknown;
      case "idle yields preserve state at the same step" idle_equivalence;
    ];
    "profiling", [
      case "tier transitions, deadlines and saturation" profile_cases;
      case "shared exceptions count once" exceptions;
      case "excluded ranges and tier complements" excluded;
      case "persistence across cwd changes" persistence;
    ];
    "sprite units", [case "hook order and empty configuration" hooks];
    "ABI", [case "generated header matches runtime" abi];
  ]
