let header = "pc\tdisposition\tlength\tflow\treads\twrites\tlive_out\tidle\tchain_candidates"

let write_row output pc decoded =
  let length, flow = match decoded with
    | Decode.Insn insn -> string_of_int insn.length, Flow.of_insn insn |> Flow.to_tsv
    | Decode.Illegal _ -> "-", "escape"
  in
  Printf.fprintf output "0x%06x\t-\t%s\t%s\t-\t-\t-\t-\t-\n" pc length flow

let write_rom output rom exclusions =
  let write_range start stop =
    for word = start / 2 to stop / 2 - 1 do
      let pc = word * 2 in
      Decode.at rom pc |> write_row output pc
    done
  in
  let start =
    List.fold_left (fun start (range : Exclusion.t) ->
      write_range start range.start;
      range.stop)
      rom.Image.base exclusions
  in
  write_range start (rom.base + Image.length rom)

(* Baseline compares the reachable RAM entries in the frozen decision dump.
   T8 owns the production RAM code discovery strategy. *)
let ram_entries rom (spec : Config.ram_code) =
  let payload = Bytes.create spec.length in
  for word = 0 to spec.length / 4 - 1 do
    let offset = word * 4 in
    let value = match spec.transform with
      | Negate_be32 -> Image.s32 rom (spec.source + offset) |> Int32.of_int |> Int32.neg
    in
    Bytes.set_int32_be payload offset value
  done;
  let image : Image.t = { base = spec.destination; bytes = Bytes.unsafe_to_string payload } in
  let rec visit entries = function
    | [] -> entries
    | pc :: rest when Pc.Map.mem pc entries -> visit entries rest
    | pc :: rest ->
        if pc land 1 <> 0 || not (Image.contains image pc) then
          Error.fail ~where:"decisions" "RAM code path leaves payload at %#x" pc;
        match Decode.at image pc with
        | Decode.Illegal _ ->
            Error.fail ~where:"decisions" "illegal RAM instruction at %#x" pc
        | Decode.Insn insn as decoded ->
            let flow = Flow.of_insn insn in
            let targets = List.filter (Image.contains image) flow.targets in
            let returns = match insn.op with Insn.Bsr _ | Jsr _ -> true | _ -> false in
            let pending = targets @ rest in
            let pending = if flow.falls_through || returns then pc + insn.length :: pending else pending in
            visit (Pc.Map.add pc decoded entries) pending
  in
  visit Pc.Map.empty [spec.entry]

let write output ~rom (config : Config.t) =
  let exclusions = Exclusion.of_config ~image:rom config.exclusions.main in
  output_string output (header ^ "\n");
  write_rom output rom exclusions;
  List.sort (fun (a : Config.ram_code) (b : Config.ram_code) -> Int.compare a.destination b.destination)
    config.ram_code
  |> List.iter (fun spec -> ram_entries rom spec |> Pc.Map.iter (write_row output))
