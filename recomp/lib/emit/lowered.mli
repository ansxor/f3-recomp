type input = { sem : Sem.t; live_out : Ccr.t; memory : Memory_path.t }

type body = Native of Lower.entry | Fallback

type t

type error = Live_flags_elided of { pc : Pc.t; flags : Ccr.t }

type counts = { flags : Lower.counts; fallbacks : int }

val make : input Pc.Map.t -> t
val bodies : t -> body Pc.Map.t
val check : t -> (unit, error) result
val counts : t -> counts
