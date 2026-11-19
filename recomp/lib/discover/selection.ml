type disposition =
  | Native of Insn.t
  | Exception_entry of int
  | Ram of Insn.t
  | Ram_exception of int

type tier = Hot | Cold
type mode = Baseline | Tiers | Slim
type profile = [ `Tiers of Profile.t | `Slim of Profile.t ]

type t = {
  program : Program_core.t;
  config : Config.t;
  units : Emit_units.t;
  mode : mode;
  entries : (disposition * tier) Pc.Map.t;
}

type violation =
  | Undecoded_hook of { unit_name : string; kind : string; pc : int }
  | Dropped_hook of { unit_name : string; kind : string; pc : int }
  | Inaccessible_entry of int
  | No_entries
  | No_executable_hits

let disposition space pc decoded =
  match Code_space.region space pc, decoded with
  | Rom, Decode.Insn insn -> Some (Native insn)
  | Rom, Decode.Illegal { vector; _ } -> Some (Exception_entry vector)
  | Ram _, Decode.Insn insn -> Some (Ram insn)
  | Ram _, Decode.Illegal { vector; _ } -> Some (Ram_exception vector)
  | (Excluded _ | Outside), _ -> None

let of_program ~(profile : profile option) program =
  let config = program.Program_core.game.config in
  let hot profile = Profile.hot profile Main program.game.rom ~exclusions:program.game.exclusions in
  let mode, hot =
    match profile with
    | None -> Baseline, Pc.Set.empty
    | Some (`Tiers profile) -> Tiers, hot profile
    | Some (`Slim profile) -> Slim, hot profile
  in
  let entries =
    Pc.Map.filter_map (fun pc decoded ->
      match mode, disposition program.space pc decoded with
      | _, None -> None
      | Baseline, Some entry | _, Some ((Ram _ | Ram_exception _) as entry) -> Some (entry, Hot)
      | (Tiers | Slim), Some entry when Pc.Set.mem pc hot -> Some (entry, Hot)
      | Tiers, Some entry -> Some (entry, Cold)
      | Slim, Some _ -> None)
      program.decoded
  in
  { program; config; units = Emit_units.of_config config; mode; entries }

let check selection =
  let hot_rom = Pc.Map.exists (fun _ -> function
    | (Native _ | Exception_entry _), Hot -> true | _ -> false) selection.entries in
  let violations =
    match selection.mode with
    | Baseline when Pc.Map.is_empty selection.entries -> [No_entries]
    | (Tiers | Slim) when not hot_rom -> [No_executable_hits]
    | _ -> []
  in
  let violations = Pc.Map.fold (fun pc _ violations ->
    match Code_space.region selection.program.space pc with
    | Rom | Ram _ -> violations
    | Excluded _ | Outside -> Inaccessible_entry pc :: violations)
    selection.program.decoded violations
  in
  let violations = List.fold_left (fun violations (unit : Config.emit_unit) ->
    let check_hooks kind pcs violations =
      List.fold_left (fun violations pc ->
        let unit_name = unit.name in
        match Code_space.region selection.program.space pc, Program_core.insn selection.program pc with
        | Rom, Some _ when Pc.Map.mem pc selection.entries -> violations
        | Rom, Some _ -> Dropped_hook { unit_name; kind; pc } :: violations
        | _ -> Undecoded_hook { unit_name; kind; pc } :: violations)
        violations pcs
    in
    violations |> check_hooks "start" unit.starts |> check_hooks "end" unit.ends)
    violations selection.config.video.emit_units
  in
  match violations with [] -> Ok () | _ -> Error (List.rev violations)

let describe_violation = function
  | Undecoded_hook { unit_name; kind; pc } ->
      Printf.sprintf "emit unit %S %s %#x is not a decoded instruction" unit_name kind pc
  | Dropped_hook { unit_name; kind; pc } ->
      Printf.sprintf "emit unit %S %s %#x is not retained by the slim profile; add it to the profile or use tiers"
        unit_name kind pc
  | Inaccessible_entry pc -> Printf.sprintf "discovery entry at inaccessible PC %#x" pc
  | No_entries -> "discovery produced no instructions"
  | No_executable_hits -> "profile contains no matching main executable hits"

let chain_candidates selection insn flow =
  Program_core.chain_candidates selection.program insn flow
    ~retained:(fun pc -> Pc.Map.mem pc selection.entries)
