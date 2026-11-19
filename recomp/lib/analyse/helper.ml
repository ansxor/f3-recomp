type 'expr t =
  | Btst of { value : 'expr; bit : 'expr; bytes : 'expr }
  | Mul_long of { signed : 'expr; src : 'expr; low : 'expr; high : 'expr }
  | Div_word of { signed : 'expr; src : 'expr; dst : 'expr }
  | Div_long of {
      signed : 'expr; src : 'expr; high : 'expr; quotient : 'expr; remainder : 'expr;
    }

let abi = function
  | Btst { value; bit; bytes } -> "f3_btst", [value; bit; bytes]
  | Mul_long { signed; src; low; high } -> "f3_multiply_long", [signed; src; low; high]
  | Div_word { signed; src; dst } -> "f3_divide_word", [signed; src; dst]
  | Div_long { signed; src; high; quotient; remainder } ->
      "f3_divide_long", [signed; src; high; quotient; remainder]

(* Divide preserves N and Z on overflow and computes them on success. *)
let reads = function
  | Btst _ | Mul_long _ -> Ccr.none
  | Div_word _ | Div_long _ -> Ccr.union (Ccr.singleton N) (Ccr.singleton Z)

let writes = function
  | Btst _ -> Ccr.singleton Z
  | Mul_long _ -> Ccr.nzvc
  | Div_word _ | Div_long _ -> Ccr.union (Ccr.singleton V) (Ccr.singleton C)
