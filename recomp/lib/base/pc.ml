type t = int

module Map = Map.Make (Int)
module Set = Set.Make (Int)

let physical pc = pc land 0xffffff
