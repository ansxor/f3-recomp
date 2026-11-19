type func = { name : string; tier : Selection.tier; pcs : int list }
type transfer = { direct : int list; indirect : int list; dispatch : bool }
type shard = { filename : string; tier : Selection.tier; functions : func list }

type t = {
  selection : Selection.t;
  lowered : Lowered.t;
  idle : Idle_loop.t Pc.Map.t;
  shards : shard list;
  owner : string Pc.Map.t;
  transfers : transfer Pc.Map.t;
}

let rom_dispatch_limit = 0x200000

let empty_transfer = { direct = []; indirect = []; dispatch = false }
let native_name pc = Printf.sprintf "f3_native_%06x" pc
let label pc = Printf.sprintf "L_%06x" pc

let pack selection max_block tier =
  let pages = Pc.Map.fold (fun pc (entry, entry_tier) pages ->
    if entry_tier <> tier then pages
    else match entry with
    | Selection.Native _ ->
        Pc.Map.update ((pc / 2) / max_block)
          (fun pcs -> Some (pc :: Option.value pcs ~default:[])) pages
    | _ -> pages)
    selection.Selection.entries Pc.Map.empty in
  let ram = Pc.Map.fold (fun pc (entry, entry_tier) acc ->
    match entry with
    | (Selection.Ram _ | Ram_exception _) when entry_tier = tier -> [pc] :: acc
    | _ -> acc) selection.entries [] in
  let blocks = List.map (fun (_, pcs) -> List.rev pcs) (Pc.Map.bindings pages) @ List.rev ram in
  List.map (fun pcs -> { name = native_name (List.hd pcs); tier; pcs }) blocks

let shard mode (tier : Selection.tier) functions =
  let prefix = match mode, tier with
    | Selection.Baseline, _ -> "blocks"
    | _, Hot -> "blocks_hot"
    | _, Cold -> "blocks_cold"
  in
  let rec split index = function
    | [] -> []
    | remaining ->
        let functions = List.take 128 remaining in
        let filename = Printf.sprintf "%s_%04d.c" prefix index in
        { filename; tier; functions } :: split (index + 1) (List.drop 128 remaining)
  in
  split 0 functions

let transfers selection owner =
  let eligible pc = pc < rom_dispatch_limit && Pc.Map.mem pc owner &&
    Code_space.region selection.Selection.program.space pc = Rom in
  Pc.Map.filter_map (fun pc (entry, _) ->
    match entry with
    | Selection.Native insn ->
        let flow = Flow.of_insn insn in
        let direct = Cfg.targets insn flow |> List.filter eligible in
        let computed = match flow.escape with Some (Computed _) -> true | _ -> false in
        let indirect = match Pc.Map.find_opt pc selection.program.tables with
          | Some table when computed ->
              table.candidates |> List.filter eligible |> List.sort_uniq Int.compare |> List.take 256
          | _ -> [] in
        let dispatch = computed || match insn.op with Rts | Rtd _ -> true | _ -> false in
        Some { direct; indirect; dispatch }
    | Ram _ | Ram_exception _ -> Some empty_transfer
    | Exception_entry _ -> None)
    selection.entries

let make ~max_block ~(selection : Selection.t) ~idle lowered =
  let shards = List.concat_map (fun tier ->
    pack selection max_block tier |> shard selection.mode tier) [Hot; Cold] in
  let owner = List.fold_left (fun owner shard ->
    List.fold_left (fun owner func ->
      List.fold_left (fun owner pc -> Pc.Map.add pc func.name owner) owner func.pcs)
      owner shard.functions) Pc.Map.empty shards in
  { selection; lowered; idle; shards; owner; transfers = transfers selection owner }

let check ~is_ram ~functions ~transfers =
  let ram_function = List.find_opt (fun func ->
    List.length func.pcs <> 1 && List.exists is_ram func.pcs) functions in
  let dispatch_ram = List.exists (fun func ->
    List.exists (fun pc -> is_ram pc && pc < rom_dispatch_limit) func.pcs) functions in
  let ram_target = Pc.Map.fold (fun pc transfer found ->
    if List.exists is_ram transfer.direct ||
       List.exists is_ram transfer.indirect then Some pc else found) transfers None in
  match ram_function, ram_target, dispatch_ram with
  | Some func, _, _ -> Error (Printf.sprintf "%s contains multiple instructions including RAM" func.name)
  | None, Some pc, _ -> Error (Printf.sprintf "transfer at %#x targets RAM" pc)
  | None, None, true -> Error "RAM entry is within the ROM dispatch table"
  | None, None, false -> Ok ()

let functions layout = List.concat_map (fun shard -> shard.functions) layout.shards

let is_ram layout pc =
  match Code_space.region layout.selection.program.space pc with Ram _ -> true | _ -> false

let check_layout layout =
  check ~is_ram:(is_ram layout) ~functions:(functions layout) ~transfers:layout.transfers

let exception_name tier vector =
  let suffix = match tier with Selection.Hot -> "" | Cold -> "_cold" in
  Printf.sprintf "f3_rom_exception_%d%s" vector suffix

let exception_entries layout =
  Pc.Map.fold (fun _ (entry, tier) entries ->
    match entry with
    | Selection.Exception_entry vector -> (tier, vector) :: entries
    | _ -> entries) layout.selection.entries []
  |> List.sort_uniq compare
