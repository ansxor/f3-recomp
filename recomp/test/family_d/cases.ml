open F3recomp_test.Asm

let seed = 0xd68020

let refinement () =
  let rng = Random.State.make [|seed|] in
  let pick bound = Random.State.int rng bound in
  let reg () = pick 8 in
  let sign () = if pick 2 = 0 then Signed else Unsigned in
  let width () = match pick 3 with 0 -> Byte | 1 -> Word | _ -> Long in
  let pair () =
    let src = reg () and dst = reg () in
    if pick 2 = 0 then d src, d dst else Predec src, Predec dst
  in
  let operand () =
    match pick 5 with
    | 0 -> d (reg ())
    | 1 -> Ind (reg ())
    | 2 -> Predec (reg ())
    | 3 -> Postinc (reg ())
    | _ -> imm (pick 0x10000 - 0x8000)
  in
  let alterable () = if pick 2 = 0 then d (reg ()) else Postinc (reg ()) in
  let cas_registers () = { compare = reg (); update = reg () } in
  let cas_operand () =
    let address = if pick 2 = 0 then D (reg ()) else A (reg ()) in
    { address; registers = cas_registers () }
  in
  let instructions =
    [| (fun () -> Mul_word (sign (), operand (), reg ()));
       (fun () -> Mul_long { sign = sign (); operand = operand (); low = reg ();
                              high = if pick 2 = 0 then None else Some (reg ()) });
       (fun () -> Div_word (sign (), operand (), reg ()));
       (fun () ->
          let quotient = reg () and remainder = reg () in
          let dividend = if pick 2 = 0 then Single quotient
            else Double { low = quotient; high = remainder } in
          Div_long { sign = sign (); operand = operand (); dividend; quotient; remainder });
       (fun () -> let src, dst = pair () in Abcd (src, dst));
       (fun () -> let src, dst = pair () in Sbcd (src, dst));
       (fun () -> Unary (Nbcd, Byte, alterable ()));
       (fun () -> Unary (Tas, Byte, alterable ()));
       (fun () -> let src, dst = pair () in Pack (src, dst, pick 0x10000 - 0x8000));
       (fun () -> let src, dst = pair () in Unpk (src, dst, pick 0x10000 - 0x8000));
       (fun () -> Cas (width (), cas_registers (), Postinc (reg ())));
       (fun () -> Cas2 ((if pick 2 = 0 then Word else Long), cas_operand (), cas_operand ()));
       (fun () -> Chk ((if pick 2 = 0 then Word else Long), operand (), reg ()));
       (fun () -> Bounds ((if pick 2 = 0 then Check else Compare), width (), Ind (reg ()), D (reg ()))) |]
  in
  let count = Array.length instructions in
  let all_forms = Array.to_list (Array.map (fun make -> insn (make ())) instructions) in
  let total = ref 0 in
  for _ = 1 to 4 do
    let random = List.init 40 (fun _ -> insn (instructions.(pick count) ())) in
    let divide_by_zero = [insn (Div_word (Signed, imm 0, 0))] in
    let program = all_forms @ random @ divide_by_zero in
    match Refinement.Harness.run ~seed ~samples:120 ~base:0x1000 program with
    | Passed comparisons -> total := !total + comparisons
    | Mismatch reason -> Alcotest.fail reason
  done;
  Printf.printf "%d family d eager/lazy comparisons passed (seed %#x)\n%!" !total seed

let () =
  Alcotest.run "family d"
    ["eager versus lazy", [Alcotest.test_case "random arithmetic programs" `Quick refinement]]
