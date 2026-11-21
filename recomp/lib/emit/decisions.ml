let header = "pc\tdisposition\tlength\tflow\treads\twrites\tlive_out\tidle\tchain_candidates"

let flags mask =
  if mask = Ccr.none then "-" else Ccr.to_string mask

let render ?(sprite_rows = []) ?(groups = Sprite_groups.empty) selection =
  let output = Buffer.create 65536 in
  (match Selection.check selection with
   | Ok () -> ()
   | Error violations ->
       List.map Selection.describe_violation violations
       |> String.concat "\n" |> Error.fail ~where:"selection" "%s");
  let remaining = ref sprite_rows in
  let rec emit_sprite_rows include_row =
    match !remaining with
    | (pc, row) :: rest when include_row pc ->
        Buffer.add_string output row;
        remaining := rest;
        emit_sprite_rows include_row
    | _ -> ()
  in
  let semantics = Cfg.semantics selection in
  let cfg = Cfg.build selection semantics ~pins:(Pins.collect selection semantics groups) in
  let liveness = Liveness.solve ~yields:(Liveness.node_pcs cfg) cfg in
  let native pc = Cfg.native cfg pc in
  let idle = Idle_loop.find selection.program ~native in
  (match Idle_loop.check_all selection.program cfg idle with
   | Ok () -> ()
   | Error violations ->
       List.map Idle_loop.describe_violation violations
       |> String.concat "\n" |> Error.fail ~where:"idle loops" "%s");
  Buffer.add_string output (header ^ "\n");
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
    Printf.bprintf output "0x%06x\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n"
      pc disposition length flow reads writes live_out idle candidates;
    emit_sprite_rows (Int.equal pc))
    selection.Selection.entries;
  emit_sprite_rows (fun _ -> true);
  Buffer.contents output

let write ?sprite_rows ?groups output selection =
  output_string output (render ?sprite_rows ?groups selection)

let read path =
  let fail line fmt =
    Printf.ksprintf (fun message -> Error.fail ~where:path "line %d: %s" line message) fmt
  in
  let parse_flags line text =
    if text = "-" then Ccr.none
    else
      let mask = String.fold_left (fun mask char ->
        let flag = match char with
          | 'X' -> Ccr.X | 'N' -> N | 'Z' -> Z | 'V' -> V | 'C' -> C
          | _ -> fail line "invalid live_out %S" text in
        Ccr.union mask (Ccr.singleton flag)) Ccr.none text in
      if text <> flags mask then fail line "invalid live_out %S" text;
      mask
  in
  In_channel.with_open_text path (fun input ->
    if In_channel.input_line input <> Some header then fail 1 "invalid decisions header";
    let rec rows line live_out idle =
      match In_channel.input_line input with
      | None -> live_out, idle
      | Some text ->
          match String.split_on_char '\t' text with
          | [address; disposition; _; _; _; _; out; shape; _] ->
              let pc = match int_of_string_opt address with
                | Some pc when pc = Pc.physical pc && pc land 1 = 0 -> pc
                | _ -> fail line "invalid PC %S" address in
              let live_out, idle = match disposition with
                | "sprite-writer" | "sprite-reset" -> live_out, idle
                | kind when String.starts_with ~prefix:"exception:" kind ||
                            String.starts_with ~prefix:"ram-exception:" kind ->
                    if out <> "-" || shape <> "-" then
                      fail line "exception at %#x has instruction decisions" pc;
                    live_out, idle
                | "native" | "fallback" | "ram" | "ram-fallback" ->
                    if Pc.Map.mem pc live_out then fail line "duplicate instruction at %#x" pc;
                    let idle = match shape with
                      | "-" -> idle
                      | "spin" | "poll" | "affine" -> Pc.Map.add pc shape idle
                      | _ -> fail line "invalid idle shape %S at %#x" shape pc in
                    Pc.Map.add pc (parse_flags line out) live_out, idle
                | _ -> fail line "invalid disposition %S" disposition in
              rows (line + 1) live_out idle
          | _ -> fail line "expected nine TSV columns"
    in
    rows 2 Pc.Map.empty Pc.Map.empty)
