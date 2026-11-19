type t
type flag = X | N | Z | V | C

val singleton : flag -> t
val all : t
val none : t
val nzvc : t
val union : t -> t -> t
val inter : t -> t -> t
val diff : t -> t -> t
val subset : t -> t -> bool
val to_string : t -> string
