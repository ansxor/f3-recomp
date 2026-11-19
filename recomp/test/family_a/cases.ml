open F3recomp
open F3recomp_test.Asm

let seed = 0x68020
let samples = 120

let compare name program =
  match Refinement.Harness.run ~seed ~samples ~base:0x1000 program with
  | Passed count ->
      Printf.printf "%s: %d eager/lazy comparisons passed (seed %#x)\n%!" name count seed
  | Mismatch message -> Alcotest.failf "%s: %s" name message

let indexed base indirect =
  Insn.Indexed
    { base; disp = 16; index = Some { reg = D 1; long = false; scale = 2 }; indirect }

let aliases =
  [ move Byte (Postinc 7) (Predec 7);
    move Long (Predec 0) (Predec 0);
    move Long (Postinc 0) (Postinc 0);
    move Long (indexed (Areg 0) (Pre 4)) (indexed (Areg 7) (Post (-4)));
    insn (Movea (Word, Postinc 2, 2));
    insn (Movea (Word, imm 0x8000, 2));
    move Word (Register (A 2)) (Absolute 0x5000);
    move Long (Pc_rel 0x1020) (d 0);
    insn (Movea (Long, indexed Suppressed Direct, 5));
    insn (Lea (indexed (Areg 0) (Post 8), 0));
    insn (Pea (indexed (Areg 7) (Pre 4)));
    insn (Movem (To_memory, Long, [A 0; D 0; A 7], Predec 0));
    insn (Movem (From_memory, Word, [A 0; D 0; A 7], Postinc 0));
    insn (Movem (From_memory, Long, [], indexed (Areg 0) (Post 4)));
    insn (Movep (Word, Disp { reg = 0; disp = -1 }, d 0));
    insn (Movep (Long, d 0, Disp { reg = 0; disp = -1 }));
    insn (Exg (D 0, A 7));
    insn (Link (7, -12)); insn (Unlk 7);
    insn (Link (6, -0x10000)); insn (Unlk 6) ]

let random_program random =
  let choose values = values.(Random.State.int random (Array.length values)) in
  let reg () = Random.State.int random 8 in
  let displacement () = Random.State.int random 64 - 32 in
  let address () =
    let r = reg () in
    choose [|Insn.Ind r; Disp { reg = r; disp = displacement () };
             indexed (Areg r) Direct; indexed (Areg r) (Pre 4);
             indexed (Areg r) (Post 4); Absolute 0x5000|]
  in
  let data () = choose [|d (reg ()); Postinc (reg ()); Predec (reg ()); address ()|] in
  let word_or_long () = choose [|Insn.Word; Long|] in
  let registers () = [Insn.D (reg ()); A (reg ())] in
  let instruction () =
    match Random.State.int random 10 with
    | 0 -> move (choose [|Byte; Word; Long|]) (data ()) (data ())
    | 1 -> insn (Movea (word_or_long (), data (), reg ()))
    | 2 -> moveq (Random.State.int random 256 - 128) (d (reg ()))
    | 3 -> insn (Lea (address (), reg ()))
    | 4 -> insn (Pea (address ()))
    | 5 ->
        let ea = address () in
        let direction, ea =
          choose [|Insn.To_memory, Predec (reg ()); From_memory, Postinc (reg ());
                   To_memory, ea; From_memory, ea|]
        in
        insn (Movem (direction, word_or_long (), registers (), ea))
    | 6 ->
        let mem = Insn.Disp { reg = reg (); disp = displacement () } in
        let data = d (reg ()) in
        let src, dst = choose [|mem, data; data, mem|] in
        insn (Movep (word_or_long (), src, dst))
    | 7 ->
        let left, right = choose [|Insn.D (reg ()), D (reg ());
                                   A (reg ()), A (reg ()); D (reg ()), A (reg ())|] in
        insn (Exg (left, right))
    | 8 -> insn (Link (reg (), displacement ()))
    | _ -> insn (Unlk (reg ()))
  in
  List.init 48 (fun _ -> instruction ())

let () =
  let random = Random.State.make [|seed|] in
  let straight_line = random_program random in
  let before = random_program random in
  let fallthrough = random_program random in
  let after = random_program random in
  let forward_branches = before @ [bcc Eq "join"] @ fallthrough @ [label "join"] @ after in
  let case name program = Alcotest.test_case name `Quick (fun () -> compare name program) in
  Alcotest.run "Move family"
    ["eager versus lazy",
     [case "aliasing and effective addresses" aliases;
      case "random straight line" straight_line;
      case "random forward branches" forward_branches]]
