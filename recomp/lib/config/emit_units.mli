type t

val of_config : Config.t -> t
val hooks : t -> int list Pc.Map.t * int list Pc.Map.t
val digest : t -> int32
val guard_lines : t -> string
val render : t -> Output.t
