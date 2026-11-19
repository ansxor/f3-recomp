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

let reads = function
  | Shift { operation = Roxr | Roxl; _ } -> Ccr.singleton X
  | Btst _ | Shift _ | Bitfield _ -> Ccr.none

(* Plain rotates preserve X. AS/LS preserve it when the masked count is zero,
   whereas ROX consumes X and produces it even at count zero. *)
let writes = function
  | Btst _ -> Ccr.singleton Z
  | Shift { operation = Roxr | Roxl; _ }
  | Shift { operation = Asr | Asl | Lsr | Lsl; nonzero_count = true; _ } -> Ccr.all
  | Shift _ | Bitfield _ -> Ccr.nzvc

let result = function
  | Shift { result; _ } -> Some result
  | Btst _ | Bitfield _ -> None
