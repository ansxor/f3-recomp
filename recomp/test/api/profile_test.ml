open F3recomp

let with_temp_dir test =
  let dir = Filename.temp_file ~temp_dir:(Sys.getcwd ()) "profile" "" in
  Sys.remove dir;
  Sys.mkdir dir 0o700;
  Fun.protect
    ~finally:(fun () ->
      Array.iter (fun file -> Sys.remove (Filename.concat dir file)) (Sys.readdir dir);
      Sys.rmdir dir)
    (fun () -> test dir)

let write_text path text = Out_channel.with_open_bin path (fun out -> output_string out text)
let read_text path = In_channel.with_open_bin path In_channel.input_all

let load text =
  with_temp_dir (fun dir ->
    let path = Filename.concat dir "in.profile" in
    write_text path ("F3-BLOCK-PROFILE 1\n" ^ text);
    Profile.load path)

let count profile kind region crc pc = Profile.count profile kind region ~crc ~pc

let canonical_text = String.concat "" [
  "F3-BLOCK-PROFILE 1\n";
  "rom main 00000001 00000000 00000008\n";
  "rom main deadbeef 00000000 00000010\n";
  "rom sound 00000002 00c00000 00000004\n";
  "hit main 00000001 00000002 3\n";
  "hit main 00000001 00000004 18446744073709551615\n";
  "hit main deadbeef 00000008 7\n";
  "hit sound 00000002 00c00002 1\n";
  "miss main 00000001 00000006 2\n";
]

let write_is_canonical () =
  (* Rows arrive shuffled, and the high CRC sorts after the low one. *)
  let profile = load (String.concat "" [
    "rom sound 00000002 00c00000 00000004\n";
    "rom main deadbeef 00000000 00000010\n";
    "hit sound 00000002 00c00002 1\n";
    "hit main deadbeef 00000008 7\n";
    "rom main 00000001 00000000 00000008\n";
    "miss main 00000001 00000006 2\n";
    "hit main 00000001 00000004 18446744073709551615\n";
    "hit main 00000001 00000002 1\n";
    "hit main 00000001 00000002 2\n";
  ]) in
  with_temp_dir (fun dir ->
    let path = Filename.concat dir "out.profile" in
    Profile.write path profile;
    Alcotest.(check string) "file" canonical_text (read_text path))

let round_trip () =
  let profile = load (String.concat "" [
    "rom main 00000001 00000000 00000008\n";
    "rom main deadbeef 00000000 00000010\n";
    "rom sound 00000002 00c00000 00000004\n";
    "hit main 00000001 00000002 3\n";
    "hit main 00000001 00000004 18446744073709551615\n";
    "hit main deadbeef 00000008 7\n";
    "hit sound 00000002 00c00002 1\n";
    "miss main 00000001 00000006 2\n";
  ]) in
  with_temp_dir (fun dir ->
    let path = Filename.concat dir "out.profile" in
    Profile.write path profile;
    let again = Profile.load path in
    let check label kind region crc pc expected =
      Alcotest.(check (option int64)) label expected (count again kind region crc pc) in
    check "plain" Hit Main 1l 2 (Some 3L);
    check "saturated count survives" Hit Main 1l 4 (Some Int64.minus_one);
    check "high crc" Hit Main (Int32.of_string "0xdeadbeef") 8 (Some 7L);
    check "sound" Hit Sound 2l 0xc00002 (Some 1L);
    check "miss" Miss Main 1l 6 (Some 2L);
    check "absent" Hit Main 1l 8 None;
    Alcotest.(check bool) "stats" true
      (Profile.stats again = { identities = 3; executed = 4; cold_misses = 1 });
    Profile.write (Filename.concat dir "again.profile") again;
    Alcotest.(check string) "writing is stable" (read_text path) (read_text (Filename.concat dir "again.profile")))

let merge_round_trip () =
  let left = load "rom main 00000001 00000000 00000008\nhit main 00000001 00000002 18446744073709551615\nhit main 00000001 00000004 5\n" in
  let right = load "rom main 00000001 00000000 00000008\nrom main 00000002 00000000 00000008\nhit main 00000001 00000002 3\nhit main 00000002 00000006 4\nmiss main 00000001 00000004 1\n" in
  with_temp_dir (fun dir ->
    let path = Filename.concat dir "merged.profile" in
    Profile.write path (Profile.merge left right);
    let merged = Profile.load path in
    let check label expected actual = Alcotest.(check (option int64)) label (Some expected) actual in
    check "saturates" Int64.minus_one (count merged Hit Main 1l 2);
    check "left only" 5L (count merged Hit Main 1l 4);
    check "right only identity" 4L (count merged Hit Main 2l 6);
    check "miss" 1L (count merged Miss Main 1l 4);
    Alcotest.(check bool) "stats" true
      (Profile.stats merged = { identities = 2; executed = 3; cold_misses = 1 }))

let () =
  let case name test = Alcotest.test_case name `Quick test in
  Alcotest.run "profile write" [
    "profile", [case "canonical file" write_is_canonical; case "write and load" round_trip;
                case "merge, write and load" merge_round_trip]]
