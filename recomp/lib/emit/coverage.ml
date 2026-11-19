(* Which decoded ROM instructions the semantics cannot describe yet, counted
   by instruction constructor. Excluded ranges are skipped. *)
let unsupported ~(rom : Image.t) ~(exclusions : Exclusion.t list) =
  let counts = Hashtbl.create 64 in
  let excluded pc = Exclusion.find exclusions pc |> Option.is_some in
  let stop = rom.base + Image.length rom in
  let rec visit pc =
    if pc < stop then begin
      (match Decode.at rom pc with
       | Insn insn when not (excluded pc) ->
           (match Semantics.of_insn insn with
            | Unsupported _ ->
                let name = Insn.op_name insn.op in
                Hashtbl.replace counts name (1 + Option.value (Hashtbl.find_opt counts name) ~default:0)
            | Body _ -> ())
       | _ -> ());
      visit (pc + 2)
    end
  in
  visit rom.base;
  Hashtbl.to_seq counts |> List.of_seq |> List.sort compare

let write output counts =
  List.iter (fun (name, count) -> Printf.fprintf output "%s\t%d\n" name count) counts;
  Printf.fprintf output "total\t%d\n" (List.fold_left (fun sum (_, n) -> sum + n) 0 counts)
