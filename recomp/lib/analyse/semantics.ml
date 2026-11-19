(* Each family module describes the instructions it owns and returns [None]
   for the rest. *)
let families =
  [ Semantics_move.of_op; Semantics_alu.of_op; Semantics_bits.of_op;
    Semantics_arith.of_op; Semantics_flow.of_op ]

let of_insn { Insn.pc; length; op } : Sem.t =
  let ctx = { Semantics_ops.pc; next = Insn.address (pc + length) } in
  try
    match List.find_map (fun family -> family ctx op) families with
    | Some sem -> sem
    | None -> Unsupported "instruction family is outside the semantic core"
  with Semantics_ops.Unsupported_operand -> Unsupported "effective address is outside the semantic core"

let of_decoded ~pc : Decode.decoded -> Sem.t = function
  | Insn insn -> of_insn insn
  | Illegal { reason = Truncated; _ } -> Unsupported "truncated instruction"
  | Illegal { vector; _ } -> Body [Raise { vector; return_pc = pc }]
