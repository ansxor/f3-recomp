type verdict = Proven of Ccr.t | Unknown of { handler : int; at : int; why : string }

(* F3 raises levels 2 and 3. Include any other populated autovectors as
   well. Ignore erased entries on inactive levels. A zero target on an
   active level uses the uninitialized vector. *)
let handlers (rom : Image.t) =
  let vector slot = Image.u32 rom (4 * slot) in
  List.init 8 (fun i -> 24 + i)
  |> List.filter_map (fun slot ->
    let target = vector slot in
    let active = slot = 26 || slot = 27 in
    if target = 0 then (if active then Some (Pc.physical (vector 15)) else None)
    else if active || target <> 0xffffffff then Some (Pc.physical target)
    else None)
  |> List.sort_uniq Int.compare

(* F3 games run supervisor-only. Assume interrupted code holds no pointers
   below its own stack pointer. The exception frame is pushed below that
   pointer, so only values derived from entry A7 can address it. *)
(* Stack offsets are relative to entry A7. Derived marks a value whose
   offset was lost through arithmetic, a partial register write, or a spill. *)
type value = Unrelated | Constant of int | Stack of int | Derived

module Registers = Map.Make (struct
  type t = Sem.register
  let compare = compare
end)

type state = {
  registers : value Registers.t;
  slots : value Pc.Map.t;
  calls : (int * int * int) list;
  inherited : Ccr.t;
}

let derived = function Stack _ | Derived -> true | Unrelated | Constant _ -> false
let register state reg = Registers.find_opt reg state.registers |> Option.value ~default:Unrelated

let combine left right =
  if derived left || derived right then Derived else Unrelated

let address_mask = Sem.mask Long

let rec value state temps : Sem.expr -> value = function
  | Const n -> Constant n
  | Reg reg -> register state reg
  | Temp n -> Pc.Map.find_opt n temps |> Option.value ~default:Unrelated
  | Sext (Long, expr) -> value state temps expr
  | Sext (width, expr) ->
      (match value state temps expr with
       | Constant n ->
           let sign = Sem.sign width in
           Constant ((((n land Sem.mask width) lxor sign) - sign) land address_mask)
       | v -> combine v Unrelated)
  | Bin (And, Sr, Const 0x1000) -> Constant 0
  | Bin (op, left, right) ->
      let left = value state temps left and right = value state temps right in
      (match op, left, right with
       | Add, Stack a, Constant b | Add, Constant b, Stack a -> Stack ((a + b) land address_mask)
       | Sub, Stack a, Constant b -> Stack ((a - b) land address_mask)
       | Eq, Constant a, Constant b -> Constant (Bool.to_int (a = b))
       | Ne, Constant a, Constant b -> Constant (Bool.to_int (a <> b))
       | And, v, Constant mask when mask = address_mask -> v
       | _ -> combine left right)
  | Sr | Ccr | Supervisor | Cond _ | Flag _ -> Unrelated

let set_register state reg width v =
  let v = match width, v with
    | Sem.Long, Constant _ -> Unrelated
    | Long, _ -> v
    | _ -> combine (register state reg) v
  in
  let registers = match v with
    | Unrelated -> Registers.remove reg state.registers
    | _ -> Registers.add reg v state.registers
  in
  { state with registers }

(* Work RAM is mirrored every 128 KiB. Treat possible mirrors as aliases
   even when the absolute address of entry A7 is unavailable. *)
let stack_ram_mask = 0x1ffff
let covers start length byte = ((byte - start) land stack_ram_mask) < length
let overlaps start length other other_length =
  covers start length other || covers other other_length start

(* Saved aliases live in private stack slots. They are inaccessible once
   no register can address the old stack. *)
let frame_reachable state = Registers.exists (fun _ v -> derived v) state.registers

let status_logic op immediate =
  let changed = if op = Insn.And then lnot immediate else immediate in
  List.fold_left (fun mask (flag, bit) ->
    if changed land bit <> 0 then Ccr.union mask (Ccr.singleton flag) else mask)
    Ccr.none [X, 16; N, 8; Z, 4; V, 2; C, 1]

let analyse ~rom:(_ : Image.t) ~(instruction : int -> Insn.t option) (cfg : Cfg.t) handler =
  let exception Unresolved of int * string in
  let unknown at why = raise (Unresolved (at, why)) in
  let flags = ref Ccr.none in
  let apply_flags state reads writes =
    flags := Ccr.union !flags (Ccr.inter state.inherited reads);
    { state with inherited = Ccr.diff state.inherited writes }
  in
  let check_read pc addr length =
    match addr with
    | Stack start when covers start length 1 -> unknown pc "memory read may observe stacked CCR"
    | Derived -> unknown pc "memory read may observe stacked CCR"
    | _ -> ()
  in
  let check_store pc state start length =
    if List.exists (fun (_, _, slot) -> overlaps start length slot 4) state.calls
    then unknown pc "store may overwrite a direct-call return address";
    if covers start length 0 || overlaps start length 2 6
    then unknown pc "store modifies exception return frame"
  in
  let store pc state width addr v =
    let length = Sem.bytes width in
    match addr with
    | Stack start ->
        check_store pc state start length;
        let slots = Pc.Map.filter_map (fun slot v ->
          if not (overlaps start length slot 4) then Some v
          else if start = slot && length = 4 then None
          else Some Derived) state.slots in
        let slots = if derived v then begin
          if width <> Sem.Long then unknown pc "partial store of a stack-derived value";
          (* Caller-owned slots can also be read through pre-existing pointers. *)
          if start < address_mask - stack_ram_mask || start > address_mask - 3 then
            unknown pc "stack-derived value escapes the handler's private stack";
          Pc.Map.add start v slots
        end else slots in
        { state with slots }
    | Derived -> unknown pc "store through an imprecise stack address"
    | Unrelated | Constant _ ->
        if derived v then unknown pc "stack-derived value escapes to memory";
        state
  in
  let helper pc state eval : Sem.expr Helper.t -> state = function
    | Bitfield { operation = Const op; memory = Const memory; location; offset; width;
                 result_reg = Const result } ->
        let loc = eval location in
        let state = match memory, loc with
          | 1, Stack start ->
              (match eval offset, eval width with
               | Constant offset, Constant width ->
                   let offset = Int32.(to_int (of_int offset)) in
                   let first = offset asr 3 in
                   let length = (((offset land 7) + ((width - 1) land 31)) / 8) + 1 in
                   let start = (start + first) land address_mask in
                   check_read pc (Stack start) length;
                   if List.mem op [2; 4; 6; 7] then begin
                     if op = 7 && derived (register state (D result)) then
                       unknown pc "bitfield stores part of a stack-derived value";
                     check_store pc state start length;
                     if Pc.Map.exists (fun slot _ -> overlaps start length slot 4) state.slots
                     then unknown pc "bitfield modifies a saved stack-derived value"
                   end;
                   state
               | _ -> unknown pc "stack bitfield has a variable offset or width")
          | 1, Derived -> unknown pc "bitfield may observe stacked CCR"
          | 1, _ when derived (eval offset) -> unknown pc "bitfield offset is stack-derived"
          | 1, _ when op = 7 && derived (register state (D result)) ->
              unknown pc "stack-derived value escapes through a bitfield"
          | _ -> state
        in
        let input = match memory, loc with
          | 0, Constant reg -> register state (D reg)
          | 1, _ -> if Pc.Map.is_empty state.slots then Unrelated else Derived
          | _ -> Derived
        in
        if List.mem op [1; 3; 5] then set_register state (D result) Long (combine input (eval offset))
        else (match memory, loc with
          | 0, Constant reg when List.mem op [2; 4; 6; 7] ->
              let insert = if op = 7 then register state (D result) else Unrelated in
              set_register state (D reg) Long (combine input insert)
          | _ -> state)
    | Mul_long { src; low = Const low; high = Const high; _ } ->
        let v = combine (eval src) (register state (D low)) in
        let state = set_register state (D low) Long v in
        if high < 8 then set_register state (D high) Long v else state
    | Btst _ | Shift _ -> state
    | _ -> unknown pc "unmodelled helper in interrupt handler"
  in
  let check_status pc = function
    | Constant sr when sr land 0x3000 = 0x2000 -> ()
    | _ -> unknown pc "SR write may switch away from the interrupt stack"
  in
  let work = Queue.create () and seen = Hashtbl.create 32 in
  let enqueue pc state =
    if (frame_reachable state || state.inherited <> Ccr.none) &&
       not (Hashtbl.mem seen (pc, state)) then begin
      if Hashtbl.length seen >= 100_000 then unknown pc "interrupt analysis state limit reached";
      Hashtbl.add seen (pc, state) ();
      Queue.add (pc, state) work
    end
  in
  let rec inspect pc state temps stmts =
    match stmts with
    | [] -> [state]
    | stmt :: rest ->
        let eval = value state temps in
        let ordinary state =
          let summary = Effects.of_stmt stmt in
          apply_flags state summary.reads summary.writes in
        let next state = inspect pc state temps rest in
        let temp n v = inspect pc (ordinary state) (Pc.Map.add n v temps) rest in
        match stmt with
        | Sem.Let (n, expr) -> temp n (eval expr)
        | Load (n, width, addr) ->
            let addr = eval addr in
            check_read pc addr (Sem.bytes width);
            let loaded = match addr with
              | Stack start ->
                  (match Pc.Map.find_opt start state.slots with
                   | Some v when width = Long -> v
                   | _ ->
                       if Pc.Map.exists (fun slot _ -> overlaps start (Sem.bytes width) slot 4) state.slots
                       then Derived else Unrelated)
              | _ -> Unrelated
            in
            temp n loaded
        | Store (width, addr, v) -> next (ordinary (store pc state width (eval addr) (eval v)))
        | Store_long_low_first (addr, v) -> next (ordinary (store pc state Long (eval addr) (eval v)))
        | Set_reg (reg, width, expr) ->
            next (ordinary (set_register state reg width (eval expr)))
        | If (Supervisor, yes, _) | If (Cond T, yes, _) | If (Cond F, _, yes) ->
            inspect pc state temps (yes @ rest)
        | If (test, yes, no) ->
            (match eval test with
             | Constant n -> inspect pc state temps ((if n = 0 then no else yes) @ rest)
             | _ ->
                 let state = apply_flags state (Effects.expr test) Ccr.none in
                 let yes = inspect pc state temps (yes @ rest) in
                 yes @ inspect pc state temps (no @ rest))
        | Runtime (Set_sr expr) ->
            check_status pc (eval expr);
            next (apply_flags state (Effects.expr expr) Ccr.all)
        | Runtime Reset_devices -> next state
        | Helper call ->
            let state = helper pc state eval call in
            (match call with
             | Shift { value; count; result; _ } ->
                 inspect pc (ordinary state) (Pc.Map.add result (combine (eval value) (eval count)) temps) rest
             | _ -> next (ordinary state))
        | Raise _ | Repeat _ -> unknown pc "handler may raise an exception"
        | Stop resume -> enqueue resume state; []
        | _ -> next (ordinary state)
  in
  let continue ~at pc state =
    (match register state (A 7) with
     | Derived -> unknown at "stack pointer lost"
     | Unrelated | Constant _ when frame_reachable state -> unknown at "stack pointer lost"
     | _ -> ());
    enqueue pc state
  in
  let initial = {
    registers = Registers.singleton (A 7) (Stack 0);
    slots = Pc.Map.empty; calls = []; inherited = Ccr.all;
  } in
  enqueue handler initial;
  try
    while not (Queue.is_empty work) do
      let pc, state = Queue.take work in
      let insn, node = match instruction pc, Pc.Map.find_opt pc cfg.nodes with
        | Some insn, Some node when Cfg.native cfg pc -> insn, node
        | _ -> unknown pc "handler reaches code outside the native graph"
      in
      let next = Pc.physical (pc + insn.length) in
      let body () = match Semantics.of_insn insn with
        | Body stmts -> inspect pc state Pc.Map.empty stmts
        | Unsupported why -> unknown pc why
      in
      let follow state =
        List.iter (function
          | Cfg.Raises -> ()  (* Feasible raises were checked in the body. *)
          | Leaves_native target -> continue ~at:pc target state
          | _ -> unknown pc "control leaves the handler graph") node.boundaries;
        List.iter (fun target -> continue ~at:pc target state) node.succ
      in
      match insn.op with
      | Rte ->
          (* Restoring the original SR to resume interrupted code is not a
             handler observation of the saved CCR. *)
          if state.calls <> [] then unknown pc "exception return with a pending direct call";
          if register state (A 7) <> Stack 0 then unknown pc "RTE at a nonzero stack offset"
      | Rts | Rtd _ ->
          (match state.calls with
           | (_, resume, slot) :: rest when register state (A 7) = Stack slot ->
               List.iter (fun state -> continue ~at:pc resume { state with calls = rest }) (body ())
           | _ -> unknown pc "return does not use a saved direct-call address")
      | Bsr _ | Jsr _ ->
          (match node.succ, node.boundaries with
           | [target], [] ->
               if target = handler || List.exists (fun (entry, _, _) -> entry = target) state.calls
               then unknown pc "recursive direct call";
               List.iter (fun state ->
                 match register state (A 7) with
                 | Stack slot -> continue ~at:pc target { state with calls = (target, next, slot) :: state.calls }
                 | _ -> unknown pc "stack pointer lost at direct call") (body ())
           | _ -> unknown pc "cannot resolve call target")
      | Movec (From_control, _, Msp) ->
          unknown pc "MSP may address a master-mode interrupt frame"
      | Logic_status (op, status, immediate) ->
          let writes = status_logic op immediate in
          let reads = if writes = Ccr.none then Ccr.none else Ccr.all in
          (match status with
           | Sr ->
               let mode = 0x2000 and mask = 0x3000 in
               let result = match op with
                 | And -> mode land immediate | Or -> mode lor immediate | Eor -> mode lxor immediate
                 | _ -> assert false
               in
               check_status pc (Constant (result land mask))
           | Ccr -> ());
          follow (apply_flags state reads writes)
      | Stop sr when sr land 0xc700 = 0x0700 ->
          (* The interrupt mask blocks every level raised on F3, and trace is off. *)
          check_status pc (Constant sr)
      | _ -> List.iter follow (body ())
    done;
    Proven !flags
  with Unresolved (at, why) -> Unknown { handler; at; why }

let observed (program : Program.t) cfg =
  let rom = program.game.rom in
  List.map (analyse ~rom ~instruction:(Program.insn program) cfg) (handlers rom)
