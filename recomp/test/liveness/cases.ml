open F3recomp

let abstract_flags = [Ccr.singleton X; Ccr.singleton Z]

let masks =
  let x = Ccr.singleton X and z = Ccr.singleton Z in
  [| Ccr.none; x; z; Ccr.union x z |]

let mask bits = masks.(bits)

let make_graph ~size ~observers ~successors ~boundary ~reads ~writes =
  let nodes = List.init size (fun pc ->
    let effects = Effects.make ~reads:(reads pc) ~writes:(writes pc) () in
    let boundaries = if boundary pc then [Cfg.Return] else [] in
    pc, { Cfg.pc; effects; succ = successors pc; boundaries })
    |> List.to_seq |> Pc.Map.of_seq in
  { Cfg.nodes; interrupt_observes = observers }

(* Enumerate simple paths for one flag, stopping at its first must-write. *)
let truly_live (cfg : Cfg.t) pc flag =
  let observes node = node.Cfg.boundaries <> [] || Ccr.subset flag cfg.interrupt_observes in
  let rec visit seen pc =
    let node = Pc.Map.find pc cfg.nodes in
    Ccr.subset flag node.effects.reads ||
    (not (Ccr.subset flag node.effects.writes) &&
     (observes node ||
      (not (Pc.Set.mem pc seen) && List.exists (visit (Pc.Set.add pc seen)) node.succ)))
  in
  let node = Pc.Map.find pc cfg.nodes in
  observes node || List.exists (visit (Pc.Set.singleton pc)) node.succ

let validate (cfg : Cfg.t) =
  let solution = Liveness.solve cfg in
  Result.is_ok (Liveness.check cfg solution) &&
  Pc.Map.for_all (fun pc (node : Cfg.node) ->
    let out = Pc.Map.find pc solution.live_out in
    List.for_all (fun flag ->
      let expected = truly_live cfg pc flag in
      expected = Ccr.subset flag out &&
      (not expected ||
       let out = Ccr.diff out flag in
       let mutated = {
         Liveness.live_out = Pc.Map.add pc out solution.live_out;
         live_in = Pc.Map.add pc (Effects.live_in node.effects ~live_out:out) solution.live_in;
       } in
       Result.is_error (Liveness.check cfg mutated)))
      abstract_flags)
    cfg.nodes

let exhaustive () =
  let full = Sys.getenv_opt "F3_LIVENESS_FULL" = Some "1" in
  for size = 0 to 3 do
    for edges = 0 to (1 lsl (size * size)) - 1 do
      let adjacency = Array.init size (fun pc ->
        List.init size Fun.id
        |> List.filter (fun target -> edges land (1 lsl (size * pc + target)) <> 0)) in
      for boundaries = 0 to (1 lsl size) - 1 do
        (* The default still crosses every graph with every boundary set.
           With three nodes it varies the effects at PCs 0 and 1. *)
        let varying = if full then size else min size 2 in
        for effects = 0 to (1 lsl (4 * varying)) - 1 do
          let cfg = make_graph ~size ~observers:Ccr.none
              ~successors:(Array.get adjacency)
              ~boundary:(fun pc -> boundaries land (1 lsl pc) <> 0)
              ~reads:(fun pc -> mask ((effects lsr (4 * pc)) land 3))
              ~writes:(fun pc -> mask ((effects lsr (4 * pc + 2)) land 3)) in
          if not (validate cfg) then
            Alcotest.failf "size=%d edges=%d boundaries=%d effects=%d" size edges boundaries effects
        done
      done
    done
  done

let random_graphs =
  let open QCheck in
  let gen =
    let open Gen in
    int_range 8 20 >>= fun size ->
    quad (array_size (return size) (int_bound ((1 lsl size) - 1)))
      (int_bound ((1 lsl size) - 1)) (array_size (return size) (int_bound 15)) (int_bound 3)
    >|= fun (edges, boundaries, effects, observers) ->
    make_graph ~size ~observers:(mask observers)
      ~successors:(fun pc -> List.init size Fun.id
        |> List.filter (fun target -> edges.(pc) land (1 lsl target) <> 0))
      ~boundary:(fun pc -> boundaries land (1 lsl pc) <> 0)
      ~reads:(fun pc -> mask (effects.(pc) land 3))
      ~writes:(fun pc -> mask (effects.(pc) lsr 2))
  in
  Test.make ~name:"8–20 node graphs with interrupt observers" ~count:1000 (make gen) validate

let decode pc hex =
  let bytes = String.init (String.length hex / 2)
      (fun i -> Char.chr (int_of_string ("0x" ^ String.sub hex (i * 2) 2))) in
  let image = { Image.base = pc; bytes } in
  let rec loop pc nodes =
    if Image.contains image pc then
      match Decode.at image pc with
      | Insn insn -> loop (pc + insn.length) (Pc.Map.add pc insn nodes)
      | Illegal _ -> Alcotest.failf "unexpected exception entry at %#x" pc
    else nodes
  in
  loop pc Pc.Map.empty

let graph ?(pins = []) chunks =
  let insns = List.fold_left (fun insns (pc, hex) ->
      Pc.Map.union (fun _ _ _ -> Alcotest.fail "test chunks overlap") insns (decode pc hex))
      Pc.Map.empty chunks in
  let native pc = Pc.Map.mem pc insns && not (List.mem pc pins) in
  let nodes = Pc.Map.mapi (fun pc insn ->
      let pins = if List.mem pc pins then [Pins.Unit_hook] else [] in
      Cfg.node ~native ~pins insn (Semantics.of_insn insn)) insns in
  insns, { Cfg.nodes; interrupt_observes = Ccr.none }

let check_live solution pc expected =
  Alcotest.(check string) (Printf.sprintf "live after %#x" pc) expected
    (Ccr.to_string (Liveness.live_out solution pc ~flags:`Lazy))

let check_code ~present code marker =
  let found = try ignore (Str.search_forward (Str.regexp_string marker) code 0); true
    with Not_found -> false in
  Alcotest.(check bool) marker present found

let lowered insns solution pc =
  let sem = Semantics.of_insn (Pc.Map.find pc insns) in
  match Lower.entry ~memory:Memory_path.Inline ~live_out:(Liveness.live_out solution pc ~flags:`Lazy) sem with
  | Ok entry -> C.print_stmts entry.stmts
  | Error why -> Alcotest.fail why

let instruction_cases =
  [ "overwrite elides producer", [0x400, "700172024e75"], [],
      [0x400, "X"; 0x402, "XNZVC"],
      [0x400, false, "cc_op"; 0x400, true, "cpu->d"; 0x402, true, "F3_CC_OP_LOGIC"];
    "conditional branch reads Z", [0x400, "b0816702760078004e75"], [],
      [0x400, "XZ"], [0x400, true, "F3_CC_OP_CMP"];
    "flagless move preserves Z", [0x400, "b08120406702760078004e75"], [],
      [0x400, "XZ"], [0x400, true, "F3_CC_OP_CMP"];
    "ADD keeps eager X and clears stale pending state", [0x400, "d2807400d98342c5"], [],
      [0x400, "X"], [0x400, false, "F3_CC_OP_ADD"; 0x400, true, "~F3_CCR_X";
                     0x400, true, "cpu->cc_op = F3_CC_OP_NONE;"];
    "compare preserves X", [0x400, "d240b443e3544e75"], [],
      [0x400, "X"; 0x402, "X"], [0x400, true, "~F3_CCR_X"; 0x402, false, "F3_CC_OP_CMP"];
    "logic preserves X", [0x400, "d2402602e3544e75"], [], [0x400, "X"], [];
    "RTS boundary", [0x400, "b0814e75"], [], [0x400, "XNZVC"], [];
    "computed JMP boundary", [0x400, "b0814ed0"], [], [0x400, "XNZVC"], [];
    "missing fallthrough", [0x400, "b081"], [], [0x400, "XNZVC"], [];
    "known BRA", [0x400, "b08160044e714e7176004e75"], [], [0x400, "X"], [];
    "missing BSR destination", [0x400, "b08161000100"], [], [0x400, "XNZVC"], [];
    "loop back edge", [0x400, "76006706b44160fa4e717a004e75"], [],
      [0x400, "XZ"; 0x404, "XZ"], [0x404, true, "F3_CC_OP_CMP"];
    "pin observes all", [0x400, "700172024e75"], [0x402], [0x400, "XNZVC"; 0x402, "XNZVC"], [] ]

let check_call () =
  List.iter (fun (callee, expected) ->
    let _, cfg = graph [0x400, "b081610000fa76004e75"; 0x4fe, callee] in
    Alcotest.(check (list int)) "direct successor only" [0x4fe] (Pc.Map.find 0x402 cfg.nodes).succ;
    check_live (Liveness.solve cfg) 0x400 expected)
    ["76004e75", "X"; "4e714e75", "XNZVC"]

let check_pending_state () =
  let open F3recomp_test.Asm in
  let check hex program =
    let _, cfg = graph [0x400, hex] in
    let solved = Liveness.solve cfg in
    let liveness _ pc = Liveness.live_out solved pc ~flags:`Lazy in
    match Refinement.Harness.run ~liveness ~seed:15 ~samples:64 ~base:0x400 program with
    | Passed _ -> ()
    | Mismatch why -> Alcotest.fail why
  in
  check "d2807400d98342c5"
    [add Long (d 0) (d 1); moveq 0 (d 2); insn (Addx (Long, d 3, d 4));
     insn (From_status (Ccr, d 5))];
  check "d240b443e3544e75"
    [add Word (d 0) (d 1); insn (Alu (Cmp, Word, d 3, d 2));
     insn (Shift (Roxl, Word, Constant 1, d 4)); insn Rts]

let check_merge () =
  let _, cfg = graph [0x400, "70004e71670272014e71"] in
  let solved = Liveness.solve cfg in
  check_live solved 0x400 "XNZVC";
  check_live solved 0x406 "XNZVC";
  Alcotest.(check (list int)) "both predecessors flow into shared tail"
    [0x404; 0x406]
    (Pc.Map.fold (fun pc (node : Cfg.node) acc ->
      if List.mem 0x408 node.succ then pc :: acc else acc) cfg.nodes [] |> List.rev)

let check_effects () =
  let cases =
    [ "7001", "", "NZVC"; "d240", "", "XNZVC"; "5240", "", "XNZVC"; "5288", "", "";
      "b041", "", "NZVC"; "d983", "XZ", "XNZVC"; "c101", "XZ", "XNZVC";
      "81410000", "", ""; "e348", "", "XNZVC"; "e3a8", "", "NZVC";
      "e358", "", "NZVC"; "e354", "X", "XNZVC"; "6702", "Z", "";
      "6e02", "NZV", ""; "56c8fffe", "Z", ""; "51c8fffe", "", "";
      "57c0", "Z", ""; "5dc0", "NV", ""; "42c2", "XNZVC", ""; "44c0", "", "XNZVC";
      "46c0", "XNZVC", ""; "2602", "", "NZVC"; "2040", "", "";
      "023c00ef", "XNZVC", "XNZVC"; "003c0010", "XNZVC", "XNZVC";
      "0a3c0010", "XNZVC", "XNZVC"; "80c1", "XNZV", "C";
      "4afc", "XNZVC", ""; "a000", "XNZVC", ""; "4e71", "", ""; "51fc", "", "" ] in
  List.iter (fun (hex, reads, writes) ->
    let bytes = String.init (String.length hex / 2) (fun i ->
      Char.chr (int_of_string ("0x" ^ String.sub hex (2 * i) 2))) in
    let pc = 0x400 in
    let effects =
      Decode.at { Image.base = pc; bytes } pc |> Semantics.of_decoded ~pc |> Effects.of_sem in
    Alcotest.(check (pair string string)) hex (reads, writes)
      (Ccr.to_string effects.reads, Ccr.to_string effects.writes)) cases

let check_all_producers () =
  List.iter (fun extension ->
    for opcode = 0 to 0xffff do
      let bytes = String.init 2 (fun i -> Char.chr ((opcode lsr (8 * (1 - i))) land 255))
        ^ extension in
      let pc = 0x1000 in
      let sem = Decode.at { Image.base = pc; bytes } pc |> Semantics.of_decoded ~pc in
      let lower live_out = Lower.entry ~memory:Memory_path.Inline sem ~live_out in
      match lower Ccr.all with
      | Ok eager when eager.counts.lazy_producers > 0 ->
          let effects = Effects.of_sem sem in
          if not (Ccr.subset Ccr.nzvc effects.writes) then
            Alcotest.failf "%04x: summary misses lazy producer writes" opcode;
          List.iter (fun live ->
            match lower live with
            | Ok entry ->
                if Ccr.inter live Ccr.nzvc = Ccr.none && entry.counts.lazy_producers <> 0 then
                  Alcotest.failf "%04x: dead lazy producer retained" opcode;
                if not (Ccr.subset (Ccr.singleton X) live) && entry.counts.eager_x_writes <> 0 then
                  Alcotest.failf "%04x: dead eager X retained" opcode
            | Error why -> Alcotest.fail why)
            [Ccr.none; Ccr.singleton X; Ccr.nzvc]
      | _ -> ()
    done)
    [String.make 10 '\000'; "\x0c\x3a\x00\x00\x00\x00\x80\x00\x12\x34\x56\x78"]

let check_cfg () =
  let config = Config.of_string ~path:"test.cfg" {|[game]
id = "fixture"
[rom]
size = 0x406
interleave = 1
[[rom.lanes]]
file = "program"
size = 0x406
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
interrupt_observes = ["Z"]
|} in
  let rom = { Image.base = 0; bytes = String.make 0x400 '\000' ^ "\x70\x00\x72\x00\x4e\x75" } in
  let program = Game.discover { Game.config; rom; exclusions = [] } in
  let selection = Program.select program ~profile:None in
  let semantics = Cfg.semantics selection in
  let build resets = Cfg.build selection semantics ~pins:(Pins.collect selection semantics resets) in
  let plain = build Pc.Set.empty in
  check_live (Liveness.solve plain) 0x400 "XZ";
  let cfg = build (Pc.Set.singleton 0x402) in
  Alcotest.(check bool) "sprite reset is not native" false (Cfg.native cfg 0x402);
  let node = Pc.Map.find 0x400 cfg.nodes in
  Alcotest.(check bool) "edge to pinned target leaves native code" true
    (List.mem (Cfg.Leaves_native 0x402) node.boundaries);
  Alcotest.(check bool) "CFG accepts correct edges" true (Result.is_ok (Cfg.check program cfg));
  let nodes = Pc.Map.add 0x400 { node with succ = [0x404] } cfg.nodes in
  Alcotest.(check bool) "CFG rejects a fabricated edge" true
    (Result.is_error (Cfg.check program { cfg with nodes }))

let check_irq () =
  let rom = { Image.base = 0; bytes = String.make 0x400 '\000' } in
  let check chunks =
    let insns, cfg = graph chunks in
    Irq_contract.analyse ~rom ~instruction:(fun pc -> Pc.Map.find_opt pc insns) cfg 0x400 in
  let proven chunks flags =
    match check chunks with
    | Proven actual -> Alcotest.(check string) "observed" flags (Ccr.to_string actual)
    | Unknown { why; _ } -> Alcotest.fail why in
  (* The return site reads Z. Only the callee's MOVEQ decides if Z is inherited. *)
  proven [0x400, "610000fe57c04e73"; 0x500, "70004e75"] "";
  proven [0x400, "610000fe57c04e73"; 0x500, "4e714e75"] "Z";
  proven [0x400, "4e73"] "";
  List.iter (fun hex ->
    match check [0x400, hex] with
    | Unknown { at = 0x400; _ } -> ()
    | _ -> Alcotest.failf "expected unknown at entry for %s" hex)
    ["4ed0"; "30174e73"]

let () =
  let case name = Alcotest.test_case name `Quick in
  Alcotest.run "CCR liveness"
    ["graphs", [case "all graphs through three nodes" exhaustive;
                case "selected CFG and certificate" check_cfg;
                QCheck_alcotest.to_alcotest random_graphs];
     "instructions",
       List.map (fun (name, chunks, pins, expected, code) -> case name (fun () ->
         let insns, cfg = graph ~pins chunks in
         let solution = Liveness.solve cfg in
         List.iter (fun (pc, flags) -> check_live solution pc flags) expected;
         List.iter (fun (pc, present, marker) ->
           check_code ~present (lowered insns solution pc) marker) code)) instruction_cases
       @ [case "known call enters callee" check_call;
          case "X survives pending flags in the runtime" check_pending_state;
          case "both branch predecessors supply the shared tail" check_merge;
          case "effects follow IR semantics" check_effects;
          case "producer effects and lowering over every opcode" check_all_producers];
     "interrupts", [case "direct calls, RTE and uncertain exits" check_irq]]
