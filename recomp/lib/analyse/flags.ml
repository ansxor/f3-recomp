type pending = Keep | Clear | Install of Sem.producer
type t = { x : Sem.expr option; pending : pending }

let writes : Sem.producer -> Ccr.t = function
  | Logic _ | Cmp _ -> Ccr.nzvc
  | Add _ | Sub _ -> Ccr.all

let specialise (producer : Sem.producer) ~live =
  let x =
    if Ccr.inter live (Ccr.singleton X) = Ccr.none then None
    else
      let open Sem in
      match producer with
      | Add { src; result; width; _ } ->
          Some (Bin (Lt, truncate width result, truncate width src))
      | Sub { src; dst; width; _ } ->
          Some (Bin (Lt, truncate width dst, truncate width src))
      | Logic _ | Cmp _ -> None
  in
  let pending =
    if Ccr.inter live Ccr.nzvc <> Ccr.none then Install producer
    else if Option.is_some x then Clear
    else Keep
  in
  { x; pending }
