type 'expr t =
  | Btst of { value : 'expr; bit : 'expr; bytes : 'expr }
  | Shift of {
      operation : Insn.shift;
      value : 'expr;
      count : 'expr;
      bytes : 'expr;
      nonzero_count : bool;
      result : int;
    }
  | Bitfield of {
      operation : 'expr;
      memory : 'expr;
      location : 'expr;
      offset : 'expr;
      width : 'expr;
      result_reg : 'expr;
    }
  | Mul_long of { signed : 'expr; src : 'expr; low : 'expr; high : 'expr }
  | Div_word of { signed : 'expr; src : 'expr; dst : 'expr }
  | Div_long of {
      signed : 'expr; src : 'expr; high : 'expr; quotient : 'expr; remainder : 'expr;
    }

let abi = function
  | Btst { value; bit; bytes } -> "f3_btst", [value; bit; bytes]
  | Shift { operation; value; count; bytes; _ } ->
      let name = match operation with
        | Asr -> "f3_asr" | Asl -> "f3_asl"
        | Lsr -> "f3_lsr" | Lsl -> "f3_lsl"
        | Roxr -> "f3_roxr" | Roxl -> "f3_roxl"
        | Ror -> "f3_ror" | Rol -> "f3_rol"
      in
      name, [value; count; bytes]
  | Bitfield { operation; memory; location; offset; width; result_reg } ->
      "f3_bitfield", [operation; memory; location; offset; width; result_reg]
  | Mul_long { signed; src; low; high } -> "f3_multiply_long", [signed; src; low; high]
  | Div_word { signed; src; dst } -> "f3_divide_word", [signed; src; dst]
  | Div_long { signed; src; high; quotient; remainder } ->
      "f3_divide_long", [signed; src; high; quotient; remainder]

(* ROX consumes X. Divide preserves N and Z on overflow and computes them on
   success. *)
let reads = function
  | Shift { operation = Roxr | Roxl; _ } -> Ccr.singleton X
  | Div_word _ | Div_long _ -> Ccr.union (Ccr.singleton N) (Ccr.singleton Z)
  | Btst _ | Shift _ | Bitfield _ | Mul_long _ -> Ccr.none

(* Plain rotates preserve X. AS/LS preserve it when the masked count is zero,
   whereas ROX consumes X and produces it even at count zero. *)
let writes = function
  | Btst _ -> Ccr.singleton Z
  | Shift { operation = Roxr | Roxl; _ }
  | Shift { operation = Asr | Asl | Lsr | Lsl; nonzero_count = true; _ } -> Ccr.all
  | Shift _ | Bitfield _ | Mul_long _ -> Ccr.nzvc
  | Div_word _ | Div_long _ -> Ccr.union (Ccr.singleton V) (Ccr.singleton C)

let result = function
  | Shift { result; _ } -> Some result
  | Btst _ | Bitfield _ | Mul_long _ | Div_word _ | Div_long _ -> None
