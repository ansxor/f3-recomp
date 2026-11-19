type t
type region = Rom | Ram of Ram_code.routine | Excluded of Exclusion.t | Outside

(** Assemble the executable address space from the verified ROM, main CPU
    exclusions, and decoded RAM routines. *)
val make : rom:Image.t -> exclusions:Exclusion.t list -> ram:Ram_code.routine list -> t

(** Classify a physical PC. ROM exclusions take precedence over ROM bytes. *)
val region : t -> Pc.t -> region

(** Return the image for an executable physical PC. *)
val image_at : t -> Pc.t -> Image.t option
