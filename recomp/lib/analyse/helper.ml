type 'expr t = Btst of { value : 'expr; bit : 'expr; bytes : 'expr }

let abi = function Btst { value; bit; bytes } -> "f3_btst", [value; bit; bytes]
let reads (Btst _) = Ccr.none
let writes (Btst _) = Ccr.singleton Z
