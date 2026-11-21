type t = { where : string; what : string }

exception Invalid of t

let fail ?(where = "") format =
  Printf.ksprintf (fun what -> raise (Invalid { where; what })) format
