open F3recomp

let check_c expected expr = Alcotest.(check string) "C expression" expected (C.print_expr expr)

let expressions () =
  let open C in
  check_c "cpu->d[0x2u]" (Index (Field (Name "cpu", "d"), UInt 2));
  check_c "f(0xffffffffu, (uint32_t)x)" (Call ("f", [UInt (-1); Cast (U32, Name "x")]));
  check_c "!(a ? b : c)" (Not (Select (Name "a", Name "b", Name "c")))

let precedence () =
  let open C in
  let a = Name "a" and b = Name "b" and c = Name "c" in
  check_c "a - (b - c)" (Bin (Sub, a, Bin (Sub, b, c)));
  check_c "(a & b) == c" (Bin (Eq, Bin (And, a, b), c));
  check_c "a & (b == c)" (Bin (And, a, Bin (Eq, b, c)));
  check_c "(a & b) | c" (Bin (Or, Bin (And, a, b), c));
  check_c "(uint32_t)(a + b)" (Cast (U32, Bin (Add, a, b)));
  check_c "(a ? b : c) ? a : b ? c : a" (Select (Select (a, b, c), a, Select (b, c, a)))

let statements () =
  let open C in
  let body =
    [Decl (U32, "n", UInt 1); Goto "test"; Label "test";
     If (Name "n", [Assign (Name "n", UInt 2)], [Return]);
     Switch (Name "n", [2, [Chain "next"]], [Dispatch_pc; Return])]
  in
  let fn = { name = "entry"; body } in
  let expected = {|void entry(f3_cpu *cpu) {
    uint32_t n = 0x1u;
    goto test;
    test:;
    if (n) {
        n = 0x2u;
    } else {
        return;
    }
    switch (n) {
    case 0x2u:
        {
            F3_CHAIN(next, cpu);
        }
    default:
        {
            F3_DISPATCH_PC(cpu);
            return;
        }
    }
}
|} in
  Alcotest.(check string) "function" expected (print_func fn);
  Alcotest.(check string) "determinism" (print_func fn) (print_func fn)

let effects () =
  let open Sem in
  let check body ~reads ~writes ~boundary =
    let actual = Effects.of_sem (Body body) in
    let flags label expected actual =
      Alcotest.(check string) label (Ccr.to_string expected) (Ccr.to_string actual)
    in
    flags "reads" reads actual.reads;
    flags "must-write" writes actual.writes;
    Alcotest.(check bool) "boundary" boundary actual.boundary
  in
  let z = Ccr.singleton Z and x = Ccr.singleton X in
  let logic = Flags (Logic { result = Const 1; width = Byte }) in
  let exit = Goto (Const 0) in
  check [logic; Let (0, Flag Z); exit] ~reads:Ccr.none ~writes:Ccr.nzvc ~boundary:false;
  check [If (Cond Eq, [logic; exit], []); Set_ccr (Const 0); exit]
    ~reads:z ~writes:Ccr.nzvc ~boundary:false;
  check [If (Flag X, [Raise { vector = 7; return_pc = 2 }], []); Set_ccr (Const 0); exit]
    ~reads:Ccr.all ~writes:Ccr.none ~boundary:true;
  check [Helper (Btst { value = Flag X; bit = Const 0; bytes = Const 4 }); exit]
    ~reads:x ~writes:z ~boundary:false;
  check [Runtime (Set_sr (Const 0)); Stop 2]
    ~reads:Ccr.all ~writes:Ccr.all ~boundary:true;
  check [Let (0, Flag X); Goto (Temp 0)] ~reads:x ~writes:Ccr.none ~boundary:true;
  let nested = Body [logic; If (Supervisor, [exit], [exit])] in
  match Lower.entry nested ~live_out:z with
  | Error reason -> Alcotest.fail reason
  | Ok entry -> Alcotest.(check bool) "terminal successors keep Z live" true (entry.elided = Ccr.none)

let process command args =
  let channel = Unix.open_process_args_in command (Array.of_list (command :: args)) in
  let output = In_channel.input_all channel in
  Unix.close_process_in channel, output

let cli args = process "../../bin/main.exe" ("lower" :: "--pc" :: "0x1000" :: args)

let lower hex args =
  match cli ("--hex" :: hex :: args) with
  | Unix.WEXITED 0, output -> output
  | _ -> Alcotest.failf "lower failed for %s" hex

let rec repo_root dir =
  if Sys.file_exists (Filename.concat dir "include/f3rt/cpu_abi.h") then dir
  else
    let parent = Filename.dirname dir in
    if parent = dir then Alcotest.fail "cannot locate runtime headers";
    repo_root parent

let syntax () =
  let encodings =
    [ "move", "2200"; "moveq", "70ff"; "move_low_first", "2300"; "movea", "3040";
      "load_postinc", "101f"; "add", "d280"; "sub", "9280"; "cmp", "b280";
      "addx", "d380"; "addx_memory", "d388"; "and", "c280"; "or", "8280";
      "eor", "b181"; "bra", "6002"; "bcc", "6602"; "dbcc", "56c8fffe";
      "jsr", "4e97"; "rts", "4e75"; "trap", "4e42"; "trapv", "4e76";
      "trapcc", "57fc"; "illegal", "4afc"; "btst", "08000007";
      "read_ccr", "42c0"; "read_sr", "40c0"; "set_ccr", "44fc001f";
      "set_sr", "46fc2000"; "read_usp", "4e68"; "set_usp", "4e60";
      "reset", "4e70"; "stop", "4e722000"; "nop", "4e71" ]
  in
  let file, out = Filename.open_temp_file ~temp_dir:"." "lower-" ".c" in
  Fun.protect ~finally:(fun () -> close_out_noerr out; Sys.remove file) (fun () ->
      output_string out "#include <f3rt/cpu_abi.h>\n#include \"cpu_ops.h\"\n";
      List.iter (fun (name, hex) ->
          Printf.fprintf out "void test_%s(f3_cpu *cpu) {\n%s}\n" name (lower hex []))
        encodings;
      List.iter (fun live ->
          Printf.fprintf out "void test_add_%s(f3_cpu *cpu) {\n%s}\n" live
            (lower "d280" ["--live-out"; live]))
        ["NZVC"; "X"; "none"];
      close_out out;
      let root = repo_root (Sys.getcwd ()) in
      match process "cc" ["-std=gnu11"; "-Wall"; "-Wextra"; "-Werror"; "-fsyntax-only";
                          "-I"; Filename.concat root "include";
                          "-I"; Filename.concat root "recomp"; file] with
      | Unix.WEXITED 0, _ -> ()
      | _ -> Alcotest.fail "generated C failed syntax checking")

let listing () =
  let output = lower "70ff 4e71" ["--count"; "2"; "--live-out"; "none"] in
  let contains haystack needle =
    let rec at index =
      index + String.length needle <= String.length haystack
      && (String.sub haystack index (String.length needle) = needle || at (index + 1))
    in
    at 0
  in
  Alcotest.(check bool) "second PC" true (contains output "/* 0x1002 */");
  Alcotest.(check bool) "dead producer omitted" false (contains output "cc_op");
  Alcotest.(check bool) "X only clears pending flags" true
    (contains (lower "d280" ["--live-out"; "X"]) "cpu->cc_op = F3_CC_OP_NONE;");
  Alcotest.(check bool) "CALLM remains unsupported" true
    (match cli ["--hex"; "06d00000"] with Unix.WEXITED 1, "" -> true | _ -> false);
  Alcotest.(check bool) "truncated instruction rejected" true
    (match cli ["--hex"; "4e72"] with Unix.WEXITED 1, "" -> true | _ -> false)

let () =
  let case name test = Alcotest.test_case name `Quick test in
  Alcotest.run "semantic lowering"
    ["printer", [case "expressions" expressions; case "precedence" precedence;
                 case "statements and deterministic output" statements];
     "effects", [case "reads, writes, terminal edges and boundaries" effects];
     "CLI", [case "compile emitted C against the runtime ABI" syntax;
             case "listing, live-out and diagnostics" listing]]
