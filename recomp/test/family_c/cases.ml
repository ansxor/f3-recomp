open F3recomp
open F3recomp_test
open Asm

let seed = 0xc031
let base = 0x1000
let samples = 120

let shifts = [|Insn.Asr; Asl; Lsr; Lsl; Roxr; Roxl; Ror; Rol|]
let bits = [|Insn.Btst; Bchg; Bclr; Bset|]
let conditions =
  [|Insn.T; F; Hi; Ls; Cc; Cs; Ne; Eq; Vc; Vs; Pl; Mi; Ge; Lt; Gt; Le|]

let bitfields reg =
  [|Insn.Bftst; Bfextu reg; Bfchg; Bfexts reg; Bfclr; Bfffo reg; Bfset; Bfins reg|]

let check name program =
  match Refinement.Harness.run ~seed ~samples ~base program with
  | Passed count ->
      Printf.printf "%s: %d eager/lazy comparisons passed (seed %#x)\n%!" name count seed
  | Mismatch message -> Alcotest.failf "%s: %s" name message

let random_programs () =
  let rng = Random.State.make [|seed|] in
  let number limit = Random.State.int rng limit in
  let choose values = values.(number (Array.length values)) in
  let reg () = number 8 in
  let memory () = choose [|Insn.Ind (reg ()); Postinc 7; Predec (reg ())|] in
  let operand () = if Random.State.bool rng then d (reg ()) else memory () in
  let count limit base =
    if Random.State.bool rng then Insn.Data_register (reg ())
    else Constant (base + number limit)
  in
  let instruction family index =
    let op = match family with
      | 0 ->
          let operation = shifts.(index mod Array.length shifts) in
          if Random.State.bool rng then
            Insn.Shift (operation, choose [|Byte; Word; Long|], count 8 1, d (reg ()))
          else Shift (operation, Word, Constant 1, memory ())
      | 1 -> Bit (bits.(index mod Array.length bits), count 256 0, operand ())
      | 2 ->
          let operations = bitfields (reg ()) in
          let operand = choose [|d (reg ()); Ind (reg ());
                                 Disp { reg = reg (); disp = number 16 - 8 }|] in
          Bitfield { operation = operations.(index mod Array.length operations);
                     offset = count 32 0; width = count 32 1; operand }
      | _ -> Scc (conditions.(index mod Array.length conditions), operand ())
    in
    insn op
  in
  for program = 1 to 4 do
    let generated = List.concat (List.init 16 (fun index ->
        let group = List.init 4 (fun family -> instruction family index) in
        (* Seed pending producers among the family instructions so a later
           reader can distinguish live flags from flags the lowerer elided. *)
        let dst = d (reg ()) in
        let second = if index mod 2 = 0 then add Long (d (reg ())) dst else moveq 0 dst in
        second :: group)) in
    check (Printf.sprintf "random program %d" program) generated
  done

let forward_branches () =
  check "forward branches"
    [add Byte (d 0) (d 1);
     insn (Shift (Roxl, Long, Data_register 1, d 1));
     bcc Cs "insert";
     insn (Bit (Bchg, Data_register 1, Postinc 7));
     insn (Scc (Ne, Predec 7));
     bra "join";
     label "insert";
     insn (Bitfield { operation = Bfins 1; offset = Data_register 1;
                      width = Data_register 1; operand = Ind 0 });
     insn (Scc (Mi, d 1));
     label "join";
     insn (Shift (Asr, Word, Constant 1, Postinc 7));
     insn (Scc (Vs, Postinc 7))]

let () =
  Alcotest.run "family C refinement"
    ["eager versus lazy",
     [Alcotest.test_case "random programs" `Quick random_programs;
      Alcotest.test_case "forward branches" `Quick forward_branches]]
