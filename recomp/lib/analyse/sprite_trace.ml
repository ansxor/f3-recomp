open Insn
open Sprite_value

type seed = { pc : Pc.t; lane : Sprite_groups.lane option }

type context = {
  rom_size : int;
  instructions : Insn.t Pc.Map.t;
  functions : Pc.Set.t;
  handlers : Pc.Set.t Pc.Map.t;
  ram_constants : Int_set.t Pc.Map.t;
  work_base : Value.t option;
}

let bytes = function Byte -> 1 | Word -> 2 | Long -> 4
let addr_register n = State.register (A n)

let direct_memory = function
  | Ind reg -> Some (addr_register reg, 0)
  | Disp { reg; disp } -> Some (addr_register reg, disp)
  | _ -> None

let cursor_memory = function
  | Postinc reg | Predec reg -> Some (addr_register reg, 0)
  | ea -> direct_memory ea

let static_address = function Absolute n | Pc_rel n -> Some (address n) | _ -> None

let constant_address state ea =
  match direct_memory ea with
  | Some (base, disp) -> Option.map (fun n -> address (n + disp)) (Value.const32 (State.get state base))
  | None -> static_address ea

let tagged ?(roots = Fields.empty) kind tags =
  Value.uniform kind { Word.empty with roots; tags }

let read_memory context state size ea =
  match ea with
  | Postinc reg | Predec reg ->
      tagged Stream (Tags.add Stream (Value.tags (State.get state (addr_register reg))))
  | Indexed { base = Areg _; indirect = (Pre _ | Post _); _ } ->
      tagged Stream (Tags.singleton Stream)
  | Indexed { base = Areg reg; indirect = Direct; _ } ->
      let base = State.get state (addr_register reg) in
      tagged ~roots:(Value.provenance base) Index (Tags.add Index (Value.tags base))
  | Absolute n | Pc_rel n ->
      let kind = address_kind ~rom_size:context.rom_size (address n) in
      tagged kind (Tags.singleton kind)
  | _ ->
      match direct_memory ea with
      | None -> tagged Mem (Tags.singleton Mem)
      | Some (base, offset) ->
          let value = State.get state base in
          let root =
            base <> State.sp && base <> State.work && not value.cursor
            && Value.const32 value = None && value.kind <> Rom && value.kind <> Stack
          in
          if root then
            if size = Long then Value.words ~hi:(Word.field base offset) ~lo:(Word.field base (offset + 2)) Mem
            else Value.words ~lo:(Word.field base offset) Mem
          else
            let kind = match value.kind with Rom | Ram -> value.kind | _ -> Derived in
            tagged ~roots:(Value.provenance value) kind (Tags.add Derived (Value.tags value))

let operand_value context state size = function
  | Register reg -> State.get state (State.register reg)
  | Immediate n when size = Long -> Value.constant ~rom_size:context.rom_size n
  | Immediate n ->
      let mask = if size = Byte then 0xff else 0xffff in
      Value.words ~lo:{ Word.empty with const = Some (n land mask) } Unknown
  | ea -> read_memory context state size ea

let move_value context ~input ~current ~address_register size source =
  let value = operand_value context input size source in
  match size, address_register with
  | Word, true -> Value.words ~hi:(Word.sign_extension value.lo) ~lo:value.lo Other
  | (Byte | Word), _ -> Value.words ~hi:current.Value.hi ~lo:value.lo Other
  | Long, _ ->
      match source with
      | Register _ -> value
      | Immediate _ -> Value.without_cursor value
      | ea ->
          match constant_address input ea with
          | None -> Value.without_cursor value
          | Some addr ->
              match ea with
              | Ind _ | Disp _ | Absolute _ ->
                  let offsets =
                    match Pc.Map.find_opt addr context.ram_constants with
                    | Some constants when Int_set.for_all sprite_address constants ->
                        Int_set.map (fun n -> n land 15) constants
                    | _ -> Int_set.empty
                  in
                  Value.cursor_offsets value offsets
              | _ -> Value.at_address value addr

let address_arithmetic ~subtract current source_ea source =
  match source_ea with
  | Immediate delta ->
      let delta = signed_word delta in
      Value.bump current (if subtract then -delta else delta)
  | _ ->
      let kind =
        match current.Value.kind with
        | Param | Mem | Derived | Stream | Ram | Other | Io | Rom -> current.kind
        | _ -> Value.join_kind current.kind source.Value.kind
      in
      Value.unknown kind

let data_arithmetic ~subtract size (current : Value.t) (source : Value.t) =
  let add = Word.add ~subtract in
  let hi = if size = Long then add current.hi source.hi else current.hi in
  Value.words ~hi ~lo:(add current.lo source.lo) (Value.join_kind current.kind source.kind)

let logic operation size (current : Value.t) source_ea (source : Value.t) =
  let immediate word mask =
    { word with Word.const = Option.map (fun n -> operation n mask) word.Word.const; transformed = true }
  in
  let hi, lo =
    match source_ea with
    | Immediate n ->
        let hi = if size = Long then immediate current.hi (word_mask (n lsr 16)) else current.hi in
        hi, immediate current.lo (n land (if size = Byte then 0xff else 0xffff))
    | _ ->
        let hi = if size = Long then Word.derive ~other:source.hi current.hi else current.hi in
        hi, Word.derive ~other:source.lo current.lo
  in
  Value.words ~hi ~lo Data

let multiply_divide context operation (current : Value.t) (source : Value.t) =
  let constant =
    match current.lo.const, source.lo.const with
    | Some x, Some y -> operation x y
    | _ -> None
  in
  match constant with
  | Some n -> Value.constant ~rom_size:context.rom_size n
  | None -> Value.words ~hi:(Word.derive current.hi) ~lo:(Word.derive ~other:source.lo current.lo) Unknown

let shift_value context operation size count (current : Value.t) =
  match operation, count, current.lo.const with
  | (Lsl | Asl | Lsr | Asr), Constant count, Some value ->
      let count = count land 63 in
      let shifted =
        match operation with
        | Lsl | Asl -> Int64.(to_int (shift_left (of_int value) count))
        | Lsr -> Int64.(to_int (shift_right_logical (of_int value) count))
        | _ ->
            let value = if size = Word then signed_word value else value in
            Int64.(to_int (shift_right (of_int value) count))
      in
      Value.constant ~rom_size:context.rom_size (if size = Long then shifted else word_mask shifted)
  | _ -> Value.words ~hi:(Word.derive current.hi) ~lo:(Word.derive current.lo) Unknown

let unary operation size (current : Value.t) =
  let zero = { Word.empty with const = Some 0 } in
  match operation with
  | Clr -> Value.words ~hi:(if size = Long then zero else current.hi) ~lo:zero Const
  | Not -> Value.words ~hi:(Word.derive current.hi) ~lo:(Word.derive current.lo) Data
  | Neg | Negx ->
      let hi = { current.hi with transformed = true } in
      let lo = { current.lo with transformed = true; const = Option.map (fun n -> word_mask (-n)) current.lo.const } in
      Value.words ~hi ~lo Data
  | Tst -> current
  | Nbcd | Tas -> Value.unknown Unknown

let operands = function
  | Move (size, src, dst) | Alu (_, size, src, dst)
  | Addx (size, src, dst) | Subx (size, src, dst) | Movep (size, src, dst) -> size, [src; dst]
  | Movea (size, src, _) | Adda (size, src, _) | Suba (size, src, _) | Cmpa (size, src, _) -> size, [src]
  | Unary (_, size, ea) | Shift (_, size, _, ea) | Movem (_, size, _, ea)
  | Chk (size, ea, _) | Moves (_, size, _, ea) | Cas (size, _, ea) -> size, [ea]
  | Mul_word (_, ea, _) | Div_word (_, ea, _) -> Word, [ea]
  | Abcd (src, dst) | Sbcd (src, dst) | Pack (src, dst, _) | Unpk (src, dst, _) -> Long, [src; dst]
  | Bit (_, _, ea) | Lea (ea, _) | Pea ea | Jmp ea | Jsr ea | Callm (_, ea)
  | Mul_long { operand = ea; _ } | Div_long { operand = ea; _ }
  | Bounds (_, _, ea, _) | Bitfield { operand = ea; _ } -> Long, [ea]
  | From_status (_, ea) | To_status (_, ea) -> Word, [ea]
  | _ -> Long, []

let auto_update stage size state ea =
  match stage, ea with
  | `Before, Predec reg | `After, Postinc reg ->
      let index = addr_register reg in
      let delta = bytes size * (if stage = `Before then -1 else 1) in
      State.update state index (Value.bump (State.get state index) delta)
  | _ -> state

(* The transfer tracks record fields and sprite cursor positions. It does
   not simulate the instruction's flags, memory contents or full arithmetic. *)
let transfer_with context insn input =
  let size, eas = operands insn.op in
  let before = List.fold_left (auto_update `Before size) input eas in
  let get reg = State.get before (State.register reg) in
  let put reg value = State.update before (State.register reg) value in
  let update reg f = put reg (f (get reg)) in
  let source size ea = operand_value context input size ea in
  let unknown reg = put reg (Value.unknown Unknown) in
  let arithmetic subtract size ea reg =
    let data = source size ea in
    match reg with
    | A _ -> update reg (fun current -> address_arithmetic ~subtract current ea data)
    | D _ -> update reg (fun current -> data_arithmetic ~subtract size current data)
  in
  let product operation size ea reg =
    update (D reg) (fun current -> multiply_divide context operation current (source size ea))
  in
  let after =
    match insn.op with
    | Move (size, ea, Register reg) ->
        update reg (fun current -> move_value context ~input ~current ~address_register:false size ea)
    | Movea (size, ea, reg) ->
        update (A reg) (fun current -> move_value context ~input ~current ~address_register:true size ea)
    | Moveq (n, reg) -> put (D reg) (Value.constant ~rom_size:context.rom_size n)
    | Lea (ea, reg) ->
        let value =
          match constant_address input ea with
          | None -> Value.unknown Addr
          | Some addr -> Value.at_address (Value.constant ~rom_size:context.rom_size addr) addr
        in
        put (A reg) value
    | Alu ((Add | Sub as operation), size, ea, Register reg) ->
        arithmetic (operation = Sub) size ea reg
    | Adda (size, ea, reg) -> arithmetic false size ea (A reg)
    | Suba (size, ea, reg) -> arithmetic true size ea (A reg)
    | Alu ((And | Or | Eor as operation), size, ea, Register reg) ->
        let apply = match operation with And -> ( land ) | Or -> ( lor ) | _ -> ( lxor ) in
        update reg (fun current -> logic apply size current ea (source size ea))
    | Unary (operation, size, Register reg) -> update reg (unary operation size)
    | Shift (operation, size, count, Register reg) -> update reg (shift_value context operation size count)
    | Mul_word (sign, ea, reg) ->
        let apply x y = Some (if sign = Signed then signed_word x * signed_word y else x * y) in
        product apply Word ea reg
    | Mul_long { sign; operand; low; high = None } ->
        let apply x y = Some (if sign = Signed then signed_word x * signed_word y else x * y) in
        product apply Long operand low
    | Div_word (sign, ea, reg) ->
        product (fun x y -> if sign = Unsigned && y <> 0 then Some (x / y) else None) Word ea reg
    | Bit ((Bchg | Bclr | Bset), _, Register reg) ->
        update reg (fun value -> Value.words ~hi:(Word.derive value.hi) ~lo:(Word.derive value.lo) Data)
    | Dbcc (_, reg, _) ->
        update (D reg) (fun value ->
          { value with lo = { value.lo with const = Option.map (fun n -> word_mask (n - 1)) value.lo.const } })
    | Ext (_, reg) ->
        update (D reg) (fun value -> Value.words ~hi:(Word.sign_extension value.lo) ~lo:value.lo Data)
    | Swap reg -> update (D reg) (fun value -> { value with hi = value.lo; lo = value.hi })
    | Exg (a, b) -> State.update (put a (get b)) (State.register b) (get a)
    | Link (reg, _) -> put (A reg) (Value.unknown Stack)
    | Unlk reg -> put (A 7) (get (A reg))
    | Addx (_, _, Register reg) | Subx (_, _, Register reg)
    | Abcd (_, Register reg) | Sbcd (_, Register reg) | Scc (_, Register reg)
    | From_status (_, Register reg) -> unknown reg
    | _ -> before
  in
  match insn.op with Jmp _ | Jsr _ -> after | _ -> List.fold_left (auto_update `After size) after eas

let empty_context rom_size =
  { rom_size; instructions = Pc.Map.empty; functions = Pc.Set.empty;
    handlers = Pc.Map.empty; ram_constants = Pc.Map.empty; work_base = None }

let transfer insn state = transfer_with (empty_context 0) insn state

let add_to_set key value map =
  Pc.Map.update key (function
    | None -> Some (Int_set.singleton value)
    | Some values -> Some (Int_set.add value values)) map

let scan_handlers instructions =
  Pc.Map.fold (fun _ insn handlers ->
    match insn.op with
    | Move (Long, Immediate target, Disp { disp; _ }) when Pc.Map.mem (address target) instructions ->
        add_to_set disp (address target) handlers
    | _ -> handlers) instructions Pc.Map.empty

let scan_work_bases instructions =
  Pc.Map.fold (fun _ insn bases ->
    match insn.op with
    | Lea (ea, 5) | Movea (_, ea, 5) ->
        (match static_address ea with
         | Some base when 0x400000 <= base && base < 0x420000 -> Int_set.add base bases
         | _ -> bases)
    | _ -> bases) instructions Int_set.empty

let scan_ram_constants instructions work_bases =
  Pc.Map.fold (fun _ insn summary ->
    match insn.op with
    | Move (size, Immediate value, destination) ->
        let value = match size with Byte -> value land 0xff | Word -> word_mask value | Long -> address value in
        let addresses =
          match destination, direct_memory destination with
          | Absolute addr, _ -> [address addr]
          | _, Some (base, disp) when base = State.work ->
              List.map (fun work -> address (work + disp)) (Int_set.elements work_bases)
          | _ -> []
        in
        List.fold_left (fun summary addr ->
          if ram_address addr || sprite_address addr then add_to_set addr value summary
          else summary) summary addresses
    | _ -> summary) instructions Pc.Map.empty

let context program =
  let rom_size = Image.length program.Program.game.rom in
  let instructions = Pc.Map.filter_map (fun pc decoded ->
    match decoded with Decode.Insn insn when pc < rom_size -> Some insn | _ -> None) program.decoded
  in
  let work_bases = scan_work_bases instructions in
  let work_base =
    Int_set.fold (fun base value ->
      let constant = Value.constant ~rom_size base in
      Some (match value with None -> constant | Some value -> Value.merge value constant))
      work_bases None
  in
  { rom_size; instructions; functions = program.functions; work_base;
    handlers = scan_handlers instructions;
    ram_constants = scan_ram_constants instructions work_bases }

let direct_target context op =
  let target =
    match op with
    | Bcc (_, pc) | Dbcc (_, _, pc) | Bra pc | Bsr pc -> Some pc
    | Jmp ea | Jsr ea -> static_address ea
    | _ -> None
  in
  match target with Some pc when 0 <= pc && pc < context.rom_size && pc land 1 = 0 -> target | _ -> None

(* Traverse the same local edges as the provenance analysis's reference.
   Calls propagate to the callee and return site. A PC-indexed jump table
   does not use the record-handler assignments collected by scan_handlers. *)
let static_successors context (insn : Insn.t) =
  let next = insn.pc + insn.length in
  let targets =
    match insn.op with
    | Jmp (Indexed { base = Areg _; disp; indirect = Pre _; _ })
    | Jsr (Indexed { base = Areg _; disp; indirect = Pre _; _ })
    | Jmp (Indexed { base = Areg _; indirect = Post disp; _ })
    | Jsr (Indexed { base = Areg _; indirect = Post disp; _ }) ->
        (match Pc.Map.find_opt disp context.handlers with None -> [] | Some pcs -> Int_set.elements pcs)
    | op -> Option.to_list (direct_target context op)
  in
  let candidates =
    match insn.op with
    | Rts | Rtd _ | Rtr | Rte -> []
    | Bra _ | Jmp _ -> targets
    | _ -> next :: targets
  in
  candidates |> List.filter (fun pc -> 0 <= pc && pc < context.rom_size) |> List.sort_uniq Int.compare

let dynamic_successors context insn state static =
  match insn.op with
  | Jsr ea | Jmp ea ->
      (match direct_memory ea with
       | None -> static
       | Some _ ->
           match constant_address state ea with
           | Some pc when Pc.Map.mem pc context.instructions -> List.sort_uniq Int.compare (pc :: static)
           | _ -> static)
  | _ -> static

let entry_seeds context writers =
  let exists pc = Pc.Map.mem pc context.instructions in
  let functions = Pc.Set.filter exists context.functions in
  let extra = Pc.Set.filter (fun pc -> exists pc && not (Pc.Set.mem pc functions)) writers in
  match Pc.Set.elements functions @ Pc.Set.elements extra with
  | [] -> List.map fst (Option.to_list (Pc.Map.min_binding_opt context.instructions))
  | seeds -> seeds

let static_graph context seeds =
  let graph = Hashtbl.create 1024 and incoming = Hashtbl.create 1024 in
  let rec visit = function
    | [] -> ()
    | pc :: rest when Hashtbl.mem graph pc -> visit rest
    | pc :: rest ->
        match Pc.Map.find_opt pc context.instructions with
        | None -> visit rest
        | Some insn ->
            let targets = static_successors context insn in
            Hashtbl.add graph pc targets;
            List.iter (fun target -> Hashtbl.replace incoming target ()) targets;
            visit (targets @ rest)
  in
  visit seeds;
  graph, incoming

let solve context writers =
  let seeds = entry_seeds context writers in
  (* A seeded function with a known incoming edge inherits the caller's
     state. Mixing in fresh parameters would erase that provenance. *)
  let graph, incoming = static_graph context seeds in
  let default = State.default context.work_base in
  let entries = Hashtbl.create (Hashtbl.length graph) in
  let queued = Hashtbl.create (List.length seeds) in
  let queue = Stack.create () in
  let enqueue pc =
    if not (Hashtbl.mem queued pc) then (
      Hashtbl.add queued pc ();
      Stack.push pc queue)
  in
  let propagate pc state =
    if Pc.Map.mem pc context.instructions then (
      let state =
        match context.work_base with
        | Some value when Pc.Set.mem pc context.functions -> State.update state State.work value
        | _ -> state
      in
      match Hashtbl.find_opt entries pc with
      | None -> Hashtbl.add entries pc state; enqueue pc
      | Some previous ->
          let merged = State.merge previous state in
          if previous != merged then (
            Hashtbl.replace entries pc merged;
            enqueue pc))
  in
  List.iter (fun pc ->
    if not (Hashtbl.mem incoming pc) then Hashtbl.add entries pc default;
    enqueue pc) seeds;
  let transfers = ref 0 in
  while not (Stack.is_empty queue) do
    let pc = Stack.pop queue in
    Hashtbl.remove queued pc;
    match Hashtbl.find_opt entries pc with
    | None -> ()
    | Some state ->
        incr transfers;
        if !transfers > 4_000_000 then Error.fail "sprite trace did not converge";
        let insn = Pc.Map.find pc context.instructions in
        let static =
          match Hashtbl.find_opt graph pc with
          | Some targets -> targets
          | None ->
              let targets = static_successors context insn in
              Hashtbl.add graph pc targets;
              targets
        in
        let output = transfer_with context insn state in
        dynamic_successors context insn state static |> List.iter (fun target -> propagate target output)
  done;
  Pc.Set.iter (fun pc ->
    if Pc.Map.mem pc context.instructions && not (Hashtbl.mem entries pc) then
      Hashtbl.add entries pc default) writers;
  entries

type store_source = Operand of ea | Status of status

type store = {
  size : size;
  source : store_source option;
  destination : ea;
  mnemonic : string;
  read_modify_write : bool;
}

let store op =
  let make mnemonic ?(read_modify_write = false) size source destination =
    match destination with
    | Register _ | Immediate _ -> None
    | _ -> Some { size; source; destination; mnemonic; read_modify_write }
  in
  let binary name size src dst = make name ~read_modify_write:true size (Some (Operand src)) dst in
  match op with
  | From_status (status, dst) -> make "move" Word (Some (Status status)) dst
  | Move (size, src, dst) -> make "move" size (Some (Operand src)) dst
  | Movep (size, src, dst) -> make "movep" size (Some (Operand src)) dst
  | Alu (Cmp, _, _, _) -> None
  | Alu (op, size, src, dst) ->
      let name = match op with Add -> "add" | Sub -> "sub" | And -> "and" | Or -> "or" | Eor -> "eor" | Cmp -> "cmp" in
      binary name size src dst
  | Addx (size, src, dst) -> binary "addx" size src dst
  | Subx (size, src, dst) -> binary "subx" size src dst
  | Abcd (src, dst) -> make "abcd" Long (Some (Operand src)) dst
  | Sbcd (src, dst) -> make "sbcd" Long (Some (Operand src)) dst
  | Unary ((Clr | Tas as op), size, dst) -> make (if op = Clr then "clr" else "tas") size None dst
  | Unary ((Neg | Negx | Not as op), size, dst) ->
      let name = match op with Neg -> "neg" | Negx -> "negx" | _ -> "not" in
      make name ~read_modify_write:true size None dst
  | Shift (op, size, _, dst) ->
      let name = match op with Asr -> "asr" | Asl -> "asl" | Lsr -> "lsr" | Lsl -> "lsl"
        | Roxr -> "roxr" | Roxl -> "roxl" | Ror -> "ror" | Rol -> "rol" in
      make name ~read_modify_write:true size None dst
  | Bit ((Bchg | Bclr | Bset as op), count, dst) ->
      let name = match op with Bchg -> "bchg" | Bclr -> "bclr" | _ -> "bset" in
      let src = match count with Constant n -> Immediate n | Data_register n -> Register (D n) in
      binary name Byte src dst
  | Scc ((T | F | Cc as cond), dst) ->
      make (match cond with T -> "st" | F -> "sf" | _ -> "scc") Byte None dst
  | _ -> None

let signed_hex n = if n < 0 then Printf.sprintf "-$%x" (-n) else Printf.sprintf "$%x" n

let describe_ea = function
  | Register reg -> Field.register_name (State.register reg)
  | Ind reg -> Printf.sprintf "(a%d)" reg
  | Postinc reg -> Printf.sprintf "(a%d)+" reg
  | Predec reg -> Printf.sprintf "-(a%d)" reg
  | Disp { reg; disp } -> Printf.sprintf "%s(a%d)" (signed_hex disp) reg
  | Immediate n -> "#" ^ signed_hex n
  | Absolute n | Pc_rel n -> signed_hex n
  | Indexed _ -> "(indexed)"

let describe_store store =
  let suffix = match store.size with Byte -> "b" | Word -> "w" | Long -> "l" in
  let source = function Operand ea -> describe_ea ea | Status Sr -> "sr" | Status Ccr -> "ccr" in
  let operands = List.map source (Option.to_list store.source) @ [describe_ea store.destination] in
  Printf.sprintf "%s.%s %s" store.mnemonic suffix (String.concat ", " operands)

let store_value context state size = function
  | Operand ea -> operand_value context state size ea
  | Status _ -> Value.unknown Unknown

let writer_lane lanes pc store base disp state =
  let open Sprite_groups in
  let cursor = State.get state base in
  match Pc.Map.find_opt pc lanes, store.size with
  | Some (X | Y as lane), Word | Some (XY as lane), Long -> Some lane
  | _ when cursor.cursor && Int_set.cardinal cursor.offsets = 1 ->
      (match store.size, (Int_set.min_elt cursor.offsets + disp) land 15 with
       | Word, 4 -> Some X | Word, 6 -> Some Y | Long, 4 -> Some XY | _ -> None)
  | _ -> None

let classify_writer context pc store src cursor_reg lane state =
  let open Sprite_groups in
  let source = store_value context state store.size src in
  let x_word = if store.size = Long then source.hi else source.lo in
  let y_word = source.lo in
  let x = if lane = Y then None else Word.anchor x_word in
  let y = if lane = X then None else Word.anchor y_word in
  let roots = List.sort_uniq Field.compare (Option.to_list x @ Option.to_list y) in
  let base = match roots with [] -> cursor_reg | field :: _ -> field.Field.base in
  let unresolved reason : (writer, unresolved) result =
    let base_reg = match x with Some root -> root.base | None -> base in
    Error { pc; cursor_reg; base_reg; lane; destination = describe_ea store.destination; reason }
  in
  let missing =
    (if lane <> Y && x = None then ["X: " ^ Word.unresolved_reason x_word] else [])
    @ (if lane <> X && y = None then ["Y: " ^ Word.unresolved_reason y_word] else [])
  in
  match missing, x, y with
  | _ :: _, _, _ -> unresolved (String.concat "; " missing)
  | [], Some x, Some y when x.base <> y.base ->
      unresolved ("ambiguous root base: " ^ Word.describe_fields (Fields.of_list roots))
  | _ ->
      let offset = function None -> absent | Some field -> field.Field.offset in
      let rigid = (lane = Y || Word.rigid x_word) && (lane = X || Word.rigid y_word) in
      Ok { pc; cursor_reg; base; lane; x_offset = offset x; y_offset = offset y;
           group_class = 0; rigid; root_offsets = roots }

let collect_writers context entries lanes =
  Hashtbl.to_seq_keys entries |> List.of_seq |> List.sort Int.compare
  |> List.fold_left (fun (writers, unresolved) pc ->
    let insn = Pc.Map.find pc context.instructions in
    let state = Hashtbl.find entries pc in
    match store insn.op with
    | Some ({ source = Some src; _ } as store) ->
        (match cursor_memory store.destination with
         | Some (base, disp) ->
             (match writer_lane lanes pc store base disp state with
              | None -> writers, unresolved
              | Some lane ->
                  match classify_writer context pc store src base lane state with
                  | Ok writer -> writer :: writers, unresolved
                  | Error writer -> writers, writer :: unresolved)
         | None -> writers, unresolved)
    | _ -> writers, unresolved) ([], [])
  |> fun (writers, unresolved) -> Sprite_groups.assign_classes (List.rev writers), List.rev unresolved

let field_write context entries default pc store field more start =
  let open Sprite_groups in
  let instruction = describe_store store in
  let kind, detail =
    match store.read_modify_write, store.source with
    | true, _ -> Continuation, " (read-modify-write of the field)"
    | false, None -> Reset, ""
    | false, Some source ->
        let state = Option.value (Hashtbl.find_opt entries pc) ~default in
        let value = store_value context state store.size source in
        let word = if store.size = Long && field.Field.offset = start then value.hi else value.lo in
        let roots = Word.provenance word in
        if Fields.mem field roots then Continuation, " <- field"
        else
          let same_record = not (Fields.is_empty roots) && Fields.for_all (fun root -> root.Field.base = field.base) roots in
          let projected = Fields.cardinal roots >= 2 || (Fields.cardinal roots = 1 && word.transformed) in
          let fields = Word.describe_fields roots in
          if same_record && word.exact && projected then Continuation, " <- same-record projection from " ^ fields
          else Reset, " <- " ^ fields
  in
  let additional =
    match more with
    | [] -> ""
    | more ->
        let offsets = List.map (fun root -> Printf.sprintf "+0x%02x" root.Field.offset) more in
        " (also writes " ^ String.concat ", " offsets ^ ")"
  in
  { pc; base = field.base; offset = field.offset; kind; instruction; evidence = instruction ^ detail ^ additional }

let classify_fields context entries writers =
  let fields = Sprite_groups.fields writers |> Fields.elements in
  let default = State.default context.work_base in
  if fields = [] then []
  else
    Pc.Map.fold (fun pc insn classifications ->
      match store insn.op with
      | None -> classifications
      | Some store ->
          match direct_memory store.destination with
          | None -> classifications
          | Some (base, start) ->
              let covers (field : Field.t) =
                base = field.base && start <= field.offset
                && field.offset + 2 <= start + bytes store.size
                && (field.offset - start) land 1 = 0
              in
              match List.filter covers fields with
              | [] -> classifications
              | field :: more -> field_write context entries default pc store field more start :: classifications)
      context.instructions []
    |> List.rev

let run program ~(config : Config.t) ~writers =
  let context = context program in
  let seeds, lanes = List.fold_left (fun (seeds, lanes) seed ->
    let pc = Pc.physical seed.pc in
    let lanes = match seed.lane with None -> lanes | Some lane -> Pc.Map.add pc lane lanes in
    Pc.Set.add pc seeds, lanes) (Pc.Set.empty, Pc.Map.empty) writers
  in
  let entries = solve context seeds in
  let writers, unresolved = collect_writers context entries lanes in
  let classifications = classify_fields context entries writers in
  let dispatch_targets =
    Pc.Map.fold (fun _ targets count -> count + Pc.Set.cardinal targets) context.handlers 0
  in
  Sprite_groups.make ~game_id:config.game.id ~instructions:(Pc.Map.cardinal context.instructions)
    ~analysed:(Hashtbl.length entries) ~dispatch_targets ~writers ~unresolved ~classifications

let load_writer_seeds path =
  (* The seed format only consumes integral PCs and string lane names. Keep
     other JSON values opaque so annotations on captures can be ignored. *)
  let fail format = Error.fail ~where:path format in
  let input = In_channel.with_open_bin path In_channel.input_all in
  let length = String.length input and position = ref 0 in
  let peek () = if !position < length then input.[!position] else '\000' in
  let advance () = incr position in
  let rec whitespace () =
    match peek () with ' ' | '\t' | '\r' | '\n' -> advance (); whitespace () | _ -> ()
  in
  let invalid () = fail "invalid sprite writers JSON at byte %d" !position in
  let expect c = whitespace (); if peek () = c then advance () else invalid () in
  let string () =
    expect '"';
    let buffer = Buffer.create 16 in
    let unicode () =
      if !position + 4 > length then invalid ();
      let code = try int_of_string ("0x" ^ String.sub input !position 4) with Failure _ -> invalid () in
      position := !position + 4;
      if Uchar.is_valid code then Buffer.add_utf_8_uchar buffer (Uchar.of_int code)
      else Buffer.add_char buffer '?'
    in
    let rec read () =
      match peek () with
      | '"' -> advance (); Buffer.contents buffer
      | '\\' ->
          advance ();
          let escaped = peek () in
          advance ();
          (match escaped with
           | '"' | '\\' | '/' -> Buffer.add_char buffer escaped
           | 'b' -> Buffer.add_char buffer '\b'
           | 'f' -> Buffer.add_char buffer '\012'
           | 'n' -> Buffer.add_char buffer '\n'
           | 'r' -> Buffer.add_char buffer '\r'
           | 't' -> Buffer.add_char buffer '\t'
           | 'u' -> unicode ()
           | _ -> invalid ());
          read ()
      | '\000' .. '\031' -> invalid ()
      | c -> Buffer.add_char buffer c; advance (); read ()
    in
    read ()
  in
  let take_digits () =
    while match peek () with '0' .. '9' -> advance (); true | _ -> false do () done
  in
  let number () =
    let start = !position in
    if peek () = '-' then advance ();
    (match peek () with '0' -> advance () | '1' .. '9' -> take_digits () | _ -> invalid ());
    let integer_end = !position in
    let digits () =
      match peek () with '0' .. '9' -> take_digits () | _ -> invalid ()
    in
    if peek () = '.' then (advance (); digits ());
    (match peek () with
     | 'e' | 'E' ->
         advance ();
         if peek () = '-' || peek () = '+' then advance ();
         digits ()
     | _ -> ());
    if !position <> integer_end then `Other
    else match int_of_string_opt (String.sub input start (integer_end - start)) with
      | Some n -> `Int n | None -> `Other
  in
  let literal text =
    String.iter (fun c -> if peek () = c then advance () else invalid ()) text
  in
  let sequence closing item =
    let rec items acc =
      let item = item () in
      whitespace ();
      match peek () with
      | ',' -> advance (); items (item :: acc)
      | c when c = closing -> advance (); List.rev (item :: acc)
      | _ -> invalid ()
    in
    whitespace ();
    if peek () = closing then (advance (); []) else items []
  in
  let rec value () =
    whitespace ();
    match peek () with
    | '[' -> advance (); `Array (sequence ']' value)
    | '{' ->
        advance ();
        let field () = let key = string () in expect ':'; key, value () in
        `Object (List.rev (sequence '}' field))
    | '"' -> `String (string ())
    | '-' | '0' .. '9' -> number ()
    | 'n' -> literal "null"; `Null
    | 't' -> literal "true"; `Other
    | 'f' -> literal "false"; `Other
    | _ -> invalid ()
  in
  let data = value () in
  whitespace ();
  if !position <> length then invalid ();
  let records =
    match data with
    | `Array records -> records
    | `Object fields ->
        (match List.assoc_opt "writers" fields with
         | Some (`Array records) -> records
         | None -> []
         | _ -> fail "'writers' must be an array")
    | _ -> fail "sprite writers file must contain an array or an object with 'writers'"
  in
  let lane fields =
    match List.assoc_opt "lane" fields with
    | None | Some `Null -> None
    | Some (`String name) ->
        Some (match String.uppercase_ascii name with
          | "X" -> Sprite_groups.X | "Y" -> Y | "XY" -> XY
          | _ -> fail "invalid sprite writer lane %S" name)
    | _ -> fail "invalid sprite writer lane"
  in
  let validate pc lane =
    if pc < 0 || pc land 1 <> 0 then fail "sprite writer PC must be an even non-negative integer: %d" pc;
    { pc; lane }
  in
  List.map (function
    | `Int pc -> validate pc None
    | `Object fields ->
        (match List.assoc_opt "pc" fields with
         | Some (`Int pc) -> validate pc (lane fields)
         | _ -> fail "sprite writer PC must be an even non-negative integer")
    | _ -> fail "invalid sprite writer record") records
