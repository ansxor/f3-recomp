open F3recomp
open F3recomp_test
open Asm

let base = 0x1000
let seed = 0xf312
let samples = 120

let check name program =
  match Refinement.Harness.run ~seed ~samples ~base program with
  | Passed count ->
      Printf.printf "%s: %d eager/lazy comparisons passed (seed %#x)\n%!" name count seed
  | Mismatch message -> Alcotest.failf "%s: %s" name message

let straight_line () =
  let random = Random.State.make [|seed|] in
  let choose values = values.(Random.State.int random (Array.length values)) in
  let reg () = d (Random.State.int random 8) in
  let size () = choose [|Byte; Word; Long|] in
  let generated =
    List.init 40 (fun _ ->
        let width = size () in
        match Random.State.int random 4 with
        | 0 -> moveq (Random.State.int random 256 - 128) (reg ())
        | 1 -> move width (reg ()) (reg ())
        | 2 -> insn (Addx (width, reg (), reg ()))
        | _ -> insn (Alu (choose [|Add; Sub; Cmp; And; Or; Eor|], width, reg (), reg ())))
  in
  let open Refinement.Harness in
  let producer pc op =
    let insn = { F3recomp.Insn.pc; length = 2; op } in
    { pc; sem = Semantics.of_insn insn }
  in
  let live = local_liveness
      [producer base (Alu (Add, Long, d 0, d 1)); producer (base + 2) (Moveq (0, 2))]
  in
  Alcotest.(check string) "MOVEQ preserves only X from ADD" "X" (Ccr.to_string (live base));
  check "straight line"
    ([ add Long (d 0) (d 1); moveq 0 (d 2); insn (Addx (Long, d 2, d 1));
       insn (Alu (Sub, Long, d 0, d 1)); add Long (d 1) (d 2);
       move Long (d 2) (Predec 1); move Byte (Postinc 7) (d 3);
       insn (Movea (Word, d 3, 2)); insn (Alu (Cmp, Word, d 2, d 3));
       insn (Alu (And, Byte, d 0, d 1)); insn (Alu (Or, Word, d 0, d 1));
       insn (Alu (Eor, Long, d 0, d 1)); insn (Addx (Long, Predec 1, Predec 2));
       insn (Bit (Btst, Constant 7, d 3));
       insn (Bit (Btst, Data_register 3, Ind 2));
       insn (From_status (Ccr, d 4)); insn (To_status (Ccr, imm 0x1f)); insn Nop ]
     @ generated
     @ [insn (From_status (Ccr, d 5))])

let forward_branches () =
  check "forward branches"
    [ insn (Alu (Cmp, Long, d 0, d 1)); bcc Ne "check_counter";
      moveq 0 (d 2); bra "join";
      label "check_counter"; dbcc Eq 3 "join"; moveq (-1) (d 2);
      label "join"; move Byte (d 2) (Postinc 0); insn (From_status (Ccr, d 5)) ]

let boundaries () =
  check "runtime and control boundaries"
    [ insn (Alu (Cmp, Byte, d 0, d 1)); insn (Trapcc (Eq, None)); insn Trapv;
      insn (From_status (Sr, d 0)); insn (Move_usp (From_control, 0));
      insn (Move_usp (To_control, 1)); insn Reset;
      insn (To_status (Sr, imm 0x2000)); insn (Stop 0x2700);
      insn (Jsr (Ind 0)); insn Rts; insn (Trap 2) ]

let broken_specialisation () =
  let open Refinement.Harness in
  let program = [moveq 0 (d 0); bcc Eq "join"; moveq 1 (d 1); label "join"; insn Nop] in
  check "mutation control" program;
  let liveness entries =
    let live = local_liveness entries in
    fun pc -> if pc = base then Ccr.none else live pc
  in
  match run ~liveness ~seed ~samples ~base program with
  | Mismatch message -> Printf.printf "dropped live MOVEQ flags: %s\n%!" message
  | Passed count -> Alcotest.failf "broken specialisation survived %d comparisons" count

let () =
  let case name test = Alcotest.test_case name `Quick test in
  Alcotest.run "C refinement"
    [ "eager versus lazy",
      [case "straight line" straight_line; case "forward branches" forward_branches;
       case "runtime and control boundaries" boundaries];
      "negative control", [case "drop a live producer" broken_specialisation] ]
