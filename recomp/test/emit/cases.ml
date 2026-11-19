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

let irq () = with_dir (fun dir ->
  let bytes = Bytes.make 0x402 '\000' in
  List.iter (fun vector -> Bytes.set_int32_be bytes (4 * vector) 0x400l) [26; 27];
  Bytes.set_uint16_be bytes 0x400 0x4ed0;
  let bytes = Bytes.to_string bytes in
  let crc = Image.crc32 { base = 0; bytes } in
  let sha = Digestif.SHA1.(to_hex (digest_string bytes)) in
  Output.write_file (Filename.concat dir "program") bytes;
  let config = Filename.concat dir "config.toml" in
  let run cpu =
    Output.write_file config (Printf.sprintf {|[game]
id = "fixture"
[rom]
size = 0x402
interleave = 1
[[rom.lanes]]
file = "program"
size = 0x402
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
|} crc sha cpu);
    emit dir ["--config"; config] in
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

let () =
  Alcotest.run "emit" [
    "layout", [Alcotest.test_case "RAM certificate" `Quick rejects_ram];
    "CLI", [Alcotest.test_case "argument errors" `Quick arguments;
            Alcotest.test_case "interrupt contract" `Quick irq];
  ]
