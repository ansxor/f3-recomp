type pending = Keep | Clear | Install of Sem.producer
type t = { x : Sem.expr option; pending : pending }

val writes : Sem.producer -> Ccr.t

(** Specialise a lazy producer for its live-out set. When X is live, a pending
    ADD or SUB must materialise the X already stored in SR. *)
val specialise : Sem.producer -> live:Ccr.t -> t
