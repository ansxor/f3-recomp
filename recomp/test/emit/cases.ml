open F3recomp

let rejects_ram () =
  let is_ram pc = pc >= 0x400000 in
  let func pcs : Layout.func = { name = "test"; tier = Hot; pcs } in
  let check functions transfers = Layout.check ~is_ram ~functions ~transfers in
  let transfer = { Layout.empty_transfer with direct = [0x400000] } in
  let functions = [func [0x1000]; func [0x400000]] in
  Alcotest.(check bool) "chain to RAM" true
    (Result.is_error (check functions (Pc.Map.singleton 0x1000 transfer)));
  let switch = { Layout.empty_transfer with indirect = [0x400000] } in
  Alcotest.(check bool) "switch to RAM" true
    (Result.is_error (check functions (Pc.Map.singleton 0x1000 switch)));
  Alcotest.(check bool) "multiple RAM instructions" true
    (Result.is_error (check [func [0x400000; 0x400002]] Pc.Map.empty));
  Alcotest.(check bool) "single RAM instruction" true
    (Result.is_ok (check [func [0x400000]] Pc.Map.empty))

let with_dir test =
  let dir = Filename.temp_file "f3-emit" "" in
  Sys.remove dir;
  Unix.mkdir dir 0o700;
  let rec remove path =
    if Sys.is_directory path then begin
      Array.iter (fun name -> remove (Filename.concat path name)) (Sys.readdir path);
      Unix.rmdir path
    end else Sys.remove path in
  Fun.protect ~finally:(fun () -> remove dir) (fun () -> test dir)

let emit dir args =
  let exe = "../../bin/main.exe" in
  let args = [exe; "emit"; "--rom-dir"; dir; "--output"; Filename.concat dir "out"] @ args in
  let log = Filename.concat dir "log" in
  let fd = Unix.openfile log [O_WRONLY; O_CREAT; O_TRUNC] 0o600 in
  let pid = Fun.protect ~finally:(fun () -> Unix.close fd)
    (fun () -> Unix.create_process exe (Array.of_list args) Unix.stdin fd fd) in
  let _, status = Unix.waitpid [] pid in
  status, In_channel.with_open_bin log In_channel.input_all

let contains text needle =
  try ignore (Str.search_forward (Str.regexp_string needle) text 0); true with Not_found -> false

let expect_error (status, output) diagnostic =
  Alcotest.(check bool) "failed" true (status <> Unix.WEXITED 0);
  Alcotest.(check bool) output true (contains output diagnostic)

let arguments () = with_dir (fun dir ->
  expect_error (emit dir []) "--config";
  let config = ["--config"; Filename.concat dir "missing.toml"] in
  expect_error (emit dir config) "missing.toml";
  expect_error (emit dir (config @ ["--max-block-instructions"; "0"])) "must be positive";
  expect_error (emit dir (config @ ["--profile-tiers"; "tiers"; "--profile-slim"; "slim"]))
    "mutually exclusive")

let fixture dir instructions =
  let size = 0x400 + List.length instructions * 2 in
  let bytes = Bytes.make size '\000' in
  List.iter (fun vector -> Bytes.set_int32_be bytes (4 * vector) 0x400l) [26; 27];
  List.iteri (fun index opcode -> Bytes.set_uint16_be bytes (0x400 + index * 2) opcode)
    instructions;
  let bytes = Bytes.to_string bytes in
  let crc = Image.crc32 { base = 0; bytes } in
  let sha = Digestif.SHA1.(to_hex (digest_string bytes)) in
  Output.write_file (Filename.concat dir "program") bytes;
  let config = Filename.concat dir "config.toml" in
  fun cpu ->
    Output.write_file config (Printf.sprintf {|[game]
id = "fixture"
[rom]
size = %d
interleave = 1
[[rom.lanes]]
file = "program"
size = %d
crc = "%08lx"
sha1 = "%s"
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
%s
|} size size crc sha cpu);
    ["--config"; config]

let irq () = with_dir (fun dir ->
  let config = fixture dir [0x4ed0] in
  let run cpu = emit dir (config cpu) in
  let result = run "" in
  expect_error result "Unproven interrupt handlers";
  expect_error result "handler 0x400, site 0x400:";
  let status, output = run {|interrupt_handlers_trusted = "fixture evidence"|} in
  Alcotest.(check bool) output true (status = Unix.WEXITED 0);
  Alcotest.(check bool) "summary includes evidence" true (contains output "fixture evidence");
  expect_error (run {|interrupt_handlers_trusted = ""|}) "expected non-empty evidence";
  let status, output = run {|interrupt_observes = []|} in
  Alcotest.(check bool) output true (status = Unix.WEXITED 0);
  Alcotest.(check bool) "summary includes explicit empty set" true
    (contains output "interrupt_observes = none"))

let modes () = with_dir (fun dir ->
  let config = fixture dir [0x7000; 0x7200; 0x60fe] in
  let path = Filename.concat dir "decisions.tsv" in
  let write ~out ~idle =
    let padding = List.init 0x200 (fun word ->
      Printf.sprintf "0x%06x\tnative\t-\t-\t-\t-\tXNZVC\t-\t-\n" (2 * word)) in
    Output.write_file path (Decisions.header ^ "\n" ^ String.concat "" (padding @ [
      Printf.sprintf "0x000400\tnative\t2\tnext\t-\tNZVC\t%s\t-\t0x000402\n" out;
      "0x000402\tnative\t2\tnext\t-\tNZVC\t-\t-\t0x000404\n";
      Printf.sprintf "0x000404\tnative\t2\tbranch:0x000404\t-\t-\t-\t%s\t0x000404\n" idle;
      "0x000400\tsprite-reset\t-\t-\t-\t-\t-\t-\t-\n";
    ])) in
  let succeeds args diagnostics =
    let status, log = emit dir args in
    Alcotest.(check bool) log true
      (status = Unix.WEXITED 0 && List.for_all (contains log) diagnostics) in
  succeeds (config "" @ ["--flags"; "eager"]) ["Flag producers elided: 0 emitted"];
  write ~out:"XNZVC" ~idle:"spin";
  let parity = ["--parity-from"; path] in
  succeeds (config "" @ parity) ["Idle loops: 1 spin"; "Flag producers elided: 1 emitted"];
  write ~out:"-" ~idle:"-";
  succeeds (config "" @ parity) ["Idle loops: 0 spin"; "Flag producers elided: 2 emitted"];
  let observed = config {|interrupt_observes = ["Z"]|} in
  expect_error (emit dir (observed @ parity)) "missing boundary flags at 0x400";
  write ~out:"XNZVC" ~idle:"poll";
  expect_error (emit dir (config "" @ parity)) "at 0x404: imported poll, expected spin";
  expect_error (emit dir (config "" @ parity @ ["--flags"; "eager"])) "mutually exclusive")

let sprite_artifacts () = with_dir (fun dir ->
  let config = fixture dir [
    0x49f9; 0x0060; 0x0340; 0x425c; 0x425c; 0x322b; 0x000c; 0x7404;
    0x0803; 0x0000; 0x6704; 0xd242; 0x6002; 0x9242; 0x38c1; 0x4e75;
  ] in
  let status, output = emit dir (config {|interrupt_handlers_trusted = "synthetic handler"|}) in
  Alcotest.(check bool) output true (status = Unix.WEXITED 0);
  let out = Filename.concat dir "out" in
  Alcotest.(check bool) "sprite JSON written" true
    (Sys.file_exists (Filename.concat out "sprite_groups.json"));
  let source = Filename.concat dir "verify.cpp" in
  Output.write_file source {|
#include "sprite_groups.hpp"
using f3rt::sprite_groups::table;
static_assert(table.writes.size() == 1);
static_assert(table.writes[0].pc == 0x41c);
static_assert(table.writes[0].lane == 1 && table.writes[0].x_offset == 0x0c);
|};
  let rec includes path =
    let include_dir = Filename.concat path "include" in
    if Sys.file_exists (Filename.concat include_dir "f3rt/sprite_group.hpp") then include_dir
    else if Filename.dirname path = path then Alcotest.fail "Cannot locate runtime headers"
    else includes (Filename.dirname path)
  in
  let compiler = "g++" in
  let args = [|compiler; "-std=c++20"; "-fsyntax-only"; "-I"; includes (Sys.getcwd ());
               "-I"; out; source|] in
  let pid = Unix.create_process compiler args Unix.stdin Unix.stdout Unix.stderr in
  Alcotest.(check bool) "compiled sprite writer table" true
    (snd (Unix.waitpid [] pid) = Unix.WEXITED 0))

let () =
  Alcotest.run "emit" [
    "layout", [Alcotest.test_case "RAM certificate" `Quick rejects_ram];
    "CLI", [Alcotest.test_case "argument errors" `Quick arguments;
            Alcotest.test_case "interrupt contract" `Quick irq;
            Alcotest.test_case "eager flags and parity certificates" `Quick modes;
            Alcotest.test_case "sprite artifacts from emit" `Quick sprite_artifacts];
  ]
