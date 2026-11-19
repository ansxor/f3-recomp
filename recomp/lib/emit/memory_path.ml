type t = Inline | Bus_writes

let at ~reset_pcs pc =
  if Pc.Set.mem pc reset_pcs then Bus_writes else Inline

let sized prefix width = prefix ^ string_of_int (8 * Sem.bytes width)

let load (_ : t) = sized "f3_ld"

let store = function
  | Inline -> sized "f3_st"
  | Bus_writes -> sized "f3_write"
