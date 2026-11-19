type reason = Fallback | Unit_hook | Sprite_reset | Ram_code
type t = reason list Pc.Map.t

let collect (selection : Selection.t) semantics groups : t =
  let starts, ends = Emit_units.hooks selection.units in
  Pc.Map.filter_map (fun pc (entry, _) ->
    let reasons =
      (match Pc.Map.find_opt pc semantics with
       | Some (Sem.Unsupported _) -> [Fallback]
       | _ -> [])
      @ (if Pc.Map.mem pc starts || Pc.Map.mem pc ends then [Unit_hook] else [])
      @ (if Sprite_groups.is_reset groups pc then [Sprite_reset] else [])
      @ (match entry with Selection.Ram _ | Ram_exception _ -> [Ram_code] | _ -> [])
    in
    match reasons with [] -> None | _ -> Some reasons)
    selection.entries
