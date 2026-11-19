type boundary = Return | Computed | Leaves_native of int | Pinned of Pins.reason | Raises
type node = { pc : int; effects : Effects.t; succ : int list; boundaries : boundary list }
type t = { nodes : node Pc.Map.t; interrupt_observes : Ccr.t }

let native cfg pc =
  match Pc.Map.find_opt pc cfg.nodes with
  | None -> false
  | Some node -> not (List.exists (function Pinned _ -> true | _ -> false) node.boundaries)

let semantics (selection : Selection.t) =
  Pc.Map.filter_map (fun _ -> function
    | (Selection.Native insn | Ram insn), _ -> Some (Semantics.of_insn insn)
    | (Exception_entry _ | Ram_exception _), _ -> None)
    selection.entries

let targets (insn : Insn.t) (flow : Flow.t) =
  let next = Pc.physical (insn.pc + insn.length) in
  List.sort_uniq Int.compare (if flow.falls_through then next :: flow.targets else flow.targets)

let node ~native ~pins (insn : Insn.t) sem =
  let flow = Flow.of_insn insn in
  let effects = Effects.of_sem sem in
  let succ, missing = List.partition native (targets insn flow) in
  let escape = match flow.escape with
    | Some (Return | Exception_return) -> [Return]
    | Some (Computed _ | Module_call _) -> [Computed]
    | Some (Raises | Halt) -> [Raises]
    | None when Insn.privileged insn.op -> [Raises]
    | None -> []
  in
  let boundaries = escape @ List.map (fun pc -> Leaves_native pc) missing
    @ List.map (fun reason -> Pinned reason) pins in
  { pc = insn.pc; effects; succ; boundaries }

let build (selection : Selection.t) semantics ~pins =
  let native pc =
    not (Pc.Map.mem pc pins) &&
    match Pc.Map.find_opt pc selection.entries with
    | Some ((Selection.Native _ | Ram _), _) -> true
    | _ -> false in
  let nodes = Pc.Map.filter_map (fun pc (entry, _) ->
    match entry with
    | Selection.Native insn | Ram insn ->
        let sem = match Pc.Map.find_opt pc semantics with
          | Some sem -> sem
          | None -> Error.fail ~where:"CFG" "missing semantics for %#x" pc in
        let pins = Option.value (Pc.Map.find_opt pc pins) ~default:[] in
        Some (node ~native ~pins insn sem)
    | Exception_entry _ | Ram_exception _ -> None)
    selection.entries in
  { nodes; interrupt_observes = selection.config.cpu.interrupt_observes }

type violation =
  | Interrupt_observers
  | Invalid_node of int
  | Missing_ram_node of int
  | Missing_hook of int

(* The retained ROM node set comes from selection and can be pruned in slim
   mode. Verify the induced graph and entries required in every mode. *)
let check (program : Program.t) cfg =
  let config = program.game.config in
  let starts, ends = Emit_units.hooks (Emit_units.of_config config) in
  let hook pc = Pc.Map.mem pc starts || Pc.Map.mem pc ends in
  let violations = Pc.Map.fold (fun pc actual acc ->
    match Program.insn program pc with
    | None -> Invalid_node pc :: acc
    | Some insn ->
        let sem = Semantics.of_insn insn in
        let pins =
          (match sem with Unsupported _ -> [Pins.Fallback] | Body _ -> [])
          @ (if hook pc then [Pins.Unit_hook] else [])
          @ (if List.mem (Pinned Pins.Sprite_reset) actual.boundaries then [Pins.Sprite_reset] else [])
          @ (match Code_space.region program.space pc with Ram _ -> [Pins.Ram_code] | _ -> [])
        in
        let expected = node ~native:(native cfg) ~pins insn sem in
        if actual = expected then acc else Invalid_node pc :: acc)
    cfg.nodes [] in
  let violations = Pc.Map.fold (fun pc decoded acc ->
    match Code_space.region program.space pc, decoded with
    | Ram _, Decode.Insn _ when not (Pc.Map.mem pc cfg.nodes) -> Missing_ram_node pc :: acc
    | _ when hook pc && not (Pc.Map.mem pc cfg.nodes) -> Missing_hook pc :: acc
    | _ -> acc)
    program.decoded violations in
  let violations =
    if cfg.interrupt_observes = config.cpu.interrupt_observes then violations
    else Interrupt_observers :: violations
  in
  match violations with [] -> Ok () | _ -> Error (List.rev violations)

let of_selection selection =
  let semantics = semantics selection in
  build selection semantics ~pins:(Pins.collect selection semantics Pc.Set.empty)
