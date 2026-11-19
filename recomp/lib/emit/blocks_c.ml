open C

let abi_guard =
  "#if F3RT_ABI_VERSION != 7\n#error \"Generated program and f3rt ABI versions differ\"\n#endif\n"

let preamble =
  "/* Generated from user-supplied ROM. Do not commit. */\n#include <f3rt/cpu_abi.h>\n"
  ^ abi_guard ^ "#include <f3rt/block_profile.h>\n#include \"recomp/cpu_ops.h\"\n"

let declaration name = Printf.sprintf "extern void %s(f3_cpu *cpu);\n" name
let pc = Lower.field "pc"

let idle (loop : Idle_loop.t) =
  match loop.shape with
  | Spin | Affine _ -> Lower.cpu_call "F3_IDLE" []
  | Poll_bcc { address; size; cond } ->
      Lower.cpu_call "F3_IDLE_POLL_BCC"
        [Lower.expr address; UInt (Sem.bytes size); UInt (Lower.condition_code cond)]

let transfers (layout : Layout.t) name (transfer : Layout.transfer) =
  let deadline = Bin (Lt, Lower.field "steps", Lower.field "dispatch_deadline") in
  let untraced = Not (Bin (And, Lower.field "sr", UInt 0xc000)) in
  let jump target =
    let owner = Pc.Map.find target layout.owner in
    if owner = name then Goto (Layout.label target) else Chain owner
  in
  let direct = List.map (fun target ->
    If (Bin (Eq, pc, UInt target), [jump target], [])) transfer.direct in
  let indirect = match transfer.indirect with
    | [] -> []
    | cases -> [Switch (pc, List.map (fun target -> target, [jump target]) cases, [Break])] in
  let dispatched = if transfer.dispatch then [Dispatch_pc] else [] in
  match direct @ indirect @ dispatched with
  | [] -> [Return]
  | chains ->
      let guard = And_then [Not (Lower.field "halted"); Not (Lower.field "stopped"); untraced; deadline] in
      [If (guard, chains, []); Return]

let rec splice_transfer tail = List.concat_map (function
  | Transfer -> tail
  | If (expr, yes, no) -> [If (expr, splice_transfer tail yes, splice_transfer tail no)]
  | Block body -> [Block (splice_transfer tail body)]
  | Repeat body -> [Repeat (splice_transfer tail body)]
  | stmt -> [stmt])

let rec can_return = function
  | Return | Transfer | Chain _ | Dispatch_pc -> true
  | If (_, yes, no) -> List.exists can_return yes || List.exists can_return no
  | Block body | Repeat body -> List.exists can_return body
  | Switch (_, cases, default) ->
      List.exists (fun (_, body) -> List.exists can_return body) cases ||
      List.exists can_return default
  | _ -> false

let function_body ~check_yield (layout : Layout.t) (func : Layout.func) =
  let starts, ends = Emit_units.hooks layout.selection.units in
  let hooks table key pc =
    Pc.Map.find_opt pc table |> Option.value ~default:[]
    |> List.map (fun id -> Lower.cpu_call key [UInt id]) in
  let last = List.hd (List.rev func.pcs) in
  let entry address =
    let rom = not (Layout.is_ram layout address) in
    let profile = if rom then [Expr (Call ("F3_PROFILE_HIT_MAIN", [UInt address]))] else [] in
    let idle = Option.to_list (Option.map idle (Pc.Map.find_opt address layout.idle)) in
    let body = match Pc.Map.find_opt address (Lowered.bodies layout.lowered) with
      | None ->
          (match Pc.Map.find address layout.selection.entries with
           | Ram_exception vector, _ -> [Lower.cpu_call "f3_exception" [UInt vector; pc]; Return]
           | _ -> Error.fail ~where:"blocks" "missing body at %#x" address)
      | Some Fallback -> [If (Not (Lower.call "f3_fallback" []),
                        [Lower.assign "halted" (UInt 1)], []); Return]
      | Some (Native lowered) ->
          let transfer = Pc.Map.find address layout.transfers in
          let profile_indirect = match Pc.Map.find address layout.selection.entries with
            | Selection.Native insn, _ ->
                (match (Flow.of_insn insn).escape with
                 | Some (Computed _) ->
                     [Expr (Call ("F3_PROFILE_INDIRECT_MAIN", [UInt address; pc]))]
                 | _ -> [])
            | _ -> [] in
          splice_transfer (profile_indirect @ transfers layout func.name transfer) lowered.stmts
    in
    if address = last || List.exists can_return body then check_yield address;
    [Label (Layout.label address);
     Block (hooks ends "F3_UNIT_EXIT" address @ hooks starts "F3_UNIT_ENTER" address @
            profile @ idle @ body)]
  in
  let cases = List.map (fun address -> address, [Goto (Layout.label address)]) func.pcs in
  { name = func.name; body = Switch (pc, cases, [Return]) :: List.concat_map entry func.pcs }

module Names = Set.Make (String)

let file ~check_yield (layout : Layout.t) (shard : Layout.shard) =
  let starts, ends = Emit_units.hooks layout.selection.units in
  let hooked = List.exists (fun (func : Layout.func) ->
    List.exists (fun pc -> Pc.Map.mem pc starts || Pc.Map.mem pc ends) func.pcs) shard.functions in
  let externs = List.fold_left (fun acc (func : Layout.func) ->
    List.fold_left (fun acc pc ->
      let transfer = Pc.Map.find pc layout.transfers in
      List.fold_left (fun acc target -> Names.add (Pc.Map.find target layout.owner) acc)
        acc (transfer.direct @ transfer.indirect))
      acc func.pcs) Names.empty shard.functions in
  let output = Buffer.create 4096 in
  Buffer.add_string output preamble;
  if hooked then Buffer.add_string output (Emit_units.guard_lines layout.selection.units);
  Names.iter (fun name -> Buffer.add_string output (declaration name)) externs;
  List.iter (fun func -> Buffer.add_string output (print_func (function_body ~check_yield layout func))) shard.functions;
  shard.filename, Buffer.contents output

let files ~check_yield (layout : Layout.t) = List.map (file ~check_yield layout) layout.shards
