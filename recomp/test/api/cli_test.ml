open F3recomp
open Json

let binary = "../../bin/main.exe"

(* ---- running the executable ---- *)

let write_file path contents =
  Out_channel.with_open_bin path (fun out -> output_string out contents)

let read_file path = In_channel.with_open_bin path In_channel.input_all

let scratch = lazy (
  let dir = Filename.temp_file ~temp_dir:(Sys.getcwd ()) "cli" "" in
  Sys.remove dir;
  Sys.mkdir dir 0o700;
  at_exit (fun () ->
    Array.iter (fun file -> try Sys.remove (Filename.concat dir file) with Sys_error _ -> ())
      (Sys.readdir dir);
    try Sys.rmdir dir with Sys_error _ -> ());
  dir)

let scratch_file name = Filename.concat (Lazy.force scratch) name

type outcome = { status : int; out : string; err : string }

let run ?(input = "") args =
  let stdin_path = scratch_file "stdin" and out_path = scratch_file "stdout"
  and err_path = scratch_file "stderr" in
  write_file stdin_path input;
  let open_for flags path = Unix.openfile path flags 0o600 in
  let stdin_fd = open_for [O_RDONLY] stdin_path in
  let out_fd = open_for [O_WRONLY; O_CREAT; O_TRUNC] out_path in
  let err_fd = open_for [O_WRONLY; O_CREAT; O_TRUNC] err_path in
  let pid = Unix.create_process binary (Array.of_list (binary :: args)) stdin_fd out_fd err_fd in
  List.iter Unix.close [stdin_fd; out_fd; err_fd];
  let _, status = Unix.waitpid [] pid in
  let status = match status with Unix.WEXITED code -> code | _ -> -1 in
  { status; out = read_file out_path; err = read_file err_path }

let succeed ?input args =
  let outcome = run ?input args in
  if outcome.status <> 0 then
    Alcotest.failf "%s failed (%d): %s" (String.concat " " args) outcome.status outcome.err;
  outcome.out

let fails ?(containing = "") args =
  let outcome = run args in
  Alcotest.(check int) "exit status" 1 outcome.status;
  Alcotest.(check string) "stdout is empty" "" outcome.out;
  let contains =
    containing = ""
    || (try ignore (Str.search_forward (Str.regexp_string containing) outcome.err 0); true
        with Not_found -> false)
  in
  if not contains then Alcotest.failf "expected %S in %S" containing outcome.err

let json_of text =
  match Json.parse text with
  | Ok json -> json
  | Error message -> Alcotest.failf "invalid JSON (%s): %s" message text

let json_out args = json_of (succeed args)

(* ---- JSON navigation ---- *)

let field key json =
  match Json.member key json with
  | Some value -> value
  | None -> Alcotest.failf "missing %S in %s" key (Json.to_string json)

let items = function Json.Array items -> items | json -> Alcotest.failf "not an array: %s" (Json.to_string json)
let int = function Json.Int n -> n | json -> Alcotest.failf "not an int: %s" (Json.to_string json)
let str = function Json.String s -> s | json -> Alcotest.failf "not a string: %s" (Json.to_string json)
let ints json = List.map int (items json)
let strs json = List.map str (items json)
let check_ints = Alcotest.(check (list int))

(* ---- the fixture game ---- *)

let words list =
  let bytes = Bytes.create (2 * List.length list) in
  List.iteri (fun i word -> Bytes.set_uint16_be bytes (2 * i) word) list;
  Bytes.to_string bytes

let rom =
  let bytes = Bytes.make 0x1000 '\000' in
  let put offset data = Bytes.blit_string data 0 bytes offset (String.length data) in
  Bytes.set_int32_be bytes 0 0x410000l;
  Bytes.set_int32_be bytes 4 0x400l;
  Bytes.set_int32_be bytes (4 * 26) 0x500l;
  Bytes.set_int32_be bytes (4 * 27) 0x500l;
  (* reset: lea $410000,a5; bsr.w $420; move.w #1,$4a0000; move.w $410010,d0; jsr $430; rts *)
  put 0x400 (words [0x4bf9; 0x0041; 0x0000; 0x6100; 0x0018; 0x33fc; 0x0001; 0x004a; 0x0000;
                    0x3039; 0x0041; 0x0010; 0x4eb9; 0x0000; 0x0430; 0x4e75]);
  (* move.w #$ff,$10(a5); tst.w $10(a5); rts *)
  put 0x420 (words [0x3b7c; 0x00ff; 0x0010; 0x4a6d; 0x0010; 0x4e75]);
  (* movea.l $20(a5),a0; jsr (a0); rts *)
  put 0x430 (words [0x206d; 0x0020; 0x4e90; 0x4e75]);
  (* the interrupt handler *)
  put 0x500 (words [0x4e73]);
  Bytes.to_string bytes

let rom_image = { Image.base = 0; bytes = rom }
let rom_crc = Image.crc32 rom_image
let sha256 = Digestif.SHA256.(to_hex (digest_string rom))

let fixture = lazy (
  let dir = Lazy.force scratch in
  write_file (Filename.concat dir "main") rom;
  let config = Filename.concat dir "config.toml" in
  write_file config (Printf.sprintf {|[game]
id = "fixture"
[rom]
size = 0x1000
interleave = 1
[[rom.lanes]]
file = "main"
size = 0x1000
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
|} rom_crc (Digestif.SHA1.(to_hex (digest_string rom))));
  ["--config"; config; "--rom-dir"; dir])

let game () = Lazy.force fixture

let profile_text ~hits =
  Printf.sprintf "F3-BLOCK-PROFILE 1\nrom main %08lx 00000000 00001000\n%s" rom_crc
    (String.concat "" (List.map (fun (pc, count) ->
       Printf.sprintf "hit main %08lx %08x %d\n" rom_crc pc count) hits))

let profile_file name ~hits =
  let path = scratch_file name in
  write_file path (profile_text ~hits);
  path

(* ---- disasm ---- *)

let disasm_text () =
  let out = succeed (["disasm"] @ game () @ ["--pc"; "0x400"; "--count"; "3"]) in
  Alcotest.(check string) "listing"
    ("000400:  4bf9 0041 0000           lea $410000,a5\n"
     ^ "000406:  6100 0018                bsr $000420\n"
     ^ "00040a:  33fc 0001 004a 0000      move.w #1,$4a0000\n")
    out

let disasm_json () =
  let lines = items (json_out (["disasm"] @ game () @ ["--pc"; "400"; "--to"; "0x420"; "--json"])) in
  Alcotest.(check (list string)) "text"
    ["lea $410000,a5"; "bsr $000420"; "move.w #1,$4a0000"; "move.w $410010,d0";
     "jsr $000430"; "rts"]
    (List.map (fun line -> str (field "text" line)) lines);
  check_ints "pcs" [0x400; 0x406; 0x40a; 0x412; 0x418; 0x41e] (List.map (fun l -> int (field "pc" l)) lines);
  let bsr = List.nth lines 1 in
  Alcotest.(check string) "bytes" "61000018" (str (field "bytes" bsr));
  Alcotest.(check string) "op_name" "Bsr" (str (field "op_name" bsr));
  Alcotest.(check string) "mnemonic" "bsr" (str (field "mnemonic" bsr));
  Alcotest.(check string) "operands" "$000420" (str (field "operands" bsr));
  check_ints "targets" [0x420] (ints (field "targets" bsr));
  Alcotest.(check bool) "bsr falls_through is reported by the flow graph" false
    (field "falls_through" bsr = Json.Bool true);
  Alcotest.(check bool) "escape" true (field "escape" bsr = Json.Null);
  let rts = List.nth lines 5 in
  Alcotest.(check string) "rts escape" "return" (str (field "escape" rts));
  Alcotest.(check int) "length" 2 (int (field "length" rts));
  let move = List.nth lines 3 in
  (match items (field "refs" move) with
   | [reference] ->
       Alcotest.(check string) "role" "read" (str (field "role" reference));
       Alcotest.(check int) "target" 0x410010 (int (field "target" reference));
       Alcotest.(check int) "size" 2 (int (field "size" reference))
   | _ -> Alcotest.fail "expected one reference")

let disasm_a5 () =
  let lines = items (json_out (["disasm"] @ game () @ ["--pc"; "0x420"; "--count"; "1"; "--a5"; "0x410000"; "--json"])) in
  match items (field "refs" (List.hd lines)) with
  | [reference] ->
      Alcotest.(check int) "resolved through a5" 0x410010 (int (field "target" reference));
      Alcotest.(check int) "displacement kept" 0x10 (int (field "a5_disp" reference))
  | _ -> Alcotest.fail "expected one reference"

let disasm_default_count () =
  let out = succeed (["disasm"] @ game () @ ["--pc"; "0x400"]) in
  Alcotest.(check int) "16 lines" 16 (List.length (String.split_on_char '\n' (String.trim out)))

let disasm_hex () =
  let lines = items (json_out ["disasm"; "--pc"; "0x1000"; "--hex"; "4e75 4afc 0efc00000504"; "--json"]) in
  Alcotest.(check int) "three lines" 3 (List.length lines);
  let illegal = List.nth lines 1 in
  Alcotest.(check bool) "illegal is not ok" true (field "ok" illegal = Json.Bool false);
  Alcotest.(check string) "reason" "illegal-opcode" (str (field "reason" illegal));
  Alcotest.(check int) "vector" 4 (int (field "vector" illegal));
  Alcotest.(check string) "illegal text" "dc.w $4afc" (str (field "text" illegal));
  Alcotest.(check string) "CAS2" "cas2.l d0:d4,d0:d4,(d0):(d0)" (str (field "text" (List.nth lines 2)));
  let truncated = items (json_out ["disasm"; "--pc"; "0x1000"; "--hex"; "4e72"; "--json"]) in
  Alcotest.(check string) "truncated" "truncated" (str (field "reason" (List.hd truncated)))

let disasm_errors () =
  fails ~containing:"--hex, or --config" ["disasm"; "--pc"; "0x400"];
  fails ~containing:"exclusive" (["disasm"] @ game () @ ["--pc"; "0x400"; "--count"; "2"; "--to"; "0x410"]);
  fails ~containing:"even" (["disasm"] @ game () @ ["--pc"; "0x401"]);
  fails ~containing:"outside the image" (["disasm"] @ game () @ ["--pc"; "0x2000"]);
  let bad = run (["disasm"] @ game () @ ["--pc"; "zz"]) in
  Alcotest.(check bool) "unparsable addresses are command line errors" true
    (bad.status <> 0 && bad.status <> 1 && bad.out = "");
  fails ~containing:"cannot be combined" (["disasm"] @ game () @ ["--pc"; "0x400"; "--hex"; "4e75"])

(* ---- lower ---- *)

let lower_json () =
  let text = succeed ["lower"; "--pc"; "0x1000"; "--hex"; "4e75"] in
  let json = json_out ["lower"; "--pc"; "0x1000"; "--hex"; "4e75"; "--json"] in
  Alcotest.(check string) "live_out" "XNZVC" (str (field "live_out" json));
  (match items (field "instructions" json) with
   | [instruction] ->
       Alcotest.(check string) "c matches the text form" text (str (field "c" instruction));
       Alcotest.(check string) "mnemonic" "rts" (str (field "mnemonic" instruction))
   | _ -> Alcotest.fail "expected one instruction");
  let from_rom = json_out (["lower"] @ game () @ ["--pc"; "0x404"; "--count"; "2"; "--json"; "--live-out"; "none"]) in
  Alcotest.(check string) "live_out none" "" (str (field "live_out" from_rom))

let lower_from_rom () =
  let from_rom = succeed (["lower"] @ game () @ ["--pc"; "0x406"; "--live-out"; "none"]) in
  let from_hex = succeed ["lower"; "--pc"; "0x406"; "--hex"; "61000018"; "--live-out"; "none"] in
  Alcotest.(check string) "same C from ROM bytes and hex" from_hex from_rom;
  let json = json_out (["lower"] @ game () @ ["--pc"; "0x406"; "--count"; "2"; "--json"]) in
  Alcotest.(check (list string)) "listing"
    ["bsr $000420"; "move.w #1,$4a0000"]
    (List.map (fun i -> str (field "text" i)) (items (field "instructions" json)));
  fails ~containing:"--hex, or --config" ["lower"; "--pc"; "0x400"];
  fails ~containing:"cannot be combined" (["lower"; "--hex"; "4e75"] @ game () @ ["--pc"; "0x400"])

let lower_unsupported () =
  fails ["lower"; "--pc"; "0x1000"; "--hex"; "06d00000"];
  let json = json_out ["lower"; "--pc"; "0x1000"; "--hex"; "06d00000"; "--json"] in
  match items (field "instructions" json) with
  | [instruction] ->
      Alcotest.(check bool) "has an error" true (Json.member "error" instruction <> None);
      Alcotest.(check bool) "no C" true (Json.member "c" instruction = None)
  | _ -> Alcotest.fail "expected one instruction"

(* ---- coverage, check-irq, decisions ---- *)

let coverage () =
  Alcotest.(check string) "text" "total\t0\n" (succeed (["coverage"] @ game ()));
  let json = json_out (["coverage"] @ game () @ ["--json"]) in
  Alcotest.(check string) "game" "fixture" (str (field "game" json));
  Alcotest.(check bool) "unsupported" true (field "unsupported" json = Json.Object []);
  Alcotest.(check int) "total" 0 (int (field "total" json))

let check_irq () =
  Alcotest.(check string) "text" "fixture [interrupt_observes=-] 0x500 Proven -\n"
    (succeed (["check-irq"] @ game ()));
  let json = json_out (["check-irq"] @ game () @ ["--json"]) in
  Alcotest.(check string) "game" "fixture" (str (field "game" json));
  Alcotest.(check string) "interrupt_observes" "" (str (field "interrupt_observes" json));
  match items (field "handlers" json) with
  | [handler] ->
      Alcotest.(check int) "handler" 0x500 (int (field "handler" handler));
      Alcotest.(check string) "verdict" "proven" (str (field "verdict" handler));
      Alcotest.(check string) "flags" "" (str (field "flags" handler));
      Alcotest.(check bool) "undeclared" true (field "undeclared" handler = Json.Bool false)
  | _ -> Alcotest.fail "expected one handler"

let decisions () =
  let text = succeed (["decisions"] @ game ()) in
  Alcotest.(check bool) "header first" true
    (String.starts_with ~prefix:"pc\tdisposition\tlength\tflow\treads\twrites\tlive_out\tidle\tchain_candidates\n" text);
  let json = json_out (["decisions"] @ game () @ ["--json"]) in
  Alcotest.(check string) "game" "fixture" (str (field "game" json));
  Alcotest.(check bool) "sprite_only" true (field "sprite_only" json = Json.Bool false);
  Alcotest.(check int) "columns" 9 (List.length (items (field "columns" json)));
  let rows = items (field "rows" json) in
  Alcotest.(check int) "one row per text line" (List.length (String.split_on_char '\n' (String.trim text)) - 1)
    (List.length rows);
  let row = List.find (fun row -> int (field "pc" row) = 0x406) rows in
  Alcotest.(check string) "disposition" "native" (str (field "disposition" row));
  Alcotest.(check int) "length" 4 (int (field "length" row));
  Alcotest.(check string) "flow" "branch:0x000420" (str (field "flow" row));
  Alcotest.(check bool) "idle" true (field "idle" row = Json.Null);
  check_ints "chain candidates" [0x420] (ints (field "chain_candidates" row));
  let sprite_only = json_out (["decisions"] @ game () @ ["--sprite-only"; "--json"]) in
  Alcotest.(check bool) "sprite_only flag" true (field "sprite_only" sprite_only = Json.Bool true);
  Alcotest.(check bool) "no sprite writers in the fixture" true (field "rows" sprite_only = Json.Array [])

(* ---- rom-read ---- *)

let rom_read () =
  let json = json_out (["rom-read"] @ game () @ ["--region"; "rom"; "--addr"; "0"; "--len"; "8"]) in
  Alcotest.(check string) "bytes" "0041000000000400" (str (field "bytes" json));
  Alcotest.(check int) "size" 0x1000 (int (field "size" json));
  Alcotest.(check string) "region" "rom" (str (field "region" json));
  Alcotest.(check string) "hex" "4bf900410000\n"
    (succeed (["rom-read"] @ game () @ ["--region"; "rom"; "--addr"; "400"; "--len"; "6"; "--format"; "hex"]));
  Alcotest.(check string) "raw" (String.sub rom 0x400 6)
    (succeed (["rom-read"] @ game () @ ["--region"; "rom"; "--addr"; "0x400"; "--len"; "6"; "--format"; "raw"]));
  Alcotest.(check int) "empty read" 0
    (String.length (str (field "bytes" (json_out (["rom-read"] @ game () @ ["--region"; "rom"; "--len"; "0"])))));
  fails ~containing:"outside the region" (["rom-read"] @ game () @ ["--region"; "rom"; "--addr"; "0xffe"; "--len"; "8"]);
  fails ~containing:"unknown region" (["rom-read"] @ game () @ ["--region"; "bogus"; "--len"; "1"]);
  fails ~containing:"outside the region" (["rom-read"] @ game () @ ["--region"; "sound"; "--len"; "1"]);
  Alcotest.(check bool) "rejects unknown formats" true
    ((run (["rom-read"] @ game () @ ["--region"; "rom"; "--len"; "1"; "--format"; "xml"])).status <> 0)

(* ---- inspect ---- *)

let routine_entries json = List.map (fun r -> int (field "entry" r)) (items (field "routines" json))

let routine json entry =
  List.find (fun r -> int (field "entry" r) = entry) (items (field "routines" json))

let inspect () =
  let json = json_out (["inspect"] @ game () @ ["--json"]) in
  Alcotest.(check string) "game" "fixture" (str (field "game" json));
  Alcotest.(check int) "rom_size" 0x1000 (int (field "rom_size" json));
  Alcotest.(check string) "rom_sha256" sha256 (str (field "rom_sha256" json));
  Alcotest.(check string) "rom_crc32" (Printf.sprintf "%08lx" rom_crc) (str (field "rom_crc32" json));
  Alcotest.(check int) "a5 detected" 0x410000 (int (field "a5" json));
  Alcotest.(check string) "a5 source" "detected" (str (field "source" (field "a5_info" json)));
  let vectors = items (field "vectors" json) in
  Alcotest.(check int) "63 vectors" 63 (List.length vectors);
  let named name = List.find (fun v -> str (field "name" v) = name) vectors in
  Alcotest.(check int) "reset" 0x400 (int (field "target" (named "reset")));
  Alcotest.(check int) "reset number" 1 (int (field "number" (named "reset")));
  Alcotest.(check int) "irq2" 0x500 (int (field "target" (named "irq2")));
  Alcotest.(check int) "irq2 number" 26 (int (field "number" (named "irq2")));
  Alcotest.(check int) "trap0 number" 32 (int (field "number" (named "trap0")));
  Alcotest.(check bool) "empty vector is invalid" true (field "valid" (named "irq1") = Json.Bool false);
  Alcotest.(check bool) "reset is valid" true (field "valid" (named "reset") = Json.Bool true);
  Alcotest.(check int) "routine_count" 4 (int (field "routine_count" json));
  check_ints "entries" [0x400; 0x420; 0x430; 0x500] (routine_entries json);
  let reset = routine json 0x400 in
  Alcotest.(check string) "reset kind" "vector" (str (field "kind" reset));
  Alcotest.(check (list string)) "reset vectors" ["reset"] (strs (field "vectors" reset));
  Alcotest.(check int) "reset instructions" 6 (int (field "instructions" reset));
  Alcotest.(check int) "reset bytes" 32 (int (field "bytes" reset));
  check_ints "reset callees" [0x420; 0x430] (ints (field "callees" reset));
  let leaf = routine json 0x420 in
  Alcotest.(check string) "call kind" "call" (str (field "kind" leaf));
  check_ints "callers" [0x400] (ints (field "callers" leaf));
  Alcotest.(check bool) "ranges" true
    (field "ranges" leaf = Json.Array [Json.Array [Int 0x420; Int 0x42c]]);
  Alcotest.(check (list string)) "irq2 and irq3 share one handler" ["irq2"; "irq3"]
    (strs (field "vectors" (routine json 0x500)));
  Alcotest.(check bool) "no profile means no hits" true (Json.member "hits" leaf = None);
  Alcotest.(check bool) "profile is null" true (field "profile" json = Json.Null);
  (match items (field "unresolved_indirect_sites" json) with
   | [site] ->
       Alcotest.(check int) "site" 0x434 (int (field "pc" site));
       Alcotest.(check int) "routine" 0x430 (int (field "routine" site));
       Alcotest.(check string) "text" "jsr (a0)" (str (field "text" site))
   | _ -> Alcotest.fail "expected one unresolved site");
  Alcotest.(check bool) "ram_code" true (field "ram_code" json = Json.Array []);
  let by_kind = field "by_kind" (field "summary" json) in
  Alcotest.(check int) "summary vectors" 2 (int (field "vector" by_kind));
  Alcotest.(check int) "summary calls" 2 (int (field "call" by_kind))

let inspect_options () =
  let summary = json_out (["inspect"] @ game () @ ["--json"; "--summary"]) in
  Alcotest.(check bool) "no routines" true (Json.member "routines" summary = None);
  Alcotest.(check int) "count remains" 4 (int (field "routine_count" summary));
  let a5 = json_out (["inspect"] @ game () @ ["--json"; "--a5"; "0x400100"; "--summary"]) in
  Alcotest.(check int) "explicit a5" 0x400100 (int (field "a5" a5));
  Alcotest.(check string) "source" "explicit" (str (field "source" (field "a5_info" a5)));
  let profile = profile_file "inspect.profile" ~hits:[0x400, 1; 0x420, 3; 0x426, 4; 0x42a, 3] in
  let with_profile = json_out (["inspect"] @ game () @ ["--json"; "--profile-tiers"; profile]) in
  let leaf = routine with_profile 0x420 in
  Alcotest.(check bool) "entry hits" true (field "entry_hits" leaf = Json.Int 3);
  Alcotest.(check bool) "routine hits" true (field "hits" leaf = Json.Int 10);
  Alcotest.(check bool) "unexecuted routine" true (field "hits" (routine with_profile 0x500) = Json.Int 0);
  Alcotest.(check int) "profile identities" 1 (int (field "identities" (field "profile" with_profile)));
  let slim = json_out (["inspect"] @ game () @ ["--json"; "--profile-slim"; profile]) in
  Alcotest.(check bool) "slim reads the same profile" true (slim = with_profile);
  fails ~containing:"mutually exclusive" (["inspect"] @ game () @ ["--profile-tiers"; profile; "--profile-slim"; profile]);
  let other = scratch_file "other.profile" in
  write_file other "F3-BLOCK-PROFILE 1\nrom main 00000001 00000000 00001000\nhit main 00000001 00000400 1\n";
  fails ~containing:"lacks matching main ROM" (["inspect"] @ game () @ ["--profile-tiers"; other])

let inspect_text () =
  let out = succeed (["inspect"] @ game ()) in
  List.iter (fun expected ->
    if not (try ignore (Str.search_forward (Str.regexp_string expected) out 0); true with Not_found -> false)
    then Alcotest.failf "expected %S in the text form" expected)
    ["game\tfixture"; "a5\t0x410000"; "26\tirq2\t0x000500"; "0x000420\tcall\t3"]

(* ---- xref ---- *)

let xref ?(extra = []) target =
  json_out (["xref"] @ game () @ ["--target"; target; "--json"] @ extra)

let refs json = items (field "refs" json)
let pcs json = List.map (fun r -> int (field "pc" r)) (refs json)

let xref_targets () =
  let io = xref "io" in
  check_ints "io region" [0x40a] (pcs io);
  let row = List.hd (refs io) in
  Alcotest.(check string) "role" "write" (str (field "role" row));
  Alcotest.(check string) "op_name" "Move" (str (field "op_name" row));
  Alcotest.(check string) "text" "move.w #1,$4a0000" (str (field "text" row));
  Alcotest.(check int) "target" 0x4a0000 (int (field "target" row));
  Alcotest.(check int) "routine" 0x400 (int (field "routine" row));
  Alcotest.(check bool) "no profile no hits" true (Json.member "hits" row = None);
  check_ints "absolute address" [0x40a] (pcs (xref "4a0000"));
  check_ints "dollar address inside a word access" [0x40a] (pcs (xref "$4a0001"));
  check_ints "a5 offset resolves through the detected base" [0x412; 0x420; 0x426] (pcs (xref "a5+0x10"));
  check_ints "a5 offset with spaces and dollar" [0x412; 0x420; 0x426] (pcs (xref "a5 + $10"));
  check_ints "absolute address of the same variable" [0x412; 0x420; 0x426] (pcs (xref "0x410010"));
  check_ints "work RAM mirror" [0x412; 0x420; 0x426] (pcs (xref "0x430010"));
  check_ints "range" [0x400; 0x412; 0x420; 0x426; 0x430] (pcs (xref "410000-41ffff"));
  check_ints "named ram region" [0x400; 0x412; 0x420; 0x426; 0x430] (pcs (xref "ram"));
  check_ints "code addresses" [0x406] (pcs (xref "0x420"));
  check_ints "unreferenced" [] (pcs (xref "0x4a0100"));
  check_ints "a5 hex address is not an offset" [] (pcs (xref "a50000"))

let xref_roles () =
  let roles target role = List.map (fun r -> str (field "role" r)) (refs (xref ~extra:["--role"; role] target)) in
  Alcotest.(check (list string)) "read" ["read"; "read"] (roles "a5+0x10" "read");
  Alcotest.(check (list string)) "write" ["write"] (roles "a5+0x10" "write");
  Alcotest.(check (list string)) "call" ["call"] (roles "0x420" "call");
  Alcotest.(check (list string)) "branch" [] (roles "0x420" "branch");
  Alcotest.(check (list string)) "call to jsr" ["call"] (roles "0x430" "call");
  Alcotest.(check (list string)) "lea" ["lea"] (roles "410000" "lea");
  Alcotest.(check (list string)) "all" ["lea"; "read"; "write"; "read"; "read"] (roles "ram" "all");
  Alcotest.(check (list string)) "rmw is separate" [] (roles "a5+0x10" "rmw");
  fails ~containing:"unknown --role" (["xref"] @ game () @ ["--target"; "io"; "--role"; "bogus"])

let xref_limit () =
  let limited = xref ~extra:["--limit"; "1"] "a5+0x10" in
  Alcotest.(check int) "total" 3 (int (field "total" limited));
  Alcotest.(check bool) "truncated" true (field "truncated" limited = Json.Bool true);
  Alcotest.(check int) "returned" 1 (List.length (refs limited));
  let all = xref "a5+0x10" in
  Alcotest.(check bool) "not truncated" true (field "truncated" all = Json.Bool false)

let xref_hits_and_a5 () =
  let profile = profile_file "xref.profile" ~hits:[0x40a, 9; 0x420, 2] in
  let json = json_out (["xref"] @ game () @ ["--target"; "io"; "--json"; "--profile-slim"; profile]) in
  Alcotest.(check bool) "hits" true (field "hits" (List.hd (refs json)) = Json.Int 9);
  (* Without a base register only displacement operands can match an a5 offset. *)
  let unknown = json_out (["xref"] @ game () @ ["--target"; "a5+0x10"; "--json"; "--a5"; "0"]) in
  check_ints "explicit zero base" [0x420; 0x426] (List.map (fun r -> int (field "pc" r)) (refs unknown));
  let moved = json_out (["xref"] @ game () @ ["--target"; "a5+0x10"; "--json"; "--a5"; "0x400000"]) in
  check_ints "explicit base moves the resolved address" [0x420; 0x426]
    (List.map (fun r -> int (field "pc" r)) (refs moved));
  let text = succeed (["xref"] @ game () @ ["--target"; "io"]) in
  Alcotest.(check string) "text form"
    "pc\troutine\trole\ttarget\thits\ttext\n0x00040a\t0x000400\twrite\t0x4a0000\t-\tmove.w #1,$4a0000\n" text;
  fails ~containing:"cannot read" (["xref"] @ game () @ ["--target"; "nonsense"]);
  fails ~containing:"below its start" (["xref"] @ game () @ ["--target"; "500-400"])

(* ---- flow ---- *)

let flow ?(extra = []) target =
  json_out (["flow"] @ game () @ ["--target"; target; "--json"] @ extra)

let sites json key = List.map (fun e -> int (field "site" e)) (items (field key json))
let routines json key = List.map (fun e -> int (field "routine" e)) (items (field key json))

let flow_json () =
  let reset = flow "reset" in
  Alcotest.(check int) "entry" 0x400 (int (field "entry" reset));
  Alcotest.(check bool) "exact" true (field "exact" reset = Json.Bool true);
  Alcotest.(check string) "kind" "vector" (str (field "kind" reset));
  check_ints "callers" [] (routines reset "callers");
  check_ints "callees" [0x420; 0x430] (routines reset "callees");
  check_ints "call sites" [0x406; 0x418] (sites reset "callees");
  let callee = List.hd (items (field "callees" reset)) in
  Alcotest.(check string) "edge kind" "call" (str (field "kind" callee));
  Alcotest.(check bool) "repeated" true (field "repeated" callee = Json.Bool false);
  Alcotest.(check bool) "leaf has no children" true (field "children" callee = Json.Array []);
  (match items (field "unresolved" reset) with
   | [site] -> Alcotest.(check int) "unresolved site reached through the callee tree" 0x434 (int (field "pc" site))
   | _ -> Alcotest.fail "expected one unresolved site");
  let shallow = flow ~extra:["--depth"; "1"] "reset" in
  check_ints "depth 1 still lists the unresolved callee" [0x434]
    (List.map (fun s -> int (field "pc" s)) (items (field "unresolved" shallow)));
  let none = flow ~extra:["--depth"; "0"] "0x400" in
  check_ints "depth 0" [] (routines none "callees");
  check_ints "depth 0 unresolved is limited to the routine" []
    (List.map (fun s -> int (field "pc" s)) (items (field "unresolved" none)));
  let own = flow ~extra:["--depth"; "0"] "0x430" in
  check_ints "depth 0 keeps the routine's own unresolved sites" [0x434]
    (List.map (fun s -> int (field "pc" s)) (items (field "unresolved" own)))

let flow_callers () =
  let leaf = flow "0x420" in
  check_ints "callers" [0x400] (routines leaf "callers");
  check_ints "caller site" [0x406] (sites leaf "callers");
  let inside = flow "0x426" in
  Alcotest.(check int) "owning routine" 0x420 (int (field "entry" inside));
  Alcotest.(check bool) "not exact" true (field "exact" inside = Json.Bool false);
  Alcotest.(check int) "pc" 0x426 (int (field "pc" inside));
  let handler = flow "irq2" in
  Alcotest.(check int) "irq2" 0x500 (int (field "entry" handler));
  Alcotest.(check (list string)) "vector names" ["irq2"; "irq3"] (strs (field "vectors" handler));
  Alcotest.(check int) "case-insensitive vector:" 0x400 (int (field "entry" (flow "Vector:RESET")))

let flow_dispatch () =
  let json = flow "?dispatch" in
  Alcotest.(check bool) "dispatch" true (field "dispatch" json = Json.Bool true);
  check_ints "callers are the unresolved sites" [0x434] (sites json "callers");
  check_ints "owning routine" [0x430] (routines json "callers");
  check_ints "no dispatch-only routines" [] (routines json "callees");
  Alcotest.(check string) "text" "jsr (a0)"
    (str (field "text" (List.hd (items (field "unresolved" json)))));
  let profile = profile_file "flow.profile" ~hits:[0x400, 1; 0x500, 1; 0x440, 1] in
  let hit = json_out (["flow"] @ game () @ ["--target"; "?dispatch"; "--json"; "--profile-tiers"; profile]) in
  check_ints "routines entered only through dispatch" [0x440] (routines hit "callees")

let flow_errors () =
  fails ~containing:"not inside any discovered routine" (["flow"] @ game () @ ["--target"; "0x800"]);
  fails ~containing:"cannot read" (["flow"] @ game () @ ["--target"; "nonsense"]);
  fails ~containing:"negative" (["flow"] @ game () @ ["--target"; "reset"; "--depth=-1"])

let flow_text () =
  let out = succeed (["flow"] @ game () @ ["--target"; "reset"]) in
  Alcotest.(check string) "text"
    "target reset entry 0x000400 kind vector\ncallers\ncallees\n  0x000420 call at 0x000406\n  0x000430 call at 0x000418\nunresolved\n  0x000434 in 0x000430 jsr (a0)\n"
    out

(* ---- profile-merge ---- *)

let profile_merge () =
  let left = profile_file "left.profile" ~hits:[0x400, 2; 0x420, 1] in
  let right = profile_file "right.profile" ~hits:[0x420, 4; 0x500, 1] in
  let output = scratch_file "merged.profile" in
  let json = json_out ["profile-merge"; "--output"; output; left; right; "--json"] in
  Alcotest.(check string) "output" output (str (field "output" json));
  Alcotest.(check int) "inputs" 2 (int (field "inputs" json));
  Alcotest.(check int) "roms" 1 (int (field "roms" json));
  Alcotest.(check int) "ever_executed" 3 (int (field "ever_executed" json));
  Alcotest.(check int) "cold_misses" 0 (int (field "cold_misses" json));
  Alcotest.(check string) "file"
    (profile_text ~hits:[0x400, 2; 0x420, 5; 0x500, 1]) (read_file output);
  let merged = json_out (["inspect"] @ game () @ ["--json"; "--profile-tiers"; output]) in
  Alcotest.(check bool) "usable as a profile" true (field "entry_hits" (routine merged 0x420) = Json.Int 5);
  Alcotest.(check string) "text form" (Printf.sprintf "merged 2 profiles into %s\n" output)
    (succeed ["profile-merge"; "--output"; output; left; right]);
  fails ~containing:"duplicate input" ["profile-merge"; "--output"; output; left; left];
  let garbage = scratch_file "garbage.profile" in
  write_file garbage "garbage\n";
  fails ~containing:"unsupported block profile" ["profile-merge"; "--output"; output; garbage]

(* ---- serve ---- *)

let request id meth params =
  Json.to_string (Object ["id", Int id; "method", String meth; "params", Object params])

let game_params =
  let args = game () in
  ["config", Json.String (List.nth args 1); "rom_dir", Json.String (List.nth args 3)]

let serve lines =
  let out = succeed ~input:(String.concat "\n" lines ^ "\n") ["serve"] in
  List.map json_of (String.split_on_char '\n' (String.trim out))

let serve_session () =
  let profile = profile_file "serve.profile" ~hits:[0x400, 1; 0x420, 6] in
  let merged = scratch_file "serve-merged.profile" in
  let responses = serve [
    request 1 "ping" [];
    request 2 "version" [];
    request 3 "rom_read" (game_params @ ["region", String "rom"; "addr", Int 0; "len", Int 8]);
    request 4 "disasm" (game_params @ ["pc", String "0x400"; "count", Int 2]);
    request 5 "inspect" (game_params @ ["routines", Bool false]);
    request 6 "xref" (game_params @ ["target", String "io"]);
    request 7 "flow" (game_params @ ["target", String "reset"; "depth", Int 1]);
    request 8 "coverage" game_params;
    request 9 "check_irq" game_params;
    request 10 "decisions" (game_params @ ["sprite_only", Bool true]);
    request 11 "lower" ["pc", Int 0x1000; "hex", String "4e75"; "live_out", String "none"];
    request 12 "lower" (game_params @ ["pc", Int 0x404; "count", Int 2]);
    request 13 "inspect" (game_params @ ["profile_tiers", String profile]);
    request 14 "profile_merge" ["inputs", Array [String profile]; "output", String merged];
    request 15 "disasm" ["pc", Int 0x1000; "hex", String "4e71"];
    "";
    request 16 "close" game_params;
    request 17 "coverage" game_params;
    request 18 "close" [];
  ] in
  Alcotest.(check int) "one response per request" 18 (List.length responses);
  Alcotest.(check (list int)) "ids are echoed in order" (List.init 18 (fun i -> i + 1))
    (List.map (fun r -> int (field "id" r)) responses);
  List.iter (fun r -> Alcotest.(check bool) "ok" true (field "ok" r = Json.Bool true)) responses;
  let result n = field "result" (List.nth responses (n - 1)) in
  Alcotest.(check bool) "ping" true (result 1 = Json.Object ["pong", Json.Bool true]);
  Alcotest.(check string) "version" "f3recomp" (str (field "name" (result 2)));
  Alcotest.(check bool) "methods listed" true
    (List.mem "profile_merge" (strs (field "methods" (result 2))));
  Alcotest.(check string) "rom_read" "0041000000000400" (str (field "bytes" (result 3)));
  Alcotest.(check (list string)) "disasm" ["lea $410000,a5"; "bsr $000420"]
    (List.map (fun l -> str (field "text" l)) (items (result 4)));
  Alcotest.(check bool) "inspect equals the command line"
    true (result 5 = json_out (["inspect"] @ game () @ ["--json"; "--summary"]));
  Alcotest.(check bool) "xref equals the command line"
    true (result 6 = json_out (["xref"] @ game () @ ["--target"; "io"; "--json"]));
  Alcotest.(check bool) "flow equals the command line"
    true (result 7 = json_out (["flow"] @ game () @ ["--target"; "reset"; "--depth"; "1"; "--json"]));
  Alcotest.(check bool) "coverage equals the command line"
    true (result 8 = json_out (["coverage"] @ game () @ ["--json"]));
  Alcotest.(check bool) "check_irq equals the command line"
    true (result 9 = json_out (["check-irq"] @ game () @ ["--json"]));
  Alcotest.(check bool) "decisions equals the command line"
    true (result 10 = json_out (["decisions"] @ game () @ ["--sprite-only"; "--json"]));
  Alcotest.(check bool) "lower from hex equals the command line"
    true (result 11 = json_out ["lower"; "--pc"; "0x1000"; "--hex"; "4e75"; "--live-out"; "none"; "--json"]);
  Alcotest.(check bool) "lower from ROM equals the command line"
    true (result 12 = json_out (["lower"] @ game () @ ["--pc"; "0x404"; "--count"; "2"; "--json"]));
  Alcotest.(check bool) "profiled inspect equals the command line" true
    (result 13 = json_out (["inspect"] @ game () @ ["--json"; "--profile-tiers"; profile]));
  Alcotest.(check int) "profile_merge" 2 (int (field "ever_executed" (result 14)));
  Alcotest.(check string) "merged file written" (profile_text ~hits:[0x400, 1; 0x420, 6]) (read_file merged);
  Alcotest.(check string) "hex disasm" "nop" (str (field "text" (List.hd (items (result 15)))));
  Alcotest.(check bool) "close" true (result 16 = Json.Object ["closed", Json.Bool true]);
  Alcotest.(check bool) "usable after close" true (result 17 = json_out (["coverage"] @ game () @ ["--json"]))

let serve_errors () =
  let responses = serve [
    "not json";
    "[1,2]";
    "{\"id\":\"a\"}";
    request 2 "nope" [];
    request 3 "disasm" ["pc", Int 0x1000];
    request 4 "rom_read" (game_params @ ["addr", Int 0]);
    request 5 "rom_read" (game_params @ ["region", String "rom"; "addr", Int 0x2000; "len", Int 4]);
    request 6 "xref" (game_params @ ["target", String "io"; "role", String "bogus"]);
    request 7 "inspect" [ "config", Json.String "missing.toml"; "rom_dir", String "." ];
    request 8 "flow" (game_params @ ["target", String "0x800"]);
    request 9 "inspect" (game_params @ ["profile_tiers", String "a"; "profile_slim", String "b"]);
    request 10 "disasm" (game_params @ ["pc", Int 0x401]);
    request 11 "lower" ["pc", Int 0x1000; "hex", String "4e75"; "count", Int 0];
    request 12 "ping" [];
  ] in
  Alcotest.(check int) "every line is answered" 14 (List.length responses);
  let failure n = List.nth responses (n - 1) in
  let message n = str (field "error" (failure n)) in
  List.iter (fun n ->
    Alcotest.(check bool) (Printf.sprintf "response %d failed" n) true
      (field "ok" (failure n) = Json.Bool false)) (List.init 13 (fun i -> i + 1));
  Alcotest.(check bool) "malformed JSON has a null id" true (field "id" (failure 1) = Json.Null);
  Alcotest.(check bool) "non-object request" true (field "id" (failure 2) = Json.Null);
  Alcotest.(check bool) "missing method keeps the id" true (field "id" (failure 3) = Json.String "a");
  Alcotest.(check bool) "unknown method" true (message 4 = "unknown method \"nope\"");
  Alcotest.(check bool) "missing source" true (String.length (message 5) > 0);
  Alcotest.(check bool) "missing len" true (message 6 = "missing parameter \"len\"");
  Alcotest.(check bool) "outside region" true
    (String.length (message 7) > 0 && Str.string_match (Str.regexp ".*outside the region") (message 7) 0);
  Alcotest.(check bool) "responses after errors still work" true (field "ok" (failure 14) = Json.Bool true)

let () =
  let case name test = Alcotest.test_case name `Quick test in
  Alcotest.run "f3recomp JSON API" [
    "disasm", [case "text" disasm_text; case "json" disasm_json; case "a5" disasm_a5;
               case "default count" disasm_default_count; case "hex" disasm_hex;
               case "errors" disasm_errors];
    "lower", [case "json" lower_json; case "from ROM" lower_from_rom;
              case "unsupported" lower_unsupported];
    "reports", [case "coverage" coverage; case "check-irq" check_irq; case "decisions" decisions];
    "rom-read", [case "formats and errors" rom_read];
    "inspect", [case "json" inspect; case "options and profiles" inspect_options;
                case "text" inspect_text];
    "xref", [case "targets" xref_targets; case "roles" xref_roles; case "limit" xref_limit;
             case "hits, a5 and text" xref_hits_and_a5];
    "flow", [case "json" flow_json; case "callers" flow_callers; case "dispatch" flow_dispatch;
             case "errors" flow_errors; case "text" flow_text];
    "profile-merge", [case "merge" profile_merge];
    "serve", [case "session" serve_session; case "errors" serve_errors];
  ]
