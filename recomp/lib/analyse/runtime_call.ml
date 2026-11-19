type 'expr t = Set_sr of 'expr | Reset_devices

let abi = function
  | Set_sr sr -> "f3_set_sr", [sr]
  | Reset_devices -> "f3_reset_devices", []

let needs_canonical_sr = function Set_sr _ | Reset_devices -> true
let writes = function Set_sr _ -> Ccr.all | Reset_devices -> Ccr.none
