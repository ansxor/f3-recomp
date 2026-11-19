open C

type counts = { lazy_producers : int; eager_x_writes : int }

let no_flags = { lazy_producers = 0; eager_x_writes = 0 }

let add_counts left right =
  { lazy_producers = left.lazy_producers + right.lazy_producers;
    eager_x_writes = left.eager_x_writes + right.eager_x_writes }

type entry = { stmts : C.stmt list; elided : Ccr.t; counts : counts }

let uint value = UInt value
let cpu = Name "cpu"
let field name = Field (cpu, name)
let call name args = Call (name, cpu :: args)
let cpu_call name args = Expr (call name args)
let flush = cpu_call "f3_cc_flush" []
let tmp slot = "t" ^ string_of_int slot
let decl slot value = Decl (U32, tmp slot, value)
let bin op left right = Bin (op, left, right)
let assign name value = Assign (field name, value)

let flag_name = function
  | Ccr.X -> "F3_CCR_X" | N -> "F3_CCR_N" | Z -> "F3_CCR_Z"
  | V -> "F3_CCR_V" | C -> "F3_CCR_C"
let flag_bit flag = Name (flag_name flag)

let register : Sem.register -> expr = function
  | D n -> Index (field "d", uint n)
  | A n -> Index (field "a", uint n)
  | Usp -> field "usp"
  | Sfc -> field "sfc" | Dfc -> field "dfc" | Cacr -> field "cacr"
  | Vbr -> field "vbr" | Caar -> field "caar" | Msp -> field "msp" | Isp -> field "ssp"

let condition_code : Insn.cond -> int = function
  | T -> 0 | F -> 1 | Hi -> 2 | Ls -> 3 | Cc -> 4 | Cs -> 5 | Ne -> 6 | Eq -> 7
  | Vc -> 8 | Vs -> 9 | Pl -> 10 | Mi -> 11 | Ge -> 12 | Lt -> 13 | Gt -> 14 | Le -> 15

let signed_types : Sem.width -> typ * typ = function
  | Byte -> U8, I8 | Word -> U16, I16 | Long -> U32, I32

let expr_bin (op : Sem.binop) left right : C.expr =
  let simple op = Bin (op, left, right) in
  match op with
  | Add -> simple Add | Sub -> simple Sub | Mul -> simple Mul
  | And -> simple And | Or -> simple Or | Xor -> simple Xor
  | Shl -> simple Shl | Shr -> simple Shr
  | Sar -> Cast (U32, Bin (Shr, Cast (I32, left), right))
  | Eq -> simple Eq | Ne -> simple Ne | Lt -> simple Lt

let rec expr : Sem.expr -> C.expr = function
  | Const n -> uint n
  | Temp slot -> Name (tmp slot)
  | Reg reg -> register reg
  | Sr -> field "sr"
  | Ccr -> bin And (field "sr") (uint 0x1f)
  | Supervisor -> bin And (field "sr") (uint 0x2000)
  | Flag flag -> bin Ne (bin And (field "sr") (flag_bit flag)) (uint 0)
  | Cond T -> uint 1
  | Cond F -> uint 0
  | Cond cond -> Call ("f3_cond_ccr", [field "sr"; uint (condition_code cond)])
  | Bin (op, left, right) -> expr_bin op (expr left) (expr right)
  | Sext (width, value) ->
      let unsigned, signed = signed_types width in
      Cast (U32, Cast (signed, Cast (unsigned, expr value)))

let clear_pending = assign "cc_op" (Name "F3_CC_OP_NONE")

let set_sr_bits mask value =
  let kept = bin And (field "sr") (Bit_not mask) in
  assign "sr" (bin Or kept value)

let bool_bit flag value = Select (expr value, flag_bit flag, uint 0)

let install (producer : Sem.producer) =
  let op, result, width, operands = match producer with
    | Logic { result; width } -> "LOGIC", result, width, []
    | Add { src; dst; result; width } ->
        "ADD", result, width, [assign "cc_src" (expr src); assign "cc_dst" (expr dst)]
    | Sub { src; dst; result; width } ->
        "SUB", result, width, [assign "cc_src" (expr src); assign "cc_dst" (expr dst)]
    | Cmp { src; dst; result; width } ->
        "CMP", result, width, [assign "cc_src" (expr src); assign "cc_dst" (expr dst)]
  in
  operands @
  [assign "cc_result" (expr result); assign "cc_width" (uint (Sem.bytes width));
   assign "cc_op" (Name ("F3_CC_OP_" ^ op))]

let complete pc exit =
  [assign "pc" pc; assign "steps" (bin Add (field "steps") (uint 1)); exit]

let store memory width addr value =
  cpu_call (Memory_path.store memory width) [addr; value]

let or_bits = function
  | [] -> uint 0
  | bit :: rest -> List.fold_left (bin Or) bit rest

let no_elision stmts = { stmts; elided = Ccr.none; counts = no_flags }

let rec statement ~memory ~exit_live ~live ~canonical (stmt : Sem.stmt) =
  let summary = Effects.of_stmt stmt in
  let lower = function
    | Sem.Let (slot, value) -> no_elision [decl slot (expr value)]
    | Load (slot, width, addr) ->
        no_elision [decl slot (call (Memory_path.load memory width) [expr addr])]
    | Store (width, addr, value) -> no_elision [store memory width (expr addr) (expr value)]
    | Store_long_low_first (addr, value) ->
        let addr = expr addr and value = expr value in
        no_elision [store memory Word (bin Add addr (uint 2)) value;
                    store memory Word addr (bin Shr value (uint 16))]
    | Set_reg (reg, width, value) ->
        let dest = register reg and value = expr value in
        let value = match width with
          | Long -> value
          | Byte | Word ->
              let mask = uint (Sem.mask width) in
              bin Or (bin And dest (Bit_not mask)) (bin And value mask)
        in
        no_elision [Assign (dest, value)]
    | Flags producer ->
        let { Flags.x; pending } = Flags.specialise producer ~live in
        let write_x = Option.to_list (Option.map (fun value ->
            set_sr_bits (flag_bit X) (bool_bit X value)) x) in
        let update, materialised = match pending with
          | Keep -> [], Ccr.none
          | Clear -> [clear_pending], Ccr.singleton X
          | Install producer ->
              let written = if Option.is_some x then Ccr.all else Ccr.nzvc in
              install producer, written
        in
        let counts =
          { lazy_producers = (match pending with Install _ -> 1 | Keep | Clear -> 0);
            eager_x_writes = List.length write_x }
        in
        { stmts = update @ write_x; elided = Ccr.diff (Flags.writes producer) materialised; counts }
    | Set_flags values ->
        let written = List.map (fun (flag, _) -> flag_bit flag) values |> or_bits in
        let bits = List.map (fun (flag, value) -> bool_bit flag value) values |> or_bits in
        no_elision [set_sr_bits written bits; clear_pending]
    | Set_ccr value ->
        no_elision [set_sr_bits (uint 0x1f) (bin And (expr value) (uint 0x1f)); clear_pending]
    | If (test, yes, no) ->
        let yes = sequence ~memory ~exit_live ~live yes and no = sequence ~memory ~exit_live ~live no in
        { stmts = [If (expr test, yes.stmts, no.stmts)];
          elided = Ccr.union yes.elided no.elided;
          counts = add_counts yes.counts no.counts }
    | Helper helper ->
        let name, args = Helper.abi helper in
        let value = call name (List.map expr args) in
        no_elision [match Helper.result helper with
          | Some slot -> decl slot value
          | None -> Expr value]
    | Runtime runtime ->
        let name, args = Runtime_call.abi runtime in
        no_elision [cpu_call name (List.map expr args)]
    | Raise { vector; return_pc } ->
        no_elision [cpu_call "f3_exception" [uint vector; uint return_pc]; Return]
    | Goto addr -> no_elision (complete (expr addr) Transfer)
    | Stop next -> no_elision (assign "stopped" (uint 1) :: complete (uint next) Return)
    | Repeat body ->
        let body = sequence ~memory ~exit_live ~live:Ccr.all body in
        { body with stmts = [Repeat body.stmts] }
  in
  let needs_canonical = match stmt with
    | If (test, _, _) -> Effects.expr test <> Ccr.none
    | Raise _ -> true
    | Repeat _ -> false
    | Set_flags _ -> summary.writes <> Ccr.all || summary.reads <> Ccr.none
    | _ -> summary.reads <> Ccr.none
  in
  let result = lower stmt in
  let result =
    if needs_canonical && not canonical then { result with stmts = flush :: result.stmts }
    else result
  in
  (* Guest memory accesses preserve the pending CCR state. Calls with other
     effects and control flow conservatively lose the known state. *)
  let canonical = match stmt with
    | Sem.Set_flags _ | Set_ccr _ -> true
    | Let _ | Load _ | Store _ | Store_long_low_first _ | Set_reg _ ->
        canonical || needs_canonical
    | _ -> false
  in
  result, canonical

and sequence ~memory ~exit_live ~live stmts =
  let exit = Effects.make ~reads:exit_live () in
  let annotate stmt (tail, next) =
    (stmt, next.Effects.reads) :: tail, Effects.before ~exit stmt next
  in
  let stmts, _ = List.fold_right annotate stmts ([], Effects.make ~reads:live ()) in
  let lower (acc, canonical) (stmt, live) =
    let head, canonical = statement ~memory ~exit_live ~live ~canonical stmt in
    { stmts = List.rev_append head.stmts acc.stmts;
      elided = Ccr.union acc.elided head.elided;
      counts = add_counts acc.counts head.counts }, canonical
  in
  let result, _ = List.fold_left lower (no_elision [], false) stmts in
  { result with stmts = List.rev result.stmts }

let entry sem ~live_out ~memory =
  match sem with
  | Sem.Unsupported reason -> Error reason
  | Body stmts ->
      let lowered = sequence ~memory ~exit_live:live_out ~live:live_out stmts in
      Ok { lowered with stmts = C.drop_unused_decls lowered.stmts }
