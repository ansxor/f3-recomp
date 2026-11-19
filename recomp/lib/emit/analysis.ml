type t = {
  selection : Selection.t;
  groups : Sprite_groups.t;
  semantics : Sem.t Pc.Map.t;
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

let run ~allow_unproven ~writers (selection : Selection.t) =
  let program = selection.program and config = selection.config in
  require "selection" Selection.describe_violation (Selection.check selection);
  let semantics = Cfg.semantics selection in
  let groups = Sprite_trace.run program ~config ~writers in
  let cfg = Cfg.build selection semantics ~pins:(Pins.collect selection semantics groups) in
  require "CFG" cfg_violation (Cfg.check program cfg);
  let irq = Irq_contract.require config.cpu ~allow_unproven (Irq_contract.handlers program.game.rom)
      (Irq_contract.observed program cfg) in
  let liveness = Liveness.solve cfg in
  require "liveness" liveness_violation (Liveness.check cfg liveness);
  let idle = Idle_loop.find program ~native:(Cfg.native cfg) in
  require "idle loops" Idle_loop.describe_violation (Idle_loop.check_all program cfg idle);
  { selection; groups; semantics; liveness; idle; irq }

let lower analysis =
  let lowered = Pc.Map.mapi (fun pc sem ->
    { Lowered.sem; live_out = Liveness.live_out analysis.liveness pc ~flags:`Lazy;
      memory = Memory_path.at analysis.groups pc }) analysis.semantics |> Lowered.make in
  (match Lowered.check lowered with
   | Ok () -> ()
   | Error (Live_flags_elided { pc; flags }) ->
       Error.fail ~where:"lowering" "live flags %s elided at %#x" (Ccr.to_string flags) pc
   | Error (Unwritten_flags_elided { pc; flags }) ->
       Error.fail ~where:"lowering" "elision of unwritten flags %s at %#x" (Ccr.to_string flags) pc);
  lowered

(* With XNZVC observed, every node has all five flags live on exit. *)
let full_observer_counts analysis =
  Pc.Map.fold (fun pc sem counts ->
    match Lower.entry sem ~live_out:Ccr.all ~memory:(Memory_path.at analysis.groups pc) with
    | Ok entry -> Lower.add_counts counts entry.counts
    | Error _ -> counts) analysis.semantics Lower.no_flags
