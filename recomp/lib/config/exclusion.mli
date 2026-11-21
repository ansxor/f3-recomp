type cpu = Main | Sound

type t = {
  start : int;
  stop : int;
  reason : string;
  evidence : string;
  fill : int option;
}

(** Validate and sort half-open intervals for one CPU's program image. *)
val validate : where:string -> base:int -> size:int -> t list -> t list

val find : t list -> Pc.t -> t option
val check : image:Image.t option -> t -> unit
val of_config : image:Image.t -> t list -> t list
