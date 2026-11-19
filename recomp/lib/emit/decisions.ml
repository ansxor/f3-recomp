let header = "pc\tdisposition\tlength\tflow\treads\twrites\tlive_out\tidle\tchain_candidates"

let flags mask =
  if mask = Ccr.none then "-" else Ccr.to_string mask

let write ?(sprite_rows = []) ?(sprite_resets = Pc.Set.empty) output selection =
  (match Selection.check selection with
   | Ok () -> ()
   | Error violations ->
       List.map Selection.describe_violation violations
       |> String.concat "\n" |> Error.fail ~where:"selection" "%s");
  let remaining = ref sprite_rows in
  let rec emit_sprite_rows include_row =
    match !remaining with
    | (pc, row) :: rest when include_row pc ->
        output_string output row;
        remaining := rest;
        emit_sprite_rows include_row
    | _ -> ()
  in
  let cfg = Cfg.of_selection selection in
  let liveness = Liveness.solve cfg in
  let native pc = Cfg.native cfg pc && not (Pc.Set.mem pc sprite_resets) in
  let idle = Idle_loop.find selection.program ~native in
  (match Idle_loop.check_all selection.program cfg idle with
   | Ok () -> ()
   | Error violations ->
       List.map Idle_loop.describe_violation violations
       |> String.concat "\n" |> Error.fail ~where:"idle loops" "%s");
  output_string output (header ^ "\n");
  Pc.Map.iter (fun pc ((entry : Selection.disposition), _) ->
    emit_sprite_rows (fun sprite_pc -> sprite_pc < pc);
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
    let reads, writes, live_out = match Pc.Map.find_opt pc cfg.nodes with
      | None -> "-", "-", "-"
      | Some node ->
          flags node.effects.reads, flags node.effects.writes,
          flags (Liveness.live_out liveness pc ~flags:`Lazy)
    in
    let idle = match Pc.Map.find_opt pc idle with
      | None -> "-"
      | Some loop -> Idle_loop.to_tsv loop.shape
    in
    Printf.fprintf output "0x%06x\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n"
      pc disposition length flow reads writes live_out idle candidates;
    emit_sprite_rows (Int.equal pc))
    selection.Selection.entries;
  emit_sprite_rows (fun _ -> true)
