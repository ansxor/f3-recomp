open F3recomp

type entry = { pc : int; sem : Sem.t }
type liveness = entry list -> (int -> Ccr.t)

(** Backward liveness for forward edges. All flags are observable on exit. *)
val local_liveness : liveness

type result = Passed of int | Mismatch of string

(** Compare every instruction entry over [samples] states. The optional
    [liveness] replaces the local analysis, including for mutation tests. *)
val run :
  ?liveness:liveness -> seed:int -> samples:int -> base:int ->
  F3recomp_test.Asm.statement list -> result
