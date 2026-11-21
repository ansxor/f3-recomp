let of_layout (layout : Layout.t) (analysis : Analysis.t) =
  let counts = Lowered.counts layout.lowered in
  let full = Analysis.producer_counts analysis ~flags:`Eager analysis.liveness in
  let empty = Analysis.observer_counts analysis Ccr.none in
  let observed = Analysis.observer_counts analysis Ccr.all in
  let elided counts = full.lazy_producers - counts.Lower.lazy_producers in
  let x_elided counts = full.eager_x_writes - counts.Lower.eager_x_writes in
  let rom, exceptional, ram, hot, cold = Pc.Map.fold (fun _ (entry, tier) (rom, exceptional, ram, hot, cold) ->
    let hot, cold = match tier with Selection.Hot -> hot + 1, cold | Cold -> hot, cold + 1 in
    match entry with
    | Selection.Native _ -> rom + 1, exceptional, ram, hot, cold
    | Exception_entry _ -> rom, exceptional + 1, ram, hot, cold
    | Ram _ | Ram_exception _ -> rom, exceptional, ram + 1, hot, cold)
    layout.selection.entries (0, 0, 0, 0, 0) in
  let spin, poll, affine = Pc.Map.fold (fun _ (loop : Idle_loop.t) (spin, poll, affine) ->
    match loop.shape with
    | Spin -> spin + 1, poll, affine
    | Poll_bcc _ -> spin, poll + 1, affine
    | Affine _ -> spin, poll, affine + 1) layout.idle (0, 0, 0) in
  let direct, sites, cases = Pc.Map.fold (fun _ (transfer : Layout.transfer) (direct, sites, cases) ->
    direct + List.length transfer.direct, sites + Bool.to_int (transfer.indirect <> []),
    cases + List.length transfer.indirect) layout.transfers (0, 0, 0) in
  [
    Printf.sprintf "%s entries: %d ROM instructions, %d ROM exceptions, %d RAM, %d fallback"
      layout.selection.config.game.id rom exceptional ram counts.fallbacks;
    Printf.sprintf "Tiers: %d hot, %d cold; %d functions in %d shards"
      hot cold (List.length (Layout.functions layout)) (List.length layout.shards);
    "Interrupt contract: " ^ analysis.irq;
    Printf.sprintf "Interrupt yield points: %d" (Pc.Set.cardinal analysis.yields);
    Printf.sprintf "Flag producers elided: %d emitted, %d empty vs %d XNZVC at yields"
      (elided counts.flags) (elided empty) (elided observed);
    Printf.sprintf "Standalone X writes elided: %d emitted, %d empty vs %d XNZVC at yields"
      (x_elided counts.flags) (x_elided empty) (x_elided observed);
    Printf.sprintf "Idle loops: %d spin, %d poll, %d affine" spin poll affine;
    Printf.sprintf "Chains: %d direct, %d indirect sites, %d indirect cases" direct sites cases;
    Printf.sprintf "Sprite writers: %d resolved, %d unresolved, %d resets"
      (List.length analysis.groups.writers) (List.length analysis.groups.unresolved)
      (List.length analysis.groups.resets);
    "Certificates checked: Selection, CFG, Liveness, Idle_loop, Lowered, Layout";
  ]

let print lines = List.iter print_endline lines
