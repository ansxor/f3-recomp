open F3recomp

let config extra = Config.of_string ~path:"fixture/config.toml" ({|
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
|} ^ extra)

let discover ?(extra = "") chunks =
  let config = config extra in
  let bytes = Bytes.make 0x1000 '\000' in
  Bytes.set_int32_be bytes 4 0x400l;
  List.iter (fun (pc, code) -> Bytes.blit_string code 0 bytes pc (String.length code)) chunks;
  let rom = { Image.base = 0; bytes = Bytes.to_string bytes } in
  let exclusions = Exclusion.of_config ~image:rom config.exclusions.main in
  Game.discover { Game.config; rom; exclusions }

let native program pcs =
  List.iter (fun pc ->
    Alcotest.(check bool) (Printf.sprintf "native %#x" pc) true
      (Option.is_some (Program.insn program pc))) pcs

let all_aligned () =
  let nop = "\x4e\x71" in
  let long_run = String.concat "" (List.init 40 (fun _ -> nop)) ^ "\x4e\x75" in
  let program = discover [
    0x400, "\x30\x3c\x00\x80\x41\xfa\x01\x7a\xd0\xc0\x4e\xd0";
    0x481, "\x00\x07\x00"; 0x500, "\x20\x3c\x4e\x71\x4e\x75";
    0x600, "\x70\x2a\x4e\x75"; 0x700, nop ^ "\x4e\x75";
    0x800, long_run; 0xffe, "\x20\x3c";
  ] in
  native program ([0x500; 0x502; 0x504; 0x600; 0x602; 0x700; 0x702] @
    List.init 41 (fun i -> 0x800 + 2 * i));
  let legal_bounds = Pc.Map.for_all (fun pc -> function
    | Decode.Insn insn ->
        pc mod 2 = 0 && insn.length mod 2 = 0 && pc + insn.length <= 0x1000
    | Illegal _ -> pc mod 2 = 0) program.decoded in
  Alcotest.(check bool) "decoded PCs and instruction lengths are aligned and bounded" true legal_bounds;
  match Pc.Map.find 0xffe program.decoded with
  | Illegal { reason = Truncated; _ } -> ()
  | _ -> Alcotest.fail "expected a truncated immediate at the final aligned PC"

let rejects fragment action =
  match action () with
  | _ -> Alcotest.fail "expected discovery to reject the fixture"
  | exception Error.Invalid { what; _ } ->
      Alcotest.(check bool) what true (String.starts_with ~prefix:fragment what)

let odd_entry () =
  rejects "address 0x401 must be even" (fun () ->
    discover ~extra:"[discovery]\nentry_points = [0x401]\n" [])

let candidates program pc = (Pc.Map.find pc program.Program.tables).candidates

let computed () =
  let program = discover [
    0x500, "\x30\x3b\x00\x08\x4e\xfb\x00\x04\x4e\x71\x00\x06\x00\x08\x00\x00\x4e\x71\x4e\x75";
    0x600, "\x4e\xf9\x00\x00\x08\x00"; 0x800, "\x4e\x75";
  ] in
  Alcotest.(check (list int)) "relative table" [0x50a; 0x510; 0x512] (candidates program 0x504);
  let flow pc = Option.get (Program.flow program pc) in
  Alcotest.(check (list int)) "computed site has no static edge" [] (flow 0x504).targets;
  Alcotest.(check bool) "computed site is not a function seed" false (Pc.Set.mem 0x504 program.functions);
  Alcotest.(check (list int)) "direct edge" [0x800] (flow 0x600).targets;
  Alcotest.(check bool) "direct edge has no table" false (Pc.Map.mem 0x600 program.tables)

let configured () =
  let program = discover ~extra:{|
[[discovery.jump_tables]]
address = 0x700
targets = [0x740, 0x720]
|} [0x700, "\x4e\xd0"; 0x720, "\x4e\x75"; 0x740, "\x4e\x75"] in
  let table = Pc.Map.find 0x700 program.tables in
  Alcotest.(check bool) "configured evidence" true (table.evidence = Configured);
  Alcotest.(check (list int)) "sorted targets" [0x720; 0x740] table.candidates;
  Alcotest.(check (list int)) "no static edge" []
    (Option.get (Program.flow program 0x700)).targets

let scanned () =
  let pointers = "\x00\x00\x04\x00\x00\x00\x04\x20\xff\xff\xff\xff" in
  let targets = [0x400, "\x4e\x75"; 0x420, "\x70\x01\x4e\x75"] in
  List.iter (fun dispatch ->
    let program = discover ((0x500, dispatch) :: (0x540, pointers) :: targets) in
    Alcotest.(check (list int)) "backward register-staged targets" [0x400; 0x420]
      (candidates program 0x508))
    ["\x20\x7b\x00\x3e\x4e\x71\x4e\x71\x4e\xd0";
     "\x41\xfb\x00\x3e\x4e\x71\x4e\x71\x4e\xf0\x01\x51"];
  let signed = discover ((0x500, "\x4e\xbb\x01\x21\xff\xe0\x4e\x75") ::
                          (0x4e2, pointers) :: targets) in
  Alcotest.(check (list int)) "negative full extension displacement" [0x400; 0x420]
    (candidates signed 0x500)

let excluded_range = {|
[[exclude]]
start = 0x800
end = 0x900
reason = "fixture data"
evidence = "fixture"
|}

let exclusions () =
  let nop = "\x4e\x71" in
  let program = discover ~extra:excluded_range [
    0x500, "\x4e\xf9\x00\x00\x08\x00"; 0x7fe, nop ^ nop; 0x8fe, nop ^ nop;
  ] in
  native program [0x7fe; 0x900];
  Alcotest.(check int) "one disposition per non-excluded word" (0x1000 / 2 - 0x100 / 2)
    (Pc.Map.cardinal program.decoded);
  Alcotest.(check bool) "entire half-open range absent" false
    (Pc.Map.exists (fun pc _ -> pc >= 0x800 && pc < 0x900) program.decoded);
  let selection = Program.select program ~profile:None in
  let instruction = Option.get (Program.insn program 0x500) in
  let flow = Flow.of_insn instruction in
  Alcotest.(check (list int)) "static transfer keeps original destination" [0x800] flow.targets;
  Alcotest.(check (list int)) "excluded destination cannot chain" []
    (Selection.chain_candidates selection instruction flow)

let excluded_seeds () =
  rejects "vector 1 points to excluded PC 0x800" (fun () ->
    discover ~extra:excluded_range [4, "\x00\x00\x08\x00"]);
  rejects "excluded PC 0x800" (fun () ->
    discover ~extra:(excluded_range ^ {|
[[discovery.jump_tables]]
address = 0x500
targets = [0x800]
|}) []);
  rejects "configured table at 0x500 targets excluded PC 0x800" (fun () ->
    discover ~extra:(excluded_range ^ {|
[[discovery.jump_tables]]
address = 0x500
table = 0x600
count = 1
|}) [0x500, "\x4e\xd0"; 0x600, "\x00\x00\x08\x00"])

let () =
  let case name test = Alcotest.test_case name `Quick test in
  Alcotest.run "aligned discovery" ["ROM", [
    case "computed, odd-offset, long and overlapping entries" all_aligned;
    case "odd configured entry" odd_entry;
    case "computed candidates and static edges" computed;
    case "explicit table overrides scanning" configured;
    case "staged tables and signed displacement" scanned;
    case "exclusion accounting and dispatch boundary" exclusions;
    case "excluded vector and configured targets" excluded_seeds;
  ]]
