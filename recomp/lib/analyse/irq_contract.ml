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

let analyse ~(rom : Image.t) ~(instruction : int -> Insn.t option) (cfg : Cfg.t) handler =
  let exception Unresolved of int * string in
  let unknown at why = raise (Unresolved (at, why)) in
  let rec inspect pc in_call : Sem.stmt -> unit = function
    | Load (_, width, Const addr)
      when Image.contains_range rom (Pc.physical addr) ~length:(Sem.bytes width) -> ()
    | Load _ | Helper (Bitfield { memory = Const 1; _ }) ->
        unknown pc "memory read may observe stacked CCR"
    | If (_, yes, no) ->
        List.iter (inspect pc in_call) yes;
        List.iter (inspect pc in_call) no
    | Store _ | Store_long_low_first _ | Set_reg ((A 7 | Msp | Isp), _, _)
      when in_call -> unknown pc "cannot verify saved return address"
    | Runtime _ -> unknown pc "runtime operation may change interrupt context"
    | Helper (Bitfield { memory = Const 0; _ }) -> ()
    | Helper (Bitfield _) -> unknown pc "bitfield may access stacked CCR"
    | Raise _ | Repeat _ | Stop _ -> unknown pc "control leaves the handler graph"
    | _ -> ()
  in
  let work = Queue.create () and seen = Hashtbl.create 32 in
  let enqueue pc stack inherited =
    let state = pc, stack, inherited in
    if not (Hashtbl.mem seen state) then begin
      Hashtbl.add seen state ();
      Queue.add state work
    end in
  let flags = ref Ccr.none in
  enqueue handler [] Ccr.all;
  try
    while not (Queue.is_empty work) do
      let pc, stack, inherited = Queue.take work in
      let insn, node = match instruction pc, Pc.Map.find_opt pc cfg.nodes with
        | Some insn, Some node when Cfg.native cfg pc -> insn, node
        | _ -> unknown pc "handler reaches code outside the native graph"
      in
      let next = Pc.physical (pc + insn.length) in
      match insn.op with
      | Rte when stack = [] -> ()
      | Rte -> unknown pc "exception return with a pending direct call"
      | Rts | Rtd 0 ->
          (match stack with
           | (_, resume) :: rest -> enqueue resume rest inherited
           | [] -> unknown pc "return without a direct call")
      | Bsr _ | Jsr _ ->
          (match node.succ, node.boundaries with
           | [target], [] ->
               if target = handler || List.exists (fun (entry, _) -> entry = target) stack
               then unknown pc "recursive direct call";
               enqueue target ((target, next) :: stack) inherited
           | _ -> unknown pc "cannot resolve call target")
      | _ ->
          (match node.boundaries with
           | [] -> ()
           | _ -> unknown pc "control leaves the handler graph");
          (match Semantics.of_insn insn with
           | Body stmts -> List.iter (inspect pc (stack <> [])) stmts
           | Unsupported why -> unknown pc why);
          flags := Ccr.union !flags (Ccr.inter inherited node.effects.reads);
          let inherited = Ccr.diff inherited node.effects.writes in
          List.iter (fun target -> enqueue target stack inherited) node.succ
    done;
    Proven !flags
  with Unresolved (at, why) -> Unknown { handler; at; why }

let observed (program : Program.t) cfg =
  let rom = program.game.rom in
  List.map (analyse ~rom ~instruction:(Program.insn program) cfg) (handlers rom)
