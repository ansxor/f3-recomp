let header = "pc\tdisposition\tlength\tflow\treads\twrites\tlive_out\tidle\tchain_candidates"

let write output selection =
  (match Selection.check selection with
   | Ok () -> ()
   | Error violations ->
       List.map Selection.describe_violation violations
       |> String.concat "\n" |> Error.fail ~where:"selection" "%s");
  output_string output (header ^ "\n");
  Pc.Map.iter (fun pc ((entry : Selection.disposition), _) ->
    let instruction disposition (insn : Insn.t) =
      let flow = Flow.of_insn insn in
      let candidates =
        match Selection.chain_candidates selection insn flow with
        | [] -> "-"
        | pcs -> String.concat "," (List.map (Printf.sprintf "0x%06x") pcs)
      in
      disposition, string_of_int insn.length, Flow.to_tsv flow, candidates
    in
    let disposition, length, flow, candidates =
      match entry with
      | Exception_entry vector -> Printf.sprintf "exception:%d" vector, "-", "escape", "-"
      | Ram_exception vector -> Printf.sprintf "ram-exception:%d" vector, "-", "escape", "-"
      | Native insn -> instruction "native" insn
      | Ram insn -> instruction "ram" insn
    in
    Printf.fprintf output "0x%06x\t%s\t%s\t%s\t-\t-\t-\t-\t%s\n"
      pc disposition length flow candidates)
    selection.Selection.entries
