(* The loaded data is shared by the discovery stages. Game owns the pipeline. *)
type t = { config : Config.t; rom : Image.t; exclusions : Exclusion.t list }
