type t = { live_in : Ccr.t Pc.Map.t; live_out : Ccr.t Pc.Map.t }

let base (cfg : Cfg.t) (node : Cfg.node) =
  match node.boundaries with [] -> cfg.interrupt_observes | _ -> Ccr.all

let solve (cfg : Cfg.t) =
  let work = Queue.create () in
  let size = Pc.Map.cardinal cfg.nodes in
  let states = Hashtbl.create size in
  let live_in = Array.make size Ccr.none in
  let live_out = Array.make size Ccr.none in
  let predecessors = Array.make size [] in
  let queued = Array.make size true in
  let nodes = Array.of_seq (Pc.Map.to_rev_seq cfg.nodes |> Seq.map snd) in
  Array.iteri (fun i (node : Cfg.node) ->
    Hashtbl.add states node.pc i;
    live_in.(i) <- node.effects.reads;
    Queue.add i work) nodes;
  let successors = Array.mapi (fun i (node : Cfg.node) ->
    List.map (fun pc ->
      let target = Hashtbl.find states pc in
      predecessors.(target) <- i :: predecessors.(target);
      target) node.succ) nodes in
  while not (Queue.is_empty work) do
    let i = Queue.take work in
    queued.(i) <- false;
    let node = nodes.(i) in
    let out = List.fold_left (fun acc target -> Ccr.union acc live_in.(target))
        (base cfg node) successors.(i) in
    live_out.(i) <- out;
    let incoming = Effects.live_in node.effects ~live_out:out in
    if incoming <> live_in.(i) then begin
      live_in.(i) <- incoming;
      List.iter (fun pred ->
        if not queued.(pred) then begin
          queued.(pred) <- true;
          Queue.add pred work
        end) predecessors.(i)
    end
  done;
  let incoming = ref Pc.Map.empty and outgoing = ref Pc.Map.empty in
  Array.iteri (fun i (node : Cfg.node) ->
    incoming := Pc.Map.add node.pc live_in.(i) !incoming;
    outgoing := Pc.Map.add node.pc live_out.(i) !outgoing) nodes;
  { live_in = !incoming; live_out = !outgoing }

type violation = Missing of int | Unexpected of int | Invalid_live_in of int
  | Boundary of int | Edge of int * int

let check (cfg : Cfg.t) cert =
  let violations = Pc.Map.fold (fun pc (node : Cfg.node) acc ->
    match Pc.Map.find_opt pc cert.live_in, Pc.Map.find_opt pc cert.live_out with
    | Some incoming, Some out ->
        let acc = if incoming = Effects.live_in node.effects ~live_out:out
          then acc else Invalid_live_in pc :: acc in
        let acc = if Ccr.subset (base cfg node) out then acc else Boundary pc :: acc in
        List.fold_left (fun acc target ->
          match Pc.Map.find_opt target cfg.nodes, Pc.Map.find_opt target cert.live_out with
          | Some successor, Some live_out
            when Ccr.subset (Effects.live_in successor.effects ~live_out) out -> acc
          | _ -> Edge (pc, target) :: acc) acc node.succ
    | _ -> Missing pc :: acc)
    cfg.nodes [] in
  let unexpected pc _ acc =
    if Pc.Map.mem pc cfg.nodes then acc else Unexpected pc :: acc in
  let violations = Pc.Map.fold unexpected cert.live_in violations
    |> Pc.Map.fold unexpected cert.live_out in
  match violations with [] -> Ok () | _ -> Error (List.rev violations)

let live_out cert pc ~flags =
  match flags with
  | `Eager -> Ccr.all
  | `Lazy -> Option.value (Pc.Map.find_opt pc cert.live_out) ~default:Ccr.all
