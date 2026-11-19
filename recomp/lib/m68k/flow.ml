open Insn

type escape = Return | Exception_return | Computed of Insn.ea | Module_call of Insn.ea | Raises | Halt
type t = { targets : Pc.t list; falls_through : bool; escape : escape option }

let next = { targets = []; falls_through = true; escape = None }
let escape ?(falls_through = false) escape = { targets = []; falls_through; escape = Some escape }
let branch ~falls_through target = { targets = [Pc.physical target]; falls_through; escape = None }

let transfer ea =
  match Insn.static_address ea with
  | Some target -> branch ~falls_through:false target
  | None -> escape (Computed ea)

(* The successors are immediate transfers. A call's return site belongs to
   the caller's analysis, and privilege and bus faults are outside this graph. *)
let of_insn insn =
  match insn.op with
  | Bra target | Bsr target -> branch ~falls_through:false target
  | Dbcc (T, _, _) -> next
  | Bcc (_, target) | Dbcc (_, _, target) -> branch ~falls_through:true target
  | Jmp ea | Jsr ea -> transfer ea
  | Callm (_, descriptor) -> escape (Module_call descriptor)
  | Rts | Rtd _ | Rtr | Rtm _ -> escape Return
  | Rte -> escape Exception_return
  | Trap _ | Trapcc (T, _) -> escape Raises
  | Trapcc (F, _) -> next
  | Trapv | Trapcc _ | Chk _ | Bounds (Check, _, _, _) | Div_word _ | Div_long _ ->
      escape ~falls_through:true Raises
  | Stop _ -> escape Halt
  | _ -> next

let to_tsv flow =
  let targets = List.sort_uniq Int.compare flow.targets in
  let parts =
    List.map (Printf.sprintf "0x%06x") targets
    @ (if flow.falls_through then ["next"] else [])
    @ (if Option.is_some flow.escape then ["escape"] else [])
  in
  match targets, parts with
  | [], [] -> "-"
  | [], [part] -> part
  | _ -> "branch:" ^ String.concat "," parts
