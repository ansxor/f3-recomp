type counts = {
  lazy_producers : int;
  eager_x_writes : int;  (** Standalone X writes that accompany producer specialisation. *)
}

val no_flags : counts
val add_counts : counts -> counts -> counts

type entry = {
  stmts : C.stmt list;
  elided : Ccr.t;  (** Union of omitted writes at individual producer sites. *)
  counts : counts;
}

val entry : Sem.t -> live_out:Ccr.t -> memory:Memory_path.t -> (entry, string) result
