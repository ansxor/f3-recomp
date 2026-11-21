type t = {
  selection : Selection.t;
  groups : Sprite_groups.t;
  semantics : Sem.t Pc.Map.t;
  cfg : Cfg.t;
  yields : Pc.Set.t;
  liveness : Liveness.t;
  idle : Idle_loop.t Pc.Map.t;
  irq : string;
}

let require certificate describe = function
  | Ok () -> ()
  | Error violations ->
      Error.fail ~where:certificate "%s" (String.concat "\n" (List.map describe violations))

let cfg_violation = function
  | Cfg.Interrupt_observers -> "interrupt observer set differs from config"
  | Invalid_node pc -> Printf.sprintf "invalid node at %#x" pc
  | Missing_ram_node pc -> Printf.sprintf "missing RAM node at %#x" pc
  | Missing_hook pc -> Printf.sprintf "missing hook at %#x" pc

let liveness_violation = function
  | Liveness.Missing pc -> Printf.sprintf "missing entry at %#x" pc
  | Unexpected pc -> Printf.sprintf "unexpected entry at %#x" pc
  | Invalid_live_in pc -> Printf.sprintf "invalid live-in at %#x" pc
  | Boundary pc -> Printf.sprintf "missing boundary flags at %#x" pc
  | Edge (pc, target) -> Printf.sprintf "missing edge flags on %#x -> %#x" pc target

let run ~parity ~allow_unproven ~writers (selection : Selection.t) =
  let program = selection.program and config = selection.config in
  require "selection" Selection.describe_violation (Selection.check selection);
  let semantics = Cfg.semantics selection in
  let groups = Sprite_trace.run program ~config ~writers in
  let cfg = Cfg.build selection semantics ~pins:(Pins.collect selection semantics groups) in
  require "CFG" cfg_violation (Cfg.check program cfg);
  let irq = Irq_contract.require config.cpu ~allow_unproven (Irq_contract.handlers program.game.rom)
      (Irq_contract.observed program cfg) in
  (* Every selected entry can return: native transfers have a guarded return,
     and exceptional and fallback entries return directly. This bound can be
     computed before liveness and is checked against the generated bodies. *)
  let yields = Pc.Map.fold (fun pc _ pcs -> Pc.Set.add pc pcs) selection.entries Pc.Set.empty in
  let discovered_idle = Idle_loop.find program ~native:(Cfg.native cfg) in
  let liveness, idle = match parity with
    | None -> Liveness.solve ~yields cfg, discovered_idle
    | Some path ->
        let live_out, imported_idle = Decisions.read path in
        let idle = Pc.Map.mapi (fun pc shape ->
          match Pc.Map.find_opt pc discovered_idle with
          | Some loop when Idle_loop.to_tsv loop.shape = shape -> loop
          | Some loop ->
              Error.fail ~where:"idle loops" "at %#x: imported %s, expected %s"
                pc shape (Idle_loop.to_tsv loop.shape)
          | None -> Error.fail ~where:"idle loops" "no verified %s loop at %#x" shape pc)
          imported_idle in
        { Liveness.live_in = Liveness.project_live_in cfg live_out; live_out }, idle in
  require "liveness" liveness_violation (Liveness.check ~yields cfg liveness);
  require "idle loops" Idle_loop.describe_violation (Idle_loop.check_all program cfg idle);
  { selection; groups; semantics; cfg; yields; liveness; idle; irq }

let lower ~flags analysis =
  let lowered = Pc.Map.mapi (fun pc sem ->
    { Lowered.sem; live_out = Liveness.live_out analysis.liveness pc ~flags;
      memory = Memory_path.at analysis.groups pc }) analysis.semantics |> Lowered.make ~flags in
  (match Lowered.check lowered with
   | Ok () -> ()
   | Error (Live_flags_elided { pc; flags }) ->
       Error.fail ~where:"lowering" "live flags %s elided at %#x" (Ccr.to_string flags) pc
   | Error (Unwritten_flags_elided { pc; flags }) ->
       Error.fail ~where:"lowering" "elision of unwritten flags %s at %#x" (Ccr.to_string flags) pc);
  lowered

let check_yield_coverage analysis pc =
  if not (Pc.Set.mem pc analysis.yields) then
    Error.fail ~where:"layout" "return at %#x is missing from interrupt yield points" pc

let sources analysis layout =
  (match Layout.check_layout layout with
   | Ok () -> ()
   | Error message -> Error.fail ~where:"layout" "%s" message);
  let check_yield = check_yield_coverage analysis in
  Pc.Map.iter (fun pc (entry, _) ->
    match entry with Selection.Exception_entry _ -> check_yield pc | _ -> ())
    layout.Layout.selection.entries;
  Blocks_c.files ~check_yield layout |> Program_c.files layout

let producer_counts analysis ~flags liveness =
  Pc.Map.fold (fun pc sem counts ->
    let live_out = Liveness.live_out liveness pc ~flags in
    match Lower.entry ~flags sem ~live_out ~memory:(Memory_path.at analysis.groups pc) with
    | Ok entry -> Lower.add_counts counts entry.counts
    | Error _ -> counts) analysis.semantics Lower.no_flags

let observer_counts analysis interrupt_observes =
  let cfg = { analysis.cfg with interrupt_observes } in
  let liveness = Liveness.solve ~yields:analysis.yields cfg in
  producer_counts analysis ~flags:`Lazy liveness
