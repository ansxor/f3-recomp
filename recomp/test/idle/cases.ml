open F3recomp

let chunks =
  [0x400, "60fe";
   0x410, "4a2d001056c8fffa4e75";
   0x420, "4a6d002066fa4e75";
   0x440, "4eb90000046060f8";
   0x460, "4eb804704e75";
   0x470, "202d00304c3c0800000000075a802b40003048404e75"]

let fixture chunks =
  let config = Config.of_string ~path:"idle.cfg" {|[game]
id = "fixture"
[rom]
size = 0x500
interleave = 1
[[rom.lanes]]
file = "program"
size = 0x500
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
|} in
  let rom = { Image.base = 0; bytes = String.make 0x500 '\000' } in
  let program = Game.discover { Game.config; rom; exclusions = [] } in
  let decoded = List.fold_left (fun decoded (base, hex) ->
    let bytes = String.init (String.length hex / 2) (fun i ->
      Char.chr (int_of_string ("0x" ^ String.sub hex (2 * i) 2))) in
    let image = { Image.base; bytes } in
    let rec decode pc decoded =
      if Image.contains image pc then
        match Decode.at image pc with
        | Insn insn as entry -> decode (pc + insn.length) (Pc.Map.add pc entry decoded)
        | Illegal _ -> Alcotest.failf "unexpected illegal instruction at %#x" pc
      else decoded
    in
    decode base decoded)
    Pc.Map.empty chunks in
  { program with decoded }

let analyse ?(pinned = []) chunks =
  let program = fixture chunks in
  let native pc = Pc.Map.mem pc program.decoded && not (List.mem pc pinned) in
  let nodes = Pc.Map.mapi (fun pc _ ->
    let insn = Option.get (Program.insn program pc) in
    let pins = if List.mem pc pinned then [Pins.Unit_hook] else [] in
    Cfg.node ~native ~pins insn (Semantics.of_insn insn)) program.decoded in
  let cfg = { Cfg.nodes; interrupt_observes = Ccr.none } in
  program, cfg, Idle_loop.find program ~native:(Cfg.native cfg)

let shapes () =
  let program, cfg, loops = analyse chunks in
  let names = Pc.Map.bindings loops |> List.map (fun (pc, (loop : Idle_loop.t)) ->
    pc, Idle_loop.to_tsv loop.shape) in
  Alcotest.(check (list (pair int string))) "heads and TSV shape names"
    [0x400, "spin"; 0x420, "poll"; 0x440, "affine"] names;
  let shape head = (Pc.Map.find head loops).shape in
  let address disp = Sem.Bin (Add, Reg (A 5), Const disp) in
  Alcotest.(check bool) "poll address, size and condition" true
    (shape 0x420 = Poll_bcc { address = address 32; size = Word; cond = Ne });
  Alcotest.(check bool) "affine word" true (shape 0x440 = Affine { word = address 48 });
  Alcotest.(check (list int)) "affine execution order across the nested call"
    [0x440; 0x460; 0x470; 0x474; 0x47c; 0x47e; 0x482; 0x484; 0x464; 0x446]
    (Pc.Map.find 0x440 loops).path;
  match Idle_loop.check_all program cfg loops with
  | Ok () -> ()
  | Error violations ->
      Alcotest.fail (String.concat "\n" (List.map Idle_loop.describe_violation violations))

let native () =
  let _, _, loops = analyse ~pinned:[0x47c] chunks in
  Alcotest.(check (list int)) "pinned affine instruction excludes the loop"
    [0x400; 0x420] (List.map fst (Pc.Map.bindings loops))

let rejects codes () =
  List.iter (fun code ->
    let _, _, loops = analyse [0x400, code] in
    Alcotest.(check int) code 0 (Pc.Map.cardinal loops)) codes

let check_rejected program cfg (loop : Idle_loop.t) rule =
  match Idle_loop.check_all program cfg (Pc.Map.singleton loop.Idle_loop.head loop) with
  | Error [violation] when violation.head = loop.head && violation.rule = rule -> ()
  | Error violations ->
      Alcotest.fail (String.concat "\n" (List.map Idle_loop.describe_violation violations))
  | Ok () -> Alcotest.fail "invalid certificate accepted"

let corrupted_path () =
  let program, cfg, loops = analyse chunks in
  let loop = Pc.Map.find 0x440 loops in
  check_rejected program cfg { loop with path = 0x440 :: 0x470 :: List.drop 2 loop.path } Cycle

let memory_write () =
  let _, _, loops = analyse chunks in
  let loop = Pc.Map.find 0x440 loops in
  (* ADDQ.L #5,D0 becomes ADDQ.L #5,(A0), preserving the path and length. *)
  let chunks = List.remove_assoc 0x470 chunks @
    [0x470, "202d00304c3c0800000000075a902b40003048404e75"] in
  let program, cfg, found = analyse chunks in
  Alcotest.(check bool) "detector rejects memory arithmetic" false (Pc.Map.mem loop.head found);
  check_rejected program cfg loop Body_effects

let () =
  let case name = Alcotest.test_case name `Quick in
  Alcotest.run "idle loops"
    ["recognition",
      [case "recognises spin, poll and affine" shapes;
       case "requires plain native code" native;
       case "rejects counted polls" (rejects ["4a2d001056c8fffa"; "4a2d001051c9fffa"]);
       case "rejects changing, indexed and PC-relative EAs"
         (rejects ["4a1d67fc"; "4a35000067fa"; "4a3a000267fa"])];
     "certificates",
      [case "rejects a corrupted path" corrupted_path;
       case "rejects arithmetic that writes memory" memory_write]]
