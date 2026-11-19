open F3recomp_test.Asm

let seed = 0xf313

let program random =
  let choose values = values.(Random.State.int random (Array.length values)) in
  let reg () = Random.State.int random 8 in
  let data () = d (reg ()) in
  let widths = [|Byte; Word; Long|] in
  let operations = [|Add; Sub; Cmp; And; Or; Eor|] in
  let random_instruction index =
    let width = choose widths in
    (* Memory operands use A7, which the other instructions leave alone. *)
    let operand () = choose [|data (); Ind 7; Postinc 7; Predec 7|] in
    let immediate () = imm (choose [|-32768; -1; 0; 1; 8; 127; 255|]) in
    insn
      (match index mod 16 with
       | 0 -> Alu (choose [|Add; Sub; And; Or; Eor|], width, data (), operand ())
       | 1 -> Alu (choose [|Add; Sub; Cmp; And; Or|], width, operand (), data ())
       | 2 -> Alu (choose operations, width, immediate (), operand ())
       | 3 -> Alu (choose [|Add; Sub|], width, imm (1 + reg ()), operand ())
       | 4 -> Alu (Cmp, width, Postinc 7, Postinc 7)
       | 5 -> Adda (Word, immediate (), 0)
       | 6 -> Suba (Long, Postinc 7, 1)
       | 7 -> Cmpa (Word, data (), reg ())
       | 8 -> Addx (width, data (), data ())
       | 9 -> Subx (width, data (), data ())
       | 10 -> Addx (width, Predec 7, Predec 7)
       | 11 -> Subx (width, Predec 7, Predec 7)
       | 12 -> Unary (Negx, width, operand ())
       | 13 -> Unary (choose [|Neg; Not; Clr; Tst|], width, operand ())
       | 14 -> Ext (choose [|Byte_to_word; Byte_to_long; Word_to_long|], reg ())
       | _ -> Swap (reg ()))
  in
  List.init 64 random_instruction

let refinement () =
  let random = Random.State.make [|seed|] in
  let samples = 120 in
  let comparisons =
    List.init 4 (fun index ->
        match Refinement.Harness.run ~seed:(seed + index) ~samples ~base:0x1000 (program random) with
        | Passed count -> count
        | Mismatch message -> Alcotest.failf "program %d: %s" index message)
    |> List.fold_left ( + ) 0
  in
  Printf.printf "%d eager/lazy comparisons passed (program seed %#x)\n%!" comparisons seed

let () =
  Alcotest.run "ALU semantics"
    ["eager versus lazy", [Alcotest.test_case "random programs" `Quick refinement]]
