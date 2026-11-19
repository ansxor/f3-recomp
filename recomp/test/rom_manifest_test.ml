open F3recomp

let fixture = {|[game]
id = "fixture"
[rom]
size = 4
interleave = 1
[[rom.lanes]]
file = "unused"
size = 4
crc = "b63cfbcd"
sha1 = "12dada1fff4d4787ade3333147202c3b443e376f"
[sprites]
size = 0
[sprites_hi]
size = 0
[tiles]
size = 0
[tiles_hi]
size = 0
[samples]
size = 0
|}

let chipspec = {|file = "chip"
size = 4
crc = "b63cfbcd"
sha1 = "12dada1fff4d4787ade3333147202c3b443e376f"
|}

let short_fields = {|file = "chip"
size = 4
crc = "245a7ee8"
sha1 = "157ce19d0d53ba1a269384f968794e7c273f5239"
short_size = 2
short_crc = "b6cc4292"
|}

let short_sha = "0ca623e2855f2c75c842ad302fe820e41b4d197d"
let short_with_sha sha1 = short_fields ^ Printf.sprintf "short_sha1 = %S\n" sha1

let sound ?(size = 4) ?(settings = "") lanes =
  let lane_tables = List.map (fun lane -> "[[sound.lanes]]\n" ^ lane) lanes in
  Printf.sprintf "[sound]\nsize = %d\n%s%s" size settings (String.concat "" lane_tables)

let load sound ~rom_dir =
  let config = Config.of_string ~path:"fixture/config.toml" (fixture ^ sound) in
  let manifest = Rom_manifest.of_configs [config] in
  (Rom_manifest.load_region manifest ~game:"fixture" Sound ~rom_dir).bytes

let with_chips files test =
  let dir = Filename.temp_file ~temp_dir:(Sys.getcwd ()) "rom-manifest" "" in
  Sys.remove dir;
  Sys.mkdir dir 0o700;
  Fun.protect
    ~finally:(fun () ->
      Sys.readdir dir |> Array.iter (fun file -> Sys.remove (Filename.concat dir file));
      Sys.rmdir dir)
    (fun () ->
      List.iter (fun (name, bytes) ->
          Out_channel.with_open_bin (Filename.concat dir name)
            (fun file -> Out_channel.output_string file bytes)) files;
      test ~rom_dir:dir)

let assert_image name ~expected ~files spec =
  Alcotest.test_case name `Quick (fun () ->
      Alcotest.(check string) "image bytes" expected (with_chips files (load spec)))

let rejects name ~chip ~error spec =
  Alcotest.test_case name `Quick (fun () ->
      match with_chips ["chip", chip] (load spec) with
      | _ -> Alcotest.fail "expected chip verification to fail"
      | exception Error.Invalid { what; _ } ->
          Alcotest.(check string) "diagnostic" error what)

let full = "\x01\x02\x03\x04"
let short = "\x01\x02"
let wrong_sha = String.make 40 '0'
let shortspec = short_with_sha short_sha

let () =
  Alcotest.run "ROM manifest"
    ["chip validation",
     [rejects "size mismatch" ~chip:"\x01\x02\x03"
        ~error:"size mismatch: expected 4 bytes, got 3" (sound [chipspec]);
      rejects "corrupted mirrored chip" ~chip:"\x01\x02\x03\x05" ~error:"CRC32 mismatch"
        (sound ~settings:"mirror_size = 8\n" [chipspec]);
      rejects "full SHA1 mismatch" ~chip:full ~error:"SHA1 mismatch"
        (sound [Printf.sprintf "file = \"chip\"\nsize = 4\ncrc = \"b63cfbcd\"\nsha1 = %S\n" wrong_sha]);
      rejects "corruption outside source slice" ~chip:"\x01\x02\x03\x05" ~error:"CRC32 mismatch"
        (sound [chipspec ^ "length = 2\n"]);
      rejects "corrupt short dump" ~chip:"\x01\x03" ~error:"CRC32 mismatch" (sound [shortspec]);
      rejects "short SHA1 mismatch" ~chip:short ~error:"SHA1 mismatch"
        (sound [short_with_sha wrong_sha]);
      rejects "unrecognized short size" ~chip:"\x01"
        ~error:"size mismatch: expected 4 bytes, got 1" (sound [shortspec]);
      rejects "padded short dump must match full hash" ~chip:short ~error:"CRC32 mismatch"
        (sound [chipspec ^ "short_size = 2\nshort_crc = \"b6cc4292\"\nshort_sha1 = \"0ca623e2855f2c75c842ad302fe820e41b4d197d\"\n"]);
      assert_image "validated short dump pads with 0xff" ~expected:"\x01\x02\xff\xff"
        ~files:["chip", short] (sound [shortspec])];
     "SHA1",
     [Alcotest.test_case "padding spans two blocks" `Quick (fun () ->
          Alcotest.(check string) "SHA1 digest" "c2db330f6083854c99d4b5bfb6e8f29f201be699"
            (Sha1.hex_digest (String.make 56 'a')))];
     "lane placement",
     [assert_image "absent region" ~expected:"" ~files:[] (sound ~size:0 []);
      assert_image "grouped sparse fill and mirror" ~expected:"\x01\x02\xff\xff\x03\x04\xff\xff\x01\x02\xff\xff\x03\x04\xff\xff"
        ~files:["chip", full]
        (sound ~size:8 ~settings:"fill = 255\nmirror_size = 16\n" [chipspec ^ "group = 2\nstride = 4\n"]);
      assert_image "word swapped source slice" ~expected:"\x00\x03\x02\x00"
        ~files:["chip", full] (sound [chipspec ^ "source_offset = 1\nlength = 2\noffset = 1\nword_swap = true\n"]);
      assert_image "continue and ordered overwrite" ~expected:"\x03\x01\x05\x06\x07\x08"
        ~files:["chip", full; "patch", "\x05\x06\x07\x08"]
        (sound ~size:6 [chipspec ^ "length = 2\noffset = 1\nstride = 2\n";
                        chipspec ^ "source_offset = 2\nstride = 2\n";
                        "file = \"patch\"\nsize = 4\ncrc = \"538d4d69\"\nsha1 = \"85d9651d9a399a67e015d047fd9e6a941e6b20bb\"\noffset = 2\n"])]]
