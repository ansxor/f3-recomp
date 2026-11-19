open F3recomp
open Insn

let base = 0x1000
let image words =
  let bytes = Bytes.create (List.length words * 2) in
  List.iteri (fun index word -> Bytes.set_uint16_be bytes (2 * index) word) words;
  { Image.base; bytes = Bytes.unsafe_to_string bytes }

let decode words = Decode.at (image words) base

let expect ?length words op =
  let length = Option.value length ~default:(List.length words * 2) in
  if decode words <> Decode.Insn { pc = base; length; op } then
    Alcotest.failf "unexpected decoding of %04x" (List.hd words)

let illegal words vector reason length =
  if decode words <> Decode.Illegal { vector; reason; length } then
    Alcotest.failf "unexpected exception for %04x" (List.hd words)

let legality () =
  let table = Array.make 65536 false in
  In_channel.with_open_text "../68020_opcodes.csv" (fun file ->
    In_channel.fold_lines (fun () line ->
      if line <> "" && line.[0] <> '#' then
        Scanf.sscanf line "%x,%x,%d" (fun mask value legal ->
          let free = 0xffff lxor mask in
          let rec populate bits =
            table.(value lor bits) <- legal = 1;
            if bits <> 0 then populate ((bits - 1) land free)
          in
          populate free)) () file);
  table

let exhaustive () =
  let legal = legality () in
  let prefix = Bytes.create 24 and suffix = Bytes.create 24 in
  let view bytes : Image.t = { base; bytes } in
  for opcode = 0 to 65535 do
    Bytes.set_uint16_be prefix 0 opcode;
    for pattern = 0 to 513 do
      let ext =
        match pattern with
        | 512 -> 0
        | 513 -> 0xffff
        | _ -> 0x100 lor (pattern land 0xff) lor ((pattern land 0x100) lsl 1)
      in
      for index = 1 to 11 do Bytes.set_uint16_be prefix (index * 2) ext done;
      let decoded = Decode.at (view (Bytes.to_string prefix)) base in
      let length = match decoded with
        | Decode.Insn insn -> insn.length
        | Decode.Illegal illegal -> illegal.length
      in
      let actual_vector = match decoded with
        | Decode.Insn _ -> None
        | Decode.Illegal illegal -> Some illegal.vector
      in
      let expected_vector =
        if opcode lsr 12 = 10 then Some 10
        else if opcode lsr 12 = 15 then Some 11
        else if opcode = 0x4e70 then None
        else if opcode = 0x4afc || opcode land 0xfff8 = 0x4848 then Some 4
        else if (opcode = 0x4e7a || opcode = 0x4e7b)
                && not (List.mem (ext land 0xfff) [0; 1; 2; 0x800; 0x801; 0x802; 0x803; 0x804])
        then Some 4
        else if legal.(opcode) then None else Some 4
      in
      if actual_vector <> expected_vector then
        Alcotest.failf "%04x %04x: vector %s, expected %s" opcode ext
          (Option.fold ~none:"none" ~some:string_of_int actual_vector)
          (Option.fold ~none:"none" ~some:string_of_int expected_vector);
      if length < 2 || length > 22 || length mod 2 <> 0 then
        Alcotest.failf "%04x %04x: length %d" opcode ext length;
      Bytes.blit prefix 0 suffix 0 length;
      for index = length to 23 do
        Bytes.set_uint8 suffix index (Bytes.get_uint8 prefix index lxor 0xff)
      done;
      if Decode.at (view (Bytes.to_string suffix)) base <> decoded then
        Alcotest.failf "%04x %04x: suffix changed the instruction" opcode ext;
      if Decode.at (view (Bytes.sub_string prefix 0 length)) base <> decoded then
        Alcotest.failf "%04x %04x: instruction depends on bytes outside its length" opcode ext
    done
  done

let brief () =
  expect [0x4ef0; 0x9e80]
    (Jmp (Indexed { base = Areg 0; disp = -128;
                    index = Some { reg = A 1; long = true; scale = 8 }; indirect = Direct }))

let index = Some { reg = D 0; long = false; scale = 1 }

let full () =
  expect [0x4efb; 0x01e3; 0xfffe; 0x7654; 0x3210]
    (Jmp (Indexed { base = Suppressed; disp = -2; index = None; indirect = Pre 0x76543210 }));
  expect [0x4efb; 0x0136; 0xffff; 0xfffc; 0xfffe]
    (Jmp (Indexed { base = Pc_at (base + 2); disp = -4; index; indirect = Post (-2) }));
  expect [0x23b0; 0x0133; 0; 0; 0; 0; 0x0137; 0; 0; 0; 0]
    (Move (Long, Indexed { base = Areg 0; disp = 0; index; indirect = Pre 0 },
                 Indexed { base = Areg 1; disp = 0; index; indirect = Post 0 }))

let reserved_base () =
  expect [0x4ef0; 0x0100] (Jmp (Indexed { base = Areg 0; disp = 0; index; indirect = Direct }))

let reserved_bit3 () =
  Alcotest.(check bool) "bit 3 leaves the encoding unchanged" true
    (decode [0x4ef0; 0x0118] = decode [0x4ef0; 0x0110])

let reserved_postindex () =
  expect [0x4ef0; 0x0104] (Jmp (Indexed { base = Areg 0; disp = 0; index; indirect = Post 0 }))

let reserved_index_suppressed () =
  List.iter (fun (selector, tail, outer) ->
    expect ([0x4ef0; 0x0140 lor selector] @ tail)
      (Jmp (Indexed { base = Areg 0; disp = 0; index = None; indirect = Pre outer })))
    [4, [], 0; 5, [], 0; 6, [0xfffe], -2; 7, [0; 8], 8]

let pc_prefixes () =
  let pc_rel = Pc_rel (base + 2) in
  expect [0xe8fa; 0; 0xfffe]
    (Bitfield { operation = Bftst; offset = Constant 0; width = Constant 32; operand = pc_rel });
  expect [0x00fa; 0x1800; 0xfffe] (Bounds (Check, Byte, pc_rel, D 1));
  expect [0x4cba; 0x0101; 0xfffe] (Movem (From_memory, Word, [D 0; A 0], pc_rel));
  expect [0x4c3a; 0x1800; 0xfffe] (Mul_long { sign = Signed; operand = pc_rel; low = 1; high = None });
  expect [0x083a; 0x1234; 0xfffe] (Bit (Btst, Constant 0x34, pc_rel));
  expect [0x0cba; 0x1234; 0x5678; 0xfffc] (Alu (Cmp, Long, Immediate 0x12345678, pc_rel));
  expect [0x23fa; 0; 0x1234; 0x5678] (Move (Long, pc_rel, Absolute 0x12345678));
  expect [0x2179; 0x1234; 0x5678; 0xfffe]
    (Move (Long, Absolute 0x12345678, Disp { reg = 0; disp = -2 }));
  expect [0x0af0; 0x0081; 0x0008]
    (Cas (Byte, { compare = 1; update = 2 }, Indexed { base = Areg 0; disp = 8; index; indirect = Direct }));
  expect [0x0e70; 0xa800; 0x0008]
    (Moves (To_memory, Word, A 2, Indexed { base = Areg 0; disp = 8; index; indirect = Direct }));
  illegal [0x0afa; 0; 0] 4 Decode.Unassigned 2;
  illegal [0x0e7a; 0; 0] 4 Decode.Unassigned 2;
  illegal [0x35c0; 0] 4 Decode.Unassigned 2

let bcd () =
  expect [0x830f] (Sbcd (Predec 7, Predec 1));
  expect [0x834f; 0xfffe] (Pack (Predec 7, Predec 1, -2));
  expect [0x838f; 0xfffe] (Unpk (Predec 7, Predec 1, -2));
  expect [0xc30f] (Abcd (Predec 7, Predec 1))

let divide () =
  let check ext sign dividend =
    expect [0x4c7a; ext; 0xfffe]
      (Div_long { sign; operand = Pc_rel (base + 2); dividend; quotient = 1; remainder = 3 })
  in
  check 0x1003 Unsigned (Single 1);
  check 0x1803 Signed (Single 1);
  check 0x1c03 Signed (Double { high = 3; low = 1 })

let cas2 () =
  let first = { address = A 1; registers = { compare = 3; update = 2 } } in
  let second = { address = D 7; registers = { compare = 5; update = 4 } } in
  expect [0x0cfc; 0x9083; 0x7105] (Cas2 (Word, first, second));
  expect [0x0efc; 0x9083; 0x7105] (Cas2 (Long, first, second))

let trapcc () =
  expect [0x56fa; 0x1234] (Trapcc (Ne, Some (Word, 0x1234)));
  expect [0x56fb; 0x1234; 0xabcd] (Trapcc (Ne, Some (Long, 0x1234abcd)));
  expect [0x56fc] (Trapcc (Ne, None))

let branches () =
  expect [0x66fe] (Bcc (Ne, base));
  expect [0x6600; 0xfffe] (Bcc (Ne, base));
  expect [0x66ff; 0xffff; 0xfffe] (Bcc (Ne, base));
  expect [0x61fe] (Bsr base);
  expect [0x6100; 0xfffe] (Bsr base);
  expect [0x61ff; 0xffff; 0xfffe] (Bsr base)

let movec () =
  List.iter (fun (code, control) ->
    expect [0x4e7a; 0xf000 lor code] (Movec (From_control, A 7, control));
    expect [0x4e7b; 0x3000 lor code] (Movec (To_control, D 3, control)))
    [0, Sfc; 1, Dfc; 2, Cacr; 0x800, Usp; 0x801, Vbr; 0x802, Caar; 0x803, Msp; 0x804, Isp];
  illegal [0x4e7b; 0xa003] 4 (Decode.Mame_default (Movec_control 3)) 4;
  Alcotest.(check (option int)) "privilege precedes control-code validation" (Some 8)
    (Decode.exception_vector ~supervisor:false (decode [0x4e7b; 0xa003]))

let f_line () =
  List.iter (fun word -> illegal [word] 11 (Decode.Mame_default F_line) 2)
    [0xf000; 0xf050; 0xf048; 0xf07a; 0xf280; 0xffff]

let a7 () =
  expect [0xbf0f] (Alu (Cmp, Byte, Postinc 7, Postinc 7));
  expect [0x1f1f] (Move (Byte, Postinc 7, Predec 7));
  Alcotest.(check int) "A7 byte step" 2 (auto_step Byte 7)

let flow () =
  let flow words = match decode words with
    | Decode.Insn insn -> Flow.of_insn insn |> Flow.to_tsv
    | Decode.Illegal _ -> Alcotest.fail "expected an instruction"
  in
  List.iter (fun (words, expected) -> Alcotest.(check string) "flow" expected (flow words))
    [[0x4eb8; 0x8000], "branch:0xff8000"; [0x50c8; 0xfffe], "next";
     [0x51c8; 0xfffe], "branch:0x001000,next"; [0x4e40], "escape";
     [0x56fc], "branch:next,escape"; [0x4e73], "escape"; [0x4ed0], "escape";
     [0x4efb; 0x0160; 0xfffe], "branch:0x001000";
     [0x4eb4; 0xfff0; 0x3c4e; 0xfff0], "branch:0x4efff0"]

let () =
  let test name = Alcotest.test_case name `Quick in
  Alcotest.run "68EC020" [
    "exhaustive", [test "primary words, extension patterns, legality and locality" exhaustive];
    "encodings", [
      test "brief index" brief;
      test "full index, suppression, signed displacements and maximum length" full;
      test "Mame_default reserved BD encodes null" reserved_base;
      test "Mame_default ignores full extension bit 3" reserved_bit3;
      test "Mame_default reserved postindex with null outer displacement" reserved_postindex;
      test "Mame_default reserved indirect selectors with index suppressed" reserved_index_suppressed;
      test "EA bases and ordering after extension prefixes" pc_prefixes;
      test "memory UNPK, PACK, SBCD and ABCD" bcd;
      test "CMPM and byte A7 stepping" a7;
      test "DIVUL, DIVSL and the double dividend" divide;
      test "CAS2 register pairs" cas2;
      test "TRAPcc operand words" trapcc;
      test "Bcc and BSR short, word and long" branches;
      test "A-line" (fun () -> illegal [0xa123] 10 Decode.A_line 2);
      test "Mame_default F-line including coprocessor placeholders" f_line;
      test "Mame_default BKPT" (fun () -> illegal [0x484d] 4 (Decode.Mame_default Breakpoint) 2);
      test "Mame_default MOVEC codes and privilege order" movec;
      test "ILLEGAL, unassigned and legal RESET" (fun () ->
        illegal [0x4afc] 4 Decode.Illegal_opcode 2;
        illegal [0x7100] 4 Decode.Unassigned 2;
        expect [0x4e70] Reset);
      test "register BTST with a byte-sized immediate EA" (fun () ->
        expect [0x013c; 0x1234] (Bit (Btst, Data_register 0, Immediate 0x34)));
      test "absolute short sign extension" (fun () -> expect [0x4ef8; 0x8000] (Jmp (Absolute 0xffff8000)));
      test "truncated extension and missing primary word" (fun () ->
        illegal [0x60ff; 0] 4 Decode.Truncated 2;
        illegal [] 4 Decode.Truncated 0);
      test "typed control flow" flow;
    ]
  ]
