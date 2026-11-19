open Insn

type scan = Relative_words | Absolute_pointers | Register_pointers
type evidence = Configured | Bounded of { compare_pc : Pc.t; limit : int } | Heuristic of scan
type t = { site : Pc.t; candidates : Pc.t list; evidence : evidence }

let read_insn space pc =
  match Code_space.image_at space pc with
  | Some image -> (match Decode.at image pc with Insn insn -> Some insn | Illegal _ -> None)
  | None -> None

let target space pc =
  pc land 1 = 0 && Code_space.region space pc = Rom && Option.is_some (read_insn space pc)

let indexed_base = function
  | Pc_at pc -> Some pc
  | Suppressed -> Some 0
  | Areg _ -> None

let relative_words image table =
  if not (Image.contains_range image table ~length:2) then []
  else
    let first = Image.s16 image table in
    if first <= 0 || first > 0x7000 then []
    else
      let count = min 128 (min (first / 2) ((image.Image.base + Image.length image - table) / 2)) in
      let offsets = List.init count (fun i -> Image.s16 image (table + i * 2)) in
      let minimum = List.fold_left (fun m off -> if off > 0 then min m off else m) first offsets in
      List.take (minimum / 2) offsets
      |> List.filter_map (fun off ->
           let pc = Pc.physical (table + off) in
           if off <> -1 && pc >= 0x400 then Some pc else None)

let absolute_pointers space image table outer count =
  (* The ROM vector header is not evidence of an indexed pointer table. *)
  let rec read slot stop remaining pcs =
    if remaining = 0 || slot + 4 > stop || not (Image.contains_range image slot ~length:4) then pcs
    else
      let pc = Insn.address (Image.u32 image slot + outer) in
      (* A heuristic must not turn arbitrary data into plausible pointers. *)
      if pc <> Pc.physical pc || pc < 0x400 || not (target space pc) then pcs
      else
        let stop = if pc >= table then min stop pc else stop in
        read (slot + 4) stop (remaining - 1) (pc :: pcs)
  in
  if table < 0x400 then []
  else read table (image.Image.base + Image.length image) count []

let register_table space (insn : Insn.t) ea =
  let load_end =
    let rec skip_nops pc remaining =
      match read_insn space (pc - 2) with
      | Some { op = Nop; _ } when remaining > 0 -> skip_nops (pc - 2) (remaining - 1)
      | _ -> pc
    in
    skip_nops insn.pc 2
  in
  match ea, read_insn space (load_end - 4) with
  | Ind reg, Some { op = Movea (Long, Indexed { base = Pc_at base; disp; indirect = Direct; _ }, dst); length = 4; _ }
  | Indexed { base = Areg reg; disp = 0; index = None; indirect = Pre 0 },
    Some { op = Lea (Indexed { base = Pc_at base; disp; indirect = Direct; _ }, dst); length = 4; _ }
    when reg = dst -> Some (Pc.physical (base + disp))
  | _ -> None

(* CMP.L #max,Dn; BHI away; JMP ([table,Dn.L*4],outer).
   The evidence describes the fallthrough from the guard. Computed entries
   can bypass that guard, so every emitted case still tests the runtime PC. *)
let bound space (insn : Insn.t) ea =
  match ea with
  | Indexed { index = Some { reg = D reg; long = true; scale = 4 }; indirect = Pre _; _ } ->
      Seq.find_map (fun branch_size ->
        let branch_pc = insn.pc - branch_size in
        let compare_pc = branch_pc - 6 in
        match read_insn space compare_pc, read_insn space branch_pc with
        | Some { op = Alu (Cmp, Long, Immediate max_index, Register (D dst)); length = 6; _ },
          Some { op = Bcc (Hi, away); length; _ }
          when reg = dst && length = branch_size && Pc.physical away <> insn.pc
               && max_index >= 0 && max_index < 64 -> Some (compare_pc, max_index + 1)
        | _ -> None)
        (List.to_seq [2; 4; 6])
  | _ -> None

let computed insn =
  match (Flow.of_insn insn).escape with Some (Computed ea) -> Some ea | _ -> None

let scan space (insn : Insn.t) =
  let make evidence pcs =
    match List.sort_uniq Int.compare (List.filter (target space) pcs) with
    | [] -> None
    | candidates -> Some { site = insn.pc; evidence; candidates }
  in
  match computed insn, Code_space.image_at space insn.pc with
  | Some ea, Some image ->
      (match register_table space insn ea with
       | Some table -> make (Heuristic Register_pointers) (absolute_pointers space image table 0 64)
       | None ->
           match ea with
           | Indexed { base = Pc_at base; disp; indirect = Direct; _ } ->
               make (Heuristic Relative_words) (relative_words image (Pc.physical (base + disp)))
           | Indexed { base; disp; indirect = Pre outer; index = Some _ } ->
               Option.bind (indexed_base base) (fun base ->
                 let table = Pc.physical (base + disp) in
                 let evidence, count =
                   match bound space insn ea with
                   | Some (compare_pc, limit) -> Bounded { compare_pc; limit }, limit
                   | None -> Heuristic Absolute_pointers, 64
                 in
                 make evidence (absolute_pointers space image table outer count))
           | _ -> None)
  | _ -> None

let configured space rom (spec : Config.jump_table) =
  let pointers =
    match spec.pointers with
    | None -> []
    | Some { address; count } ->
        List.init count (fun slot -> Pc.physical (Image.u32 rom (address + 4 * slot)))
  in
  let targets =
    spec.targets @ pointers
    |> List.filter (fun pc -> pc land 1 = 0 && Image.contains rom pc)
  in
  List.iter (fun pc ->
    match Code_space.region space pc with
    | Excluded _ ->
        Error.fail ~where:"jump table" "configured table at %#x targets excluded PC %#x" spec.address pc
    | _ -> ())
    targets;
  (* Config records apply at computed JMP or JSR sites and override a scan
     when they provide an in-ROM target. *)
  match targets, Option.bind (read_insn space spec.address) computed with
  | [], _ | _, None -> None
  | _, Some _ ->
      let candidates = List.sort_uniq Int.compare (List.filter (target space) targets) in
      Some { site = spec.address; evidence = Configured; candidates }

let check table space =
  let valid_site =
    table.site land 1 = 0
    && match read_insn space table.site with
       | Some insn ->
           (match computed insn, table.evidence with
            | None, _ -> false
            | Some ea, Bounded { compare_pc; limit } ->
                bound space insn ea = Some (compare_pc, limit)
                && List.length table.candidates <= limit
            | Some _, _ -> true)
       | None -> false
  in
  if not valid_site
     || table.candidates <> List.sort_uniq Int.compare table.candidates
     || not (List.for_all (target space) table.candidates)
  then Error.fail ~where:"jump table" "invalid candidates or evidence at %#x" table.site
