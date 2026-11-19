type cpu = Main | Sound

type t = {
  start : int;
  stop : int;
  reason : string;
  evidence : string;
  fill : int option;
}

let validate ~where ~base ~size exclusions =
  List.iter
    (fun { start; stop; reason; evidence; fill } ->
      if start < base || stop <= start || stop - base < 0 || stop - base > size
         || start mod 2 <> 0 || stop mod 2 <> 0 then
        Error.fail ~where "invalid exclusion interval [%#x, %#x)" start stop;
      if String.trim reason = "" then Error.fail ~where "exclusion requires nonempty reason";
      if String.trim evidence = "" then Error.fail ~where "exclusion requires nonempty evidence";
      Option.iter
        (fun byte ->
          if byte < 0 || byte > 255 then Error.fail ~where "fill must be a byte value")
        fill)
    exclusions;
  let sorted = List.sort (fun a b -> Int.compare a.start b.start) exclusions in
  let rec check_neighbors = function
    | first :: (second :: _ as rest) ->
        if first.stop > second.start then
          Error.fail ~where "overlapping exclusions at %#x" second.start;
        check_neighbors rest
    | _ -> ()
  in
  check_neighbors sorted;
  sorted

let find exclusions pc =
  let address = Pc.physical pc in
  List.find_opt (fun { start; stop; _ } -> start <= address && address < stop) exclusions

let check ~image exclusion =
  Option.iter
    (fun value ->
      let image = match image with
        | Some image -> image
        | None -> Error.fail ~where:"exclusion" "declares fill but no program image was given"
      in
      for pc = exclusion.start to exclusion.stop - 1 do
        let actual = Image.u8 image pc in
        if actual <> value then
          Error.fail ~where:"exclusion" "fill 0x%02x contradicted by byte 0x%02x at %#x"
            value actual pc
      done)
    exclusion.fill

let of_config ~image exclusions =
  let exclusions =
    validate ~where:"exclusion" ~base:image.Image.base ~size:(Image.length image) exclusions
  in
  List.iter (check ~image:(Some image)) exclusions;
  exclusions
