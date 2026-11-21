open F3recomp

let lower sem ~memory =
  match Lower.entry sem ~memory ~live_out:Ccr.all with
  | Ok entry -> entry.stmts
  | Error reason -> Alcotest.fail reason

let memory_paths () =
  let open Sem in
  let addr = Const 0x400000 in
  let sem = Body [
      Load (0, Byte, addr); Load (1, Word, addr); Load (2, Long, addr);
      Store (Byte, addr, Temp 0); Store (Word, addr, Temp 1); Store (Long, addr, Temp 2);
      Store_long_low_first (addr, Temp 2);
    ] in
  let groups = { Sprite_groups.empty with reset_pcs = Pc.Set.singleton 0x1000 } in
  let calls pc =
    lower sem ~memory:(Memory_path.at groups pc)
    |> List.map (function
      | C.Decl (_, _, Call (name, _)) | Expr (Call (name, _)) -> name
      | _ -> Alcotest.fail "expected a memory access")
  in
  let reads = ["f3_ld8"; "f3_ld16"; "f3_ld32"] in
  Alcotest.(check (list string)) "inline" (reads @
    ["f3_st8"; "f3_st16"; "f3_st32"; "f3_st16"; "f3_st16"]) (calls 0x1002);
  Alcotest.(check (list string)) "sprite reset" (reads @
    ["f3_write8"; "f3_write16"; "f3_write32"; "f3_write16"; "f3_write16"]) (calls 0x1000)

let input sem live_out = { Lowered.sem; live_out; memory = Inline }

let check_elision () =
  let open Sem in
  let pc = 0x1234 in
  let logic = Flags (Logic { result = Const 1; width = Byte }) in
  let live_out = Ccr.singleton Z in
  (* The later CCR write leaves the producer dead locally, but the checker
     rejects any recorded elision that intersects the supplied live-out. *)
  let lowered =
    Pc.Map.singleton pc (input (Body [logic; Set_ccr (Const 0)]) live_out)
    |> Lowered.make
  in
  match Lowered.check lowered with
  | Error (Live_flags_elided { pc = actual_pc; flags }) ->
      Alcotest.(check int) "PC" pc actual_pc;
      Alcotest.(check string) "overlap" "Z" (Ccr.to_string flags)
  | Error _ | Ok () -> Alcotest.fail "expected a live flag elision"

let summary () =
  let open Sem in
  let producer = Add { src = Const 1; dst = Const 2; result = Const 3; width = Byte } in
  let native = Body [If (Supervisor, [Flags producer], [])] in
  let lowered =
    Pc.Map.of_list [0, input native Ccr.all; 2, input (Unsupported "test") Ccr.all]
    |> Lowered.make
  in
  let counts = Lowered.counts lowered in
  Alcotest.(check bool) "valid elisions" true (Lowered.check lowered = Ok ());
  Alcotest.(check int) "lazy producers" 1 counts.flags.lazy_producers;
  Alcotest.(check int) "eager X writes" 1 counts.flags.eager_x_writes;
  Alcotest.(check int) "fallbacks" 1 counts.fallbacks

let exception_body () =
  let pc = 0x1000 in
  let sem = Sem.Body [Raise { vector = 32; return_pc = 0x1002 }] in
  let lowered = Pc.Map.singleton pc (input sem Ccr.all) |> Lowered.make in
  match Pc.Map.find pc (Lowered.bodies lowered) with
  | Native { stmts = [Expr (Call ("f3_cc_flush", _));
                      Expr (Call ("f3_exception", [Name "cpu"; UInt 32; UInt 0x1002]));
                      Return]; _ } -> ()
  | _ -> Alcotest.fail "expected the exception call with its return PC"

let extend_flushes () =
  List.iter (fun (name, opcode) ->
      let pc = 0x1000 in
      let image = { Image.base = pc; bytes = opcode } in
      let sem = Decode.at image pc |> Semantics.of_decoded ~pc in
      let flushes = lower sem ~memory:Inline |> List.filter (function
          | C.Expr (Call ("f3_cc_flush", _)) -> true
          | _ -> false) in
      Alcotest.(check int) name 1 (List.length flushes))
    ["ADDX register", "\xd3\x80"; "ADDX memory", "\xd3\x88";
     "SUBX", "\x93\x80"; "NEGX", "\x40\x80"]

let () =
  let case name test = Alcotest.test_case name `Quick test in
  Alcotest.run "T14 lowering"
    ["lowering", [case "memory paths" memory_paths;
                  case "live elision diagnostic" check_elision;
                  case "summary counts" summary;
                  case "native exception body" exception_body;
                  case "extend arithmetic flushes once" extend_flushes]]
