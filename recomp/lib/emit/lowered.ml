type input = { sem : Sem.t; live_out : Ccr.t; memory : Memory_path.t }

type body = Native of Lower.entry | Fallback

type t = { bodies : body Pc.Map.t; live_out : Ccr.t Pc.Map.t }

type error = Live_flags_elided of { pc : Pc.t; flags : Ccr.t }

type counts = { flags : Lower.counts; fallbacks : int }

let make inputs =
  let bodies = Pc.Map.map (fun { sem; live_out; memory } ->
      match Lower.entry sem ~live_out ~memory with
      | Ok entry -> Native entry
      | Error _ -> Fallback) inputs in
  { bodies; live_out = Pc.Map.map (fun (input : input) -> input.live_out) inputs }

let bodies t = t.bodies

let check t =
  Pc.Map.fold (fun pc body result ->
      match result, body with
      | Ok (), Native { elided; _ } ->
          let flags = Ccr.inter elided (Pc.Map.find pc t.live_out) in
          if flags = Ccr.none then Ok () else Error (Live_flags_elided { pc; flags })
      | _ -> result)
    t.bodies (Ok ())

let counts t =
  Pc.Map.fold (fun _ body total ->
      match body with
      | Native entry -> { total with flags = Lower.add_counts total.flags entry.counts }
      | Fallback -> { total with fallbacks = total.fallbacks + 1 })
    t.bodies { flags = Lower.no_flags; fallbacks = 0 }
