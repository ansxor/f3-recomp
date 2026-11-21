type routine = {
  destination : Pc.t;
  payload : Image.t;
  entry : Pc.t;
  decoded : Decode.decoded Pc.Map.t;
}

let validated_specs (specs : Config.ram_code list) rom =
  List.iter (fun (spec : Config.ram_code) ->
    let { Config.destination; source; length; entry; _ } = spec in
    let fits_window base stop = destination >= base && destination <= stop - length in
    if length <= 0 || length mod 4 <> 0 || source < 0 || source mod 2 <> 0
       || not (Image.contains_range rom source ~length)
       || destination mod 2 <> 0 || entry mod 2 <> 0
       || not (fits_window 0x400000 0x440000 || fits_window 0x600000 0x610000)
       || entry < destination || entry >= destination + length then
      Error.fail ~where:"RAM code" "invalid RAM code range at %#x or ROM source at %#x"
        destination source)
    specs;
  let specs =
    List.sort (fun (a : Config.ram_code) (b : Config.ram_code) ->
      Int.compare a.destination b.destination) specs
  in
  let rec check_neighbors = function
    | (first : Config.ram_code) :: (second :: _ as rest) ->
        if first.destination + first.length > second.destination then
          Error.fail ~where:"RAM code" "overlapping RAM code at %#x" second.destination;
        check_neighbors rest
    | _ -> ()
  in
  check_neighbors specs;
  specs

let payload_of_spec rom (spec : Config.ram_code) =
  let bytes = Bytes.create spec.length in
  let transform_word = match spec.transform with Config.Negate_be32 -> Int32.neg in
  for word = 0 to spec.length / 4 - 1 do
    let offset = word * 4 in
    let value = Image.s32 rom (spec.source + offset) |> Int32.of_int in
    Bytes.set_int32_be bytes offset (transform_word value)
  done;
  { Image.base = spec.destination; bytes = Bytes.unsafe_to_string bytes }

let decode_payload payload =
  let rec decode_at pc decoded =
    if Image.contains payload pc then
      decode_at (pc + 2) (Pc.Map.add pc (Decode.at payload pc) decoded)
    else decoded
  in
  decode_at payload.Image.base Pc.Map.empty

let decode specs rom =
  validated_specs specs rom
  |> List.map (fun (spec : Config.ram_code) ->
       let payload = payload_of_spec rom spec in
       let decoded = decode_payload payload in
       { destination = spec.destination; payload; entry = spec.entry; decoded })

let exits routines =
  List.fold_left (fun exits routine ->
    Pc.Map.fold (fun _ decoded exits ->
      match decoded with
      | Decode.Illegal _ -> exits
      | Decode.Insn insn ->
          List.fold_left (fun exits target ->
            if Image.contains routine.payload target then exits
            else Pc.Set.add target exits)
            exits (Flow.of_insn insn).targets)
      routine.decoded exits)
    Pc.Set.empty routines

let check specs rom routines =
  let specs = validated_specs specs rom in
  if List.length specs <> List.length routines then
    Error.fail ~where:"RAM code" "routine count does not match configuration";
  List.iter2 (fun (spec : Config.ram_code) routine ->
    if routine.destination <> spec.destination || routine.entry <> spec.entry
       || routine.payload <> payload_of_spec rom spec then
      Error.fail ~where:"RAM code" "routine does not match configuration at %#x" spec.destination;
    if Pc.Map.cardinal routine.decoded <> spec.length / 2
       || not (Pc.Map.for_all (fun pc _ ->
         pc mod 2 = 0 && Image.contains routine.payload pc) routine.decoded) then
      Error.fail ~where:"RAM code" "incorrect aligned PC coverage at %#x" spec.destination)
    specs routines
