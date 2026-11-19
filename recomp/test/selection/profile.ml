open F3recomp

let load text =
  let path = Filename.temp_file "f3-test" ".profile" in
  Fun.protect ~finally:(fun () -> Sys.remove path) (fun () ->
    Out_channel.with_open_text path (fun out -> output_string out text);
    Profile.load path)

let profile rows = load ("F3-BLOCK-PROFILE 1\n" ^ rows)

let rejects details run =
  match run () with
  | _ -> Alcotest.fail "expected Error.Invalid"
  | exception Error.Invalid { what; _ } ->
      List.iter (fun detail ->
        try ignore (Str.search_forward (Str.regexp_string detail) what 0)
        with Not_found -> Alcotest.failf "expected %S in %S" detail what)
        details

let merge () =
  let left = profile "rom main 00000001 00000000 00000008\nhit main 00000001 00000002 18446744073709551615\n" in
  let right = profile
    "hit main 00000001 00000002 3\nrom main 00000001 00000000 00000008\nrom main 00000002 00000000 00000008\nhit main 00000002 00000002 4\nhit main 00000002 00000002 3\nmiss main 00000001 00000004 1\n" in
  let combined = Profile.merge left right in
  let count kind crc pc = Profile.count combined kind Main ~crc ~pc in
  let check label expected = Alcotest.(check (option int64)) label (Some expected) in
  check "saturates at UINT64_MAX" Int64.minus_one (count Hit 1l 2);
  check "keeps other ROM and adds duplicate rows" 7L (count Hit 2l 2);
  check "keeps misses" 1L (count Miss 1l 4);
  let conflict = profile "rom main 00000001 00000000 0000000a\n" in
  rejects ["bounds disagree"] (fun () -> Profile.merge left conflict)

let identity () =
  let image : Image.t = { base = 0xc00000; bytes = "\x4e\x71\x4e\x71\x4e\x71" } in
  let crc = Image.crc32 image in
  let profile = profile (Printf.sprintf
    "rom sound %08lx 00c00000 00000006\nhit sound %08lx 00c00000 2\nmiss sound %08lx 00c00002 1\n" crc crc crc) in
  let hot image = Profile.hot profile Sound image ~exclusions:[] in
  Alcotest.(check (list int)) "only hits" [image.base] (Pc.Set.elements (hot image));
  rejects ["lacks matching sound ROM"] (fun () -> hot { image with bytes = String.make 6 '\x00' });
  rejects ["lacks matching sound ROM"] (fun () -> hot { image with base = 0 })

let exclusions () =
  let bytes = "\x4e\x71\x4e\x71\x4e\x71\x4e\x71" in
  List.iter (fun (region, name, other, base) ->
    let image : Image.t = { base; bytes } in
    let exclusion : Exclusion.t =
      { start = base + 2; stop = base + 4; reason = "fixture data"; evidence = "fixture"; fill = None } in
    let crc = Image.crc32 image in
    let other_crc = Int32.logxor crc 1l in
    let rom name crc = Printf.sprintf "rom %s %08lx %08x 00000008\n" name crc base in
    let row kind name crc pc = Printf.sprintf "%s %s %08lx %08x 1\n" kind name crc pc in
    let data = rom name crc ^ rom name other_crc ^ rom other crc
      ^ row "hit" name crc base ^ row "hit" name crc (base + 4)
      ^ row "hit" name other_crc (base + 2) ^ row "hit" other crc (base + 2) in
    let hot text = Profile.hot (profile text) region image ~exclusions:[exclusion] in
    Alcotest.(check (list int)) "other identities ignored" [base; base + 4]
      (Pc.Set.elements (hot data));
    List.iter (fun kind ->
      rejects [kind; name; Printf.sprintf "%#x" (base + 2);
               Printf.sprintf "[%#x, %#x)" (base + 2) (base + 4); exclusion.reason]
        (fun () -> hot (data ^ row kind name crc (base + 2))))
      ["hit"; "miss"])
    [Profile.Main, "main", "sound", 0; Profile.Sound, "sound", "main", 0xc00000]

let invalid () =
  List.iter (fun (row, message) ->
    rejects [message] (fun () -> profile ("rom main 00000001 00000000 00000008\n" ^ row ^ "\n")))
    ["hit main 00000001 00000001 1", "outside aligned ROM";
     "hit main 00000001 00000008 1", "outside aligned ROM";
     "hit main 00000002 00000002 1", "without ROM identity";
     "hit main 00000001 00000002 0", "outside uint64";
     "hit main 00000001 00000002 -1", "expected decimal";
     "hit main 00000001 00000002 18446744073709551616", "outside uint64";
     "rom main 00000001 00000000 00000008", "duplicate ROM";
     "rom sound 00000001 fffffffe 00000004", "invalid ROM bounds";
     "rom main 0000000_ 00000000 00000008", "expected eight hex digits"];
  rejects ["unsupported block profile version"] (fun () -> load "F3-BLOCK-PROFILE 2\n")

let () =
  Alcotest.run "block profile" ["format", [
    Alcotest.test_case "merge, identities and saturation" `Quick merge;
    Alcotest.test_case "matching identity and miss filtering" `Quick identity;
    Alcotest.test_case "exclusions on both CPUs" `Quick exclusions;
    Alcotest.test_case "strict input" `Quick invalid;
  ]]
