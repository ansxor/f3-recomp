type t = { reads : Ccr.t; writes : Ccr.t; boundary : bool }

let make ?(reads = Ccr.none) ?(writes = Ccr.none) ?(boundary = false) () =
  { reads; writes; boundary }

let live_in summary ~live_out =
  Ccr.union summary.reads (Ccr.diff live_out summary.writes)

let then_ first next =
  { reads = live_in first ~live_out:next.reads;
    writes = Ccr.union first.writes next.writes;
    boundary = first.boundary || next.boundary }

let condition : Insn.cond -> Ccr.t = function
  | T | F -> Ccr.none
  | Hi | Ls -> Ccr.union (Ccr.singleton C) (Ccr.singleton Z)
  | Cc | Cs -> Ccr.singleton C
  | Ne | Eq -> Ccr.singleton Z
  | Vc | Vs -> Ccr.singleton V
  | Pl | Mi -> Ccr.singleton N
  | Ge | Lt -> Ccr.union (Ccr.singleton N) (Ccr.singleton V)
  | Gt | Le -> Ccr.diff Ccr.nzvc (Ccr.singleton C)

let rec expr : Sem.expr -> Ccr.t = function
  | Sr | Ccr -> Ccr.all
  | Cond cond -> condition cond
  | Flag flag -> Ccr.singleton flag
  | Bin (_, left, right) -> Ccr.union (expr left) (expr right)
  | Sext (_, value) -> expr value
  | Const _ | Temp _ | Reg _ | Supervisor -> Ccr.none

let expressions values =
  List.fold_left (fun reads value -> Ccr.union reads (expr value)) Ccr.none values

let producer : Sem.producer -> Ccr.t = function
  | Logic { result; _ } -> expr result
  | Add { src; dst; result; _ } | Sub { src; dst; result; _ }
  | Cmp { src; dst; result; _ } -> expressions [src; dst; result]

let rec before ~exit (stmt : Sem.stmt) next =
  match stmt with
  | Let (_, value) | Load (_, _, value) | Set_reg (_, _, value) ->
      then_ (make ~reads:(expr value) ()) next
  | Store (_, addr, value) | Store_long_low_first (addr, value) ->
      then_ (make ~reads:(expressions [addr; value]) ()) next
  | Flags value -> then_ (make ~reads:(producer value) ~writes:(Flags.writes value) ()) next
  | Set_flags values ->
      let writes = List.fold_left
          (fun flags (flag, _) -> Ccr.union flags (Ccr.singleton flag)) Ccr.none values in
      then_ (make ~reads:(expressions (List.map snd values)) ~writes ()) next
  | Set_ccr value -> then_ (make ~reads:(expr value) ~writes:Ccr.all ()) next
  | If (Cond T, yes, _) -> sequence ~exit yes next
  | If (Cond F, _, no) -> sequence ~exit no next
  | If (test, yes, no) ->
      let yes = sequence ~exit yes next and no = sequence ~exit no next in
      { reads = Ccr.union (expr test) (Ccr.union yes.reads no.reads);
        writes = Ccr.inter yes.writes no.writes;
        boundary = yes.boundary || no.boundary }
  | Helper call ->
      let _, args = Helper.abi call in
      let reads = Ccr.union (Helper.reads call) (expressions args) in
      then_ (make ~reads ~writes:(Helper.writes call) ()) next
  | Runtime call ->
      let _, args = Runtime_call.abi call in
      let reads = if Runtime_call.needs_canonical_sr call then Ccr.all else expressions args in
      then_ (make ~reads ~writes:(Runtime_call.writes call) ()) next
  | Raise _ -> make ~reads:Ccr.all ~boundary:true ()
  | Goto (Const _) -> exit
  | Goto value -> then_ (make ~reads:(expr value) ~boundary:true ()) exit
  | Stop _ -> { exit with boundary = true }
  | Repeat _ -> make ~reads:Ccr.all ~boundary:true ()

and sequence ~exit stmts next = List.fold_right (before ~exit) stmts next

let of_stmt stmt = before ~exit:(make ()) stmt (make ())

let of_sem : Sem.t -> t = function
  | Body stmts -> sequence ~exit:(make ()) stmts (make ())
  | Unsupported _ -> make ~reads:Ccr.all ~boundary:true ()
