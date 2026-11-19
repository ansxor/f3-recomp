open F3recomp_test.Asm

let base = 0x1000
let seed = 0x68020
let samples = 120

let refine program =
  match Refinement.Harness.run ~seed ~samples ~base program with
  | Passed count ->
      Printf.printf "%d eager/lazy comparisons passed (seed %#x)\n%!" count seed
  | Mismatch message -> Alcotest.fail message

let status_and_branches () =
  let random = Random.State.make [|seed|] in
  let pick values = values.(Random.State.int random (Array.length values)) in
  let register () = Random.State.int random 8 in
  let generate index =
    let join = Printf.sprintf "join_%d" index in
    let status = pick [|Sr; Ccr|] in
    let condition = pick [|T; F; Hi; Ls; Cc; Cs; Ne; Eq; Vc; Vs; Pl; Mi; Ge; Lt; Gt; Le|] in
    let bits = Random.State.int random (if status = Sr then 0x4000 else 0x100) in
    [moveq (Random.State.int random 256 - 128) (d (register ()));
     insn (Logic_status (pick [|And; Or; Eor|], status, bits));
     pick [|bra join; bcc (if condition = T || condition = F then Ne else condition) join;
            dbcc condition (register ()) join|];
     insn (To_status (status, imm bits)); label join;
     insn (From_status (status, d (register ()))) ]
  in
  refine (List.concat (List.init 48 generate) @ [insn Nop])

let transfers () =
  let indirect =
    Indexed { base = Areg 7; disp = 8;
              index = Some { reg = D 2; long = false; scale = 4 }; indirect = Post 4 }
  in
  let width_cases width =
    [ insn (Moves (To_memory, width, A 0, Postinc 0));
      insn (Moves (From_memory, width, A 0, Predec 0));
      insn (Moves (From_memory, width, D 0, Ind 0)) ]
  in
  let control_cases control =
    [insn (Movec (To_control, D 2, control)); insn (Movec (From_control, A 3, control))]
  in
  refine
    (List.concat_map width_cases [Byte; Word; Long]
     @ List.concat_map control_cases [Sfc; Dfc; Cacr; Usp; Vbr; Caar; Msp; Isp]
     @ [insn (Move_usp (To_control, 2)); insn (Move_usp (From_control, 3));
        bsr "callee"; insn (Jsr indirect); insn (Jmp indirect);
        label "callee"; insn Rts; insn (Rtd (-16)); insn Rtr; insn Rte;
        insn (Trap 3); insn (Trapcc (Eq, Some (Long, 42))); insn Trapv;
        insn Reset; insn (Stop 0x2000)])

let () =
  Alcotest.run "family e"
    ["refinement",
     [Alcotest.test_case "seeded status and branches" `Quick status_and_branches;
      Alcotest.test_case "control transfers" `Quick transfers]]
