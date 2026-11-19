type t = int
type flag = X | N | Z | V | C

let singleton = function X -> 16 | N -> 8 | Z -> 4 | V -> 2 | C -> 1
let all = 31
let none = 0
let nzvc = 15
let union a b = a lor b
let inter a b = a land b
let diff a b = a land lnot b
let subset a b = diff a b = none

let to_string flags =
  [ X, "X"; N, "N"; Z, "Z"; V, "V"; C, "C" ]
  |> List.filter_map (fun (flag, name) ->
         if subset (singleton flag) flags then Some name else None)
  |> String.concat ""
