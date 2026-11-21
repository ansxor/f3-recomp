type input = { sem : Sem.t; live_out : Ccr.t; memory : Memory_path.t }

type body = Native of Lower.entry | Fallback

type t = { bodies : body Pc.Map.t; live_out : Ccr.t Pc.Map.t; producer_writes : Ccr.t Pc.Map.t }

type error =
  | Live_flags_elided of { pc : Pc.t; flags : Ccr.t }
  | Unwritten_flags_elided of { pc : Pc.t; flags : Ccr.t }

type counts = { flags : Lower.counts; fallbacks : int }

let rec producer_writes acc = function
  | Sem.Flags producer -> Ccr.union acc (Flags.writes producer)
  | If (_, yes, no) -> List.fold_left producer_writes (List.fold_left producer_writes acc yes) no
  | Repeat body -> List.fold_left producer_writes acc body
  | _ -> acc

let make ?(flags = `Lazy) inputs =
  let bodies = Pc.Map.map (fun { sem; live_out; memory } ->
      match Lower.entry ~flags sem ~live_out ~memory with
      | Ok entry -> Native entry
      | Error _ -> Fallback) inputs in
  let producer_writes = Pc.Map.map (fun input ->
    match input.sem with
    | Body stmts -> List.fold_left producer_writes Ccr.none stmts
    | Unsupported _ -> Ccr.none) inputs in
  { bodies; live_out = Pc.Map.map (fun (input : input) -> input.live_out) inputs; producer_writes }

let bodies t = t.bodies

let check t =
  Pc.Map.fold (fun pc body result ->
      match result, body with
      | Ok (), Native { elided; _ } ->
          let flags = Ccr.inter elided (Pc.Map.find pc t.live_out) in
          let unwritten = Ccr.diff elided (Pc.Map.find pc t.producer_writes) in
          if flags <> Ccr.none then Error (Live_flags_elided { pc; flags })
          else if unwritten <> Ccr.none then Error (Unwritten_flags_elided { pc; flags = unwritten })
          else Ok ()
      | _ -> result)
    t.bodies (Ok ())

let counts t =
  Pc.Map.fold (fun _ body total ->
      match body with
      | Native entry -> { total with flags = Lower.add_counts total.flags entry.counts }
      | Fallback -> { total with fallbacks = total.fallbacks + 1 })
    t.bodies { flags = Lower.no_flags; fallbacks = 0 }
