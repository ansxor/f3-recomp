let header = "pc\tdisposition\tlength\tflow\treads\twrites\tlive_out\tidle\tchain_candidates"

let write output program =
  output_string output (header ^ "\n");
  Pc.Map.iter (fun pc decoded ->
    let ram = match Code_space.region program.Program.space pc with Ram _ -> true | _ -> false in
    let disposition, length, flow, candidates =
      match decoded with
      | Decode.Illegal { vector; _ } ->
          let prefix = if ram then "ram-exception" else "exception" in
          Printf.sprintf "%s:%d" prefix vector, "-", "escape", "-"
      | Decode.Insn insn ->
          let flow = Flow.of_insn insn in
          let candidates =
            match Program.chain_candidates program insn flow with
            | [] -> "-"
            | pcs -> String.concat "," (List.map (Printf.sprintf "0x%06x") pcs)
          in
          (if ram then "ram" else "native"), string_of_int insn.length, Flow.to_tsv flow, candidates
    in
    Printf.fprintf output "0x%06x\t%s\t%s\t%s\t-\t-\t-\t-\t%s\n"
      pc disposition length flow candidates)
    program.decoded
