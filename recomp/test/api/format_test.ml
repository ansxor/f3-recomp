open F3recomp
open Insn
open F3recomp_test

let at pc op = { pc; length = 2; op }
let check_text expected op = Alcotest.(check string) expected expected (Format.to_string (at 0x1000 op))

let d n = Register (D n)
let a n = Register (A n)
let index ?(long = false) ?(scale = 1) reg = Some { reg; long; scale }

let moves () =
  check_text "move.l d0,(a0)+" (Move (Long, d 0, Postinc 0));
  check_text "move.w -$60ac(a5),d1" (Move (Word, Disp { reg = 5; disp = -0x60ac }, d 1));
  check_text "move.b #$ff,0(a0)" (Move (Byte, Immediate 0xff, Disp { reg = 0; disp = 0 }));
  check_text "move.l -(a7),d3" (Move (Long, Predec 7, d 3));
  check_text "movea.l $4a0000,a0" (Movea (Long, Absolute 0x4a0000, 0));
  check_text "move.w #5,$ffff8000" (Move (Word, Immediate 5, Absolute 0xffff8000));
  check_text "moveq #-1,d3" (Moveq (-1, 3));
  check_text "moveq #$7f,d0" (Moveq (127, 0));
  check_text "lea $00a1b2(pc),a0" (Lea (Pc_rel 0xa1b2, 0));
  check_text "pea (a3)" (Pea (Ind 3));
  check_text "exg d0,a1" (Exg (D 0, A 1));
  check_text "move sr,d0" (From_status (Sr, d 0));
  check_text "move d0,ccr" (To_status (Ccr, d 0));
  check_text "move a2,usp" (Move_usp (To_control, 2));
  check_text "movec cacr,d0" (Movec (From_control, D 0, Cacr));
  check_text "moves.w (a0),d1" (Moves (From_memory, Word, D 1, Ind 0))

let arithmetic () =
  check_text "adda.w d0,a1" (Adda (Word, d 0, 1));
  check_text "addq.w #4,a7" (Adda (Word, Immediate 4, 7));
  check_text "suba.l #$100,a0" (Suba (Long, Immediate 0x100, 0));
  check_text "addi.l #$64,d2" (Alu (Add, Long, Immediate 100, d 2));
  check_text "addq.b #1,(a0)" (Alu (Add, Byte, Immediate 1, Ind 0));
  check_text "subi.w #$20,d1" (Alu (Sub, Word, Immediate 0x20, d 1));
  check_text "cmpi.b #0,(a0)" (Alu (Cmp, Byte, Immediate 0, Ind 0));
  check_text "cmpm.b (a0)+,(a1)+" (Alu (Cmp, Byte, Postinc 0, Postinc 1));
  check_text "eor.l d1,(a0)" (Alu (Eor, Long, d 1, Ind 0));
  check_text "and.w (a0),d2" (Alu (And, Word, Ind 0, d 2));
  check_text "cmpa.l a1,a0" (Cmpa (Long, a 1, 0));
  check_text "addx.l d1,d2" (Addx (Long, d 1, d 2));
  check_text "neg.w d0" (Unary (Neg, Word, d 0));
  check_text "tas (a0)" (Unary (Tas, Byte, Ind 0));
  check_text "ext.w d0" (Ext (Byte_to_word, 0));
  check_text "extb.l d0" (Ext (Byte_to_long, 0));
  check_text "swap d3" (Swap 3);
  check_text "lsl.w #2,d0" (Shift (Lsl, Word, Constant 2, d 0));
  check_text "asr.l d1,d0" (Shift (Asr, Long, Data_register 1, d 0));
  check_text "roxl.w (a0)" (Shift (Roxl, Word, Constant 1, Ind 0));
  check_text "btst #7,(a0)" (Bit (Btst, Constant 7, Ind 0));
  check_text "bset d1,d0" (Bit (Bset, Data_register 1, d 0));
  check_text "mulu.w (a0),d1" (Mul_word (Unsigned, Ind 0, 1));
  check_text "muls.l d1,d2:d3" (Mul_long { sign = Signed; operand = d 1; low = 3; high = Some 2 });
  check_text "divu.l d1,d2" (Div_long { sign = Unsigned; operand = d 1; dividend = Single 2;
                                         quotient = 2; remainder = 2 });
  check_text "divsl.l d1,d3:d2" (Div_long { sign = Signed; operand = d 1; dividend = Single 2;
                                             quotient = 2; remainder = 3 });
  check_text "divs.l d1,d3:d2" (Div_long { sign = Signed; operand = d 1;
                                             dividend = Double { high = 3; low = 2 };
                                             quotient = 2; remainder = 3 });
  check_text "chk.w d1,d0" (Chk (Word, d 1, 0));
  check_text "cmp2.b (a0),a1" (Bounds (Compare, Byte, Ind 0, A 1));
  check_text "pack -(a0),-(a1),#$30" (Pack (Predec 0, Predec 1, 0x30));
  check_text "abcd d0,d1" (Abcd (d 0, d 1))

let bitfields () =
  check_text "bfextu d1{4:8},d2"
    (Bitfield { operation = Bfextu 2; offset = Constant 4; width = Constant 8; operand = d 1 });
  check_text "bfins d2,(a0){d3:5}"
    (Bitfield { operation = Bfins 2; offset = Data_register 3; width = Constant 5; operand = Ind 0 });
  check_text "bfset (a1){0:32}"
    (Bitfield { operation = Bfset; offset = Constant 0; width = Constant 32; operand = Ind 1 })

let control () =
  check_text "bra $009b32" (Bra 0x9b32);
  check_text "bsr $001234" (Bsr 0x1234);
  check_text "bne $001000" (Bcc (Ne, 0x1000));
  check_text "dbra d0,$001000" (Dbcc (F, 0, 0x1000));
  check_text "dbne d1,$001000" (Dbcc (Ne, 1, 0x1000));
  check_text "seq (a0)" (Scc (Eq, Ind 0));
  check_text "jmp (a0)" (Jmp (Ind 0));
  check_text "jsr $004736" (Jsr (Absolute 0x4736));
  check_text "rts" Rts;
  check_text "rte" Rte;
  check_text "rtd #-4" (Rtd (-4));
  check_text "trap #15" (Trap 15);
  check_text "trapne" (Trapcc (Ne, None));
  check_text "trapeq.w #$1234" (Trapcc (Eq, Some (Word, 0x1234)));
  check_text "link a6,#-8" (Link (6, -8));
  check_text "link.l a6,#-$10000" (Link (6, -0x10000));
  check_text "unlk a6" (Unlk 6);
  check_text "stop #$2700" (Stop 0x2700);
  check_text "ori #$700,sr" (Logic_status (Or, Sr, 0x700));
  check_text "andi #$f,ccr" (Logic_status (And, Ccr, 0xf));
  check_text "nop" Nop;
  check_text "reset" Reset;
  check_text "callm #2,(a0)" (Callm (2, Ind 0));
  check_text "rtm d0" (Rtm (D 0))

let lists () =
  check_text "movem.l d0-d2/a6,-(a7)" (Movem (To_memory, Long, [D 0; D 1; D 2; A 6], Predec 7));
  check_text "movem.l (a7)+,d0/d2-d3/a0-a1"
    (Movem (From_memory, Long, [A 1; D 3; D 2; D 0; A 0], Postinc 7));
  check_text "movem.w d7/a0,$10(a1)" (Movem (To_memory, Word, [A 0; D 7], Disp { reg = 1; disp = 0x10 }));
  check_text "movem.l d0-d7/a0-a6,-(a7)"
    (Movem (To_memory, Long, List.init 15 (fun i -> if i < 8 then D i else A (i - 8)), Predec 7))

let cas () =
  check_text "cas.w d1,d2,(a0)" (Cas (Word, { compare = 1; update = 2 }, Ind 0));
  check_text "cas2.l d3:d5,d2:d4,(a1):(d7)"
    (Cas2 (Long, { address = A 1; registers = { compare = 3; update = 2 } },
           { address = D 7; registers = { compare = 5; update = 4 } }))

let effective_addresses () =
  let ea expected value = Alcotest.(check string) expected expected (Format.ea_to_string value) in
  ea "-$80(a0,a1.l*8)" (Indexed { base = Areg 0; disp = -128; index = index ~long:true ~scale:8 (A 1);
                                  indirect = Direct });
  ea "$10(a2,d3.w)" (Indexed { base = Areg 2; disp = 0x10; index = index (D 3); indirect = Direct });
  ea "-$2(pc,d0.w*2)" (Indexed { base = Pc_at 0x1002; disp = -2; index = index ~scale:2 (D 0);
                                 indirect = Direct });
  ea "($1234)" (Indexed { base = Suppressed; disp = 0x1234; index = None; indirect = Direct });
  ea "([$10,a2,d3.w],$4)" (Indexed { base = Areg 2; disp = 0x10; index = index (D 3); indirect = Pre 4 });
  ea "([$10,a2],d3.w,$4)" (Indexed { base = Areg 2; disp = 0x10; index = index (D 3); indirect = Post 4 });
  ea "([a2])" (Indexed { base = Areg 2; disp = 0; index = None; indirect = Pre 0 });
  ea "([pc],d1.l)" (Indexed { base = Pc_at 0; disp = 0; index = index ~long:true (D 1); indirect = Post 0 });
  ea "#$12345678" (Immediate 0x12345678);
  ea "#9" (Immediate 9);
  ea "$ffff8000" (Absolute 0xffff8000);
  ea "$00ffff" (Absolute 0xffff);
  ea "$01234567" (Absolute 0x1234567)

let pieces () =
  let op = Move (Long, d 0, Postinc 0) in
  Alcotest.(check string) "mnemonic" "move.l" (Format.mnemonic op);
  Alcotest.(check string) "operands" "d0,(a0)+" (Format.operands op);
  Alcotest.(check string) "no operands" "" (Format.operands Rts);
  Alcotest.(check string) "no trailing space" "rts" (Format.to_string (at 0 Rts))

let decoded_cas2 () =
  (* Words 0efc 0000 0504 decode as CAS2.L with reserved bits ignored. *)
  let image = { Image.base = 0x1000; bytes = "\x0e\xfc\x00\x00\x05\x04" } in
  match Decode.at image 0x1000 with
  | Decode.Insn insn ->
      Alcotest.(check string) "text" "cas2.l d0:d4,d0:d4,(d0):(d0)" (Format.to_string insn)
  | Illegal _ -> Alcotest.fail "expected CAS2"

let reference_equal = Alcotest.testable
  (fun ppf (r : Format.reference) ->
    Stdlib.Format.fprintf ppf "%s target=%s a5=%s size=%d indexed=%b" (Format.role_name r.role)
      (Option.fold ~none:"-" ~some:(Printf.sprintf "%#x") r.target)
      (Option.fold ~none:"-" ~some:string_of_int r.a5_disp) r.size r.indexed) ( = )

let refs ?a5 op = Format.references ?a5 (at 0x2000 op)
let reference ?target ?a5_disp ?(size = 0) ?(indexed = false) role : Format.reference =
  { role; target; a5_disp; size; indexed }

let references () =
  let check name expected actual = Alcotest.(check (list reference_equal)) name expected actual in
  check "read and write"
    [reference Read ~target:0x4a0000 ~size:4; reference Write ~target:0x40fff0 ~a5_disp:(-16) ~size:4]
    (refs ~a5:0x410000 (Move (Long, Absolute 0x4a0000, Disp { reg = 5; disp = -16 })));
  check "a5 unknown keeps the displacement"
    [reference Read ~a5_disp:(-0x60ac) ~size:2]
    (refs (Move (Word, Disp { reg = 5; disp = -0x60ac }, d 1)));
  check "other address registers are dynamic" [] (refs (Move (Word, Disp { reg = 3; disp = 4 }, d 1)));
  check "registers and immediates" [] (refs (Move (Word, Immediate 4, d 1)));
  check "compare reads" [reference Read ~target:0x400010 ~size:2]
    (refs (Alu (Cmp, Word, Absolute 0x400010, d 0)));
  check "memory arithmetic is rmw" [reference Rmw ~target:0x400010 ~size:1]
    (refs (Alu (Add, Byte, d 0, Absolute 0x400010)));
  check "tst reads, clr writes"
    [reference Read ~target:0x400010 ~size:4] (refs (Unary (Tst, Long, Absolute 0x400010)));
  check "clr" [reference Write ~target:0x400010 ~size:2] (refs (Unary (Clr, Word, Absolute 0x400010)));
  check "bit test vs set" [reference Read ~target:0x400010 ~size:1]
    (refs (Bit (Btst, Constant 1, Absolute 0x400010)));
  check "bit set" [reference Rmw ~target:0x400010 ~size:1]
    (refs (Bit (Bset, Constant 1, Absolute 0x400010)));
  check "calls and branches"
    [reference Call ~target:0x1234] (refs (Bsr 0x1234));
  check "branch" [reference Branch ~target:0x1234] (refs (Bcc (Ne, 0x1234)));
  check "jsr absolute" [reference Call ~target:0x4736] (refs (Jsr (Absolute 0x4736)));
  check "jmp pc-relative" [reference Branch ~target:0x3000] (refs (Jmp (Pc_rel 0x3000)));
  check "computed transfers have no static target" [] (refs (Jmp (Ind 0)));
  check "lea" [reference Lea ~target:0x400000] (refs (Lea (Absolute 0x400000, 0)));
  check "lea from a5" [reference Lea ~target:0x400010 ~a5_disp:0x10]
    (refs ~a5:0x400000 (Lea (Disp { reg = 5; disp = 0x10 }, 0)));
  check "pc-relative lea" [reference Lea ~target:0x2800] (refs (Lea (Pc_rel 0x2800, 1)));
  check "movem width"
    [reference Write ~target:0x400000 ~size:12] (refs (Movem (To_memory, Long, [D 0; D 1; D 2], Absolute 0x400000)));
  check "indexed table"
    [reference Branch ~target:0x2100 ~indexed:true]
    (refs (Jmp (Indexed { base = Pc_at 0x2002; disp = 0xfe; index = index (D 0); indirect = Direct })));
  check "zero-based indexed"
    [reference Read ~target:0x400000 ~indexed:true ~size:2]
    (refs (Move (Word, Indexed { base = Suppressed; disp = 0x400000; index = index (D 0); indirect = Direct },
                 d 1)));
  check "memory indirect is dynamic" []
    (refs (Move (Word, Indexed { base = Suppressed; disp = 0x400000; index = None; indirect = Pre 0 }, d 1)));
  check "mirrored a5 target folds to 32 bits" [reference Read ~target:0xfffffff0 ~a5_disp:(-16) ~size:1]
    (refs ~a5:0 (Move (Byte, Disp { reg = 5; disp = -16 }, d 0)))

let formats_every_instruction () =
  let property = QCheck.Test.make ~count:20_000 ~name:"formatter totality"
    (QCheck.make Insn_gen.generate)
    (fun insn ->
      let text = Format.to_string insn in
      let mnemonic = Format.mnemonic insn.op in
      ignore (Format.references ~a5:0x400000 insn);
      mnemonic <> "" && String.starts_with ~prefix:mnemonic text
      && text = String.trim text && not (String.contains text '\n'))
  in
  QCheck.Test.check_exn ~rand:(Random.State.make [|0x3f3|]) property

let formats_every_decoding () =
  let prefix = Bytes.create 24 in
  List.iter (fun ext ->
    for opcode = 0 to 65535 do
      Bytes.set_uint16_be prefix 0 opcode;
      for index = 1 to 11 do Bytes.set_uint16_be prefix (2 * index) ext done;
      match Decode.at { Image.base = 0x1000; bytes = Bytes.to_string prefix } 0x1000 with
      | Decode.Illegal _ -> ()
      | Insn insn ->
          let text = Format.to_string insn in
          if text = "" || text <> String.trim text then
            Alcotest.failf "%04x %04x: bad text %S" opcode ext text
    done)
    [0x0000; 0x0130; 0x01e3; 0x8e45; 0xffff]

let () =
  let case name test = Alcotest.test_case name `Quick test in
  Alcotest.run "m68k format" [
    "text", [
      case "moves" moves; case "arithmetic" arithmetic; case "bit fields" bitfields;
      case "control flow" control; case "register lists" lists; case "CAS" cas;
      case "effective addresses" effective_addresses; case "pieces" pieces;
      case "decoded CAS2" decoded_cas2;
    ];
    "references", [case "static addresses" references];
    "totality", [
      case "every generated instruction" formats_every_instruction;
      case "every decodable opcode" formats_every_decoding;
    ]]
