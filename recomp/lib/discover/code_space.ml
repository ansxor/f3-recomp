type t = { rom : Image.t; exclusions : Exclusion.t list; ram : Ram_code.routine list }
type region = Rom | Ram of Ram_code.routine | Excluded of Exclusion.t | Outside

let make ~rom ~exclusions ~ram =
  let exclusions = Exclusion.of_config ~image:rom exclusions in
  { rom; exclusions; ram }

let region space pc =
  if Image.contains space.rom pc then
    match Exclusion.find space.exclusions pc with
    | Some exclusion -> Excluded exclusion
    | None -> Rom
  else
    match List.find_opt (fun routine -> Image.contains routine.Ram_code.payload pc) space.ram with
    | Some routine -> Ram routine
    | None -> Outside

let image_at space pc =
  match region space pc with
  | Rom -> Some space.rom
  | Ram routine -> Some routine.payload
  | Excluded _ | Outside -> None
