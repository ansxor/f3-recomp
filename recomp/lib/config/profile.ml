type region = Exclusion.cpu = Main | Sound
type record_kind = Hit | Miss

module Roms = Map.Make (struct
  type t = region * int32
  let compare = compare
end)

type counts = int64 Pc.Map.t
type t = {
  roms : (int * int) Roms.t;
  hits : counts Roms.t;
  misses : counts Roms.t;
}

let empty = { roms = Roms.empty; hits = Roms.empty; misses = Roms.empty }

let add_count left right =
  let sum = Int64.add left right in
  if Int64.unsigned_compare sum left < 0 then Int64.minus_one else sum

let merge left right =
  let roms = Roms.union (fun _ a b ->
    if a <> b then Error.fail "profile ROM bounds disagree";
    Some a) left.roms right.roms in
  let combine = Roms.union (fun _ a b ->
    Some (Pc.Map.union (fun _ x y -> Some (add_count x y)) a b)) in
  { roms; hits = combine left.hits right.hits; misses = combine left.misses right.misses }

let region_name = function Main -> "main" | Sound -> "sound"
let kind_name = function Hit -> "hit" | Miss -> "miss"
let rows profile = function Hit -> profile.hits | Miss -> profile.misses

let addresses profile kind key =
  Roms.find_opt key (rows profile kind) |> Option.value ~default:Pc.Map.empty

let count profile kind region ~crc ~pc =
  Pc.Map.find_opt pc (addresses profile kind (region, crc))

let digits = function '0' .. '9' -> true | _ -> false
let hex = function 'a' .. 'f' | 'A' .. 'F' -> true | c -> digits c
let whitespace = function ' ' | '\t' | '\r' | '\012' | '\011' -> true | _ -> false

let hex_word ~where text =
  if String.length text <> 8 || not (String.for_all hex text) then
    Error.fail ~where "expected eight hex digits";
  int_of_string ("0x" ^ text)

let read_count ~where text =
  if not (String.for_all digits text) then Error.fail ~where "expected decimal count";
  match Int64.of_string_opt ("0u" ^ text) with
  | Some count when count <> 0L -> count
  | _ -> Error.fail ~where "count outside uint64"

type execution = {
  where : string;
  kind : record_kind;
  identity : region * int32;
  pc : int;
  count : int64;
}

let read_record ~where text profile pending =
  let fields =
    String.map (fun c -> if whitespace c then ' ' else c) text
    |> String.split_on_char ' ' |> List.filter ((<>) "")
  in
  match fields with
  | [] -> profile, pending
  | [kind; region; crc; address; value] ->
      let region = match region with
        | "main" -> Main | "sound" -> Sound
        | _ -> Error.fail ~where "unknown ROM region"
      in
      let identity = region, Int32.of_int (hex_word ~where crc) in
      let address = hex_word ~where address in
      (match kind with
       | "rom" ->
           let size = hex_word ~where value in
           if address land 1 <> 0 || size = 0 || size land 1 <> 0
              || address + size > 0x100000000 then
             Error.fail ~where "invalid ROM bounds";
           if Roms.mem identity profile.roms then Error.fail ~where "duplicate ROM identity";
           { profile with roms = Roms.add identity (address, size) profile.roms }, pending
       | "hit" | "miss" ->
           let kind = if kind = "hit" then Hit else Miss in
           let count = read_count ~where value in
           profile, { where; kind; identity; pc = address; count } :: pending
       | _ -> Error.fail ~where "unknown record kind")
  | _ -> Error.fail ~where "expected five fields"

let add_execution profile { where; kind; identity; pc; count } =
  let base, size = match Roms.find_opt identity profile.roms with
    | Some bounds -> bounds
    | None ->
        let region, crc = identity in
        Error.fail ~where "profile record without ROM identity: %s %08lx" (region_name region) crc
  in
  if pc land 1 <> 0 || pc < base || pc >= base + size then
    Error.fail ~where "profile address outside aligned ROM: %08x" pc;
  let counts = Pc.Map.update pc (function
    | None -> Some count | Some prior -> Some (add_count prior count))
    (addresses profile kind identity) in
  match kind with
  | Hit -> { profile with hits = Roms.add identity counts profile.hits }
  | Miss -> { profile with misses = Roms.add identity counts profile.misses }

let load path =
  In_channel.with_open_text path (fun input ->
    (match In_channel.input_line input with
     | Some ("F3-BLOCK-PROFILE 1" | "F3-BLOCK-PROFILE 1\r") -> ()
     | _ -> Error.fail ~where:path "unsupported block profile version");
    let rec read_records line profile pending =
      match In_channel.input_line input with
      | None -> List.fold_left add_execution profile (List.rev pending)
      | Some text ->
          let where = Printf.sprintf "%s:%d" path line in
          let profile, pending = read_record ~where text profile pending in
          read_records (line + 1) profile pending
    in
    read_records 2 empty [])

type stats = { identities : int; executed : int; cold_misses : int }

let stats (profile : t) =
  let total rows = Roms.fold (fun _ counts sum -> sum + Pc.Map.cardinal counts) rows 0 in
  { identities = Roms.cardinal profile.roms; executed = total profile.hits;
    cold_misses = total profile.misses }

(* ROM identities sort by region, then unsigned CRC, as the runtime writes them. *)
let compare_identity (left_region, left_crc) (right_region, right_crc) =
  match compare left_region right_region with
  | 0 -> Int.compare (Int32.to_int left_crc land 0xffffffff) (Int32.to_int right_crc land 0xffffffff)
  | order -> order

let render profile =
  let buffer = Buffer.create 4096 in
  Buffer.add_string buffer "F3-BLOCK-PROFILE 1\n";
  let sorted rows =
    Roms.bindings rows |> List.sort (fun (a, _) (b, _) -> compare_identity a b)
  in
  List.iter (fun ((region, crc), (base, size)) ->
    Printf.bprintf buffer "rom %s %08lx %08x %08x\n" (region_name region) crc base size)
    (sorted profile.roms);
  let rows kind counts =
    List.iter (fun ((region, crc), counts) ->
      Pc.Map.iter (fun pc count ->
        Printf.bprintf buffer "%s %s %08lx %08x %Lu\n" kind (region_name region) crc pc count)
        counts)
      (sorted counts)
  in
  rows "hit" profile.hits;
  rows "miss" profile.misses;
  Buffer.contents buffer

let write path profile = Output.write_file path (render profile)

let hot profile region (image : Image.t) ~exclusions =
  let crc = Image.crc32 image in
  let size = Image.length image in
  let key = region, crc in
  if Roms.find_opt key profile.roms <> Some (image.base, size) then
    Error.fail "profile lacks matching %s ROM CRC %08lx, base %08x, size %08x"
      (region_name region) crc image.base size;
  let hits = addresses profile Hit key in
  let check kind pc _ =
    match List.find_opt (fun (range : Exclusion.t) -> range.start <= pc && pc < range.stop) exclusions with
    | None -> ()
    | Some range ->
        Error.fail "profile %s targets excluded %s PC %#x in [%#x, %#x): %s"
          (kind_name kind) (region_name region) pc range.start range.stop range.reason
  in
  Pc.Map.iter (check Hit) hits;
  Pc.Map.iter (check Miss) (addresses profile Miss key);
  Pc.Map.fold (fun pc _ hot -> Pc.Set.add pc hot) hits Pc.Set.empty
