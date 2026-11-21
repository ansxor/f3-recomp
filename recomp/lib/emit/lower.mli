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

val entry : ?flags:[ `Lazy | `Eager ] -> Sem.t -> live_out:Ccr.t -> memory:Memory_path.t -> (entry, string) result

val field : string -> C.expr
val call : string -> C.expr list -> C.expr
val cpu_call : string -> C.expr list -> C.stmt
val assign : string -> C.expr -> C.stmt
val expr : Sem.expr -> C.expr
val condition_code : Insn.cond -> int
