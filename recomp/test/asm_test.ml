open F3recomp
open F3recomp_test

let hex bytes =
  String.to_seq bytes |> Seq.map (fun byte -> Printf.sprintf "%02x" (Char.code byte))
  |> List.of_seq |> String.concat " "

let round_trip () =
  let seen = Hashtbl.create 59 in
  let print { Insn.pc; length; op } =
    Printf.sprintf "%s at 0x%08x (%d bytes)" (Insn_gen.constructor op) pc length
  in
  let property = QCheck.Test.make ~count:100_000 ~name:"decoder round trip"
    (QCheck.make ~print Insn_gen.generate)
    (fun insn ->
      let bytes = Asm.encode insn in
      let decoded = Decode.at { Image.base = insn.pc; bytes } insn.pc in
      if decoded <> Decode.Insn insn then
        QCheck.Test.fail_reportf "bytes %s; decoded %s" (hex bytes)
          (match decoded with Decode.Insn actual -> print actual
           | Decode.Illegal illegal -> Printf.sprintf "illegal vector %d after %d bytes" illegal.vector illegal.length);
      Hashtbl.replace seen (Insn_gen.constructor insn.op) ();
      true)
  in
  QCheck.Test.check_exn ~rand:(Random.State.make [|0x68020|]) property;
  Alcotest.(check int) "Insn.op constructors covered" 59 (Hashtbl.length seen)

let alternate_widths () =
  let pc = 0x1000 in
  let bytes = Bytes.create 24 in
  List.iter (fun extension ->
    for index = 1 to 11 do Bytes.set_uint16_be bytes (2 * index) extension done;
    for opcode = 0 to 65535 do
      Bytes.set_uint16_be bytes 0 opcode;
      let image = { Image.base = pc; bytes = Bytes.to_string bytes } in
      match Decode.at image pc with
      | Decode.Illegal _ -> ()
      | Decode.Insn insn ->
          let input = String.sub image.bytes 0 insn.length in
          let canonical = match Asm.encode insn with
            | bytes -> bytes
            | exception Invalid_argument reason ->
                Alcotest.failf "%s (%s): %s" (Insn_gen.constructor insn.op) (hex input) reason
          in
          if Decode.at { image with bytes = canonical } pc <> Decode.Insn insn then
            Alcotest.failf "%s (%s) re-encoded as %s" (Insn_gen.constructor insn.op)
              (hex input) (hex canonical)
    done)
    [0; 0xffff; 0x0104; 0x0137]

let labels () =
  let base = 0x2000 in
  let image = Asm.(assemble ~base [
    label "loop"; moveq 1 (d 0); add Long (d 0) (d 1); bcc Eq "done";
    dbcc F 2 "loop"; label "done"; insn Rts
  ]) in
  let check offset length op =
    let pc = base + offset in
    Alcotest.(check bool) "assembled instruction" true
      (Decode.at image pc = Decode.Insn { pc; length; op })
  in
  check 0 2 (Moveq (1, 0));
  check 2 2 (Alu (Add, Long, Register (D 0), Register (D 1)));
  check 4 6 (Bcc (Eq, base + 14));
  check 10 4 (Dbcc (F, 2, base));
  check 14 2 Rts

let () =
  Alcotest.run "Assembler" [
    "properties", [
      Alcotest.test_case "100,000 round trips and constructor coverage" `Quick round_trip;
      Alcotest.test_case "decoded instructions retain their width" `Quick alternate_widths;
    ];
    "programs", [Alcotest.test_case "forward and backward labels" `Quick labels]
  ]
