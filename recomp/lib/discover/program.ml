type seeds = {
  vectors : Pc.Set.t;
  configured : Pc.Set.t;
  ram_entries : Pc.Set.t;
  ram_exits : Pc.Set.t;
}

type t = {
  game : Game_image.t;
  space : Code_space.t;
  decoded : Decode.decoded Pc.Map.t;
  functions : Pc.Set.t;
  tables : Jump_table.t Pc.Map.t;
  seeds : seeds;
}

let insn program pc =
  match Pc.Map.find_opt pc program.decoded with
  | Some (Decode.Insn insn) -> Some insn
  | _ -> None

let flow program pc = Option.map Flow.of_insn (insn program pc)

let chain_candidates program (instruction : Insn.t) flow =
  let eligible pc =
    pc < 0x200000 && Code_space.region program.space pc = Rom
    && Option.is_some (insn program pc)
  in
  let static =
    if flow.Flow.falls_through then Pc.physical (instruction.pc + instruction.length) :: flow.targets
    else flow.targets
  in
  let indirect =
    match Pc.Map.find_opt instruction.pc program.tables with
    | Some table -> table.candidates |> List.filter eligible |> List.take 256
    | None -> []
  in
  List.sort_uniq Int.compare (List.filter eligible static @ indirect)
