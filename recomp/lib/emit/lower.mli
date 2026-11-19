type entry = {
  stmts : C.stmt list;
  elided : Ccr.t;  (** Union of omitted writes at individual producer sites. *)
}

val entry : Sem.t -> live_out:Ccr.t -> (entry, string) result
