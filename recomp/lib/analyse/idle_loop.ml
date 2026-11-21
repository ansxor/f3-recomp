open Insn

type shape =
  | Spin
  | Poll_bcc of { address : Sem.expr; size : Insn.size; cond : Insn.cond }
  | Affine of { word : Sem.expr }

type t = { head : int; path : int list; shape : shape }

type rule = Head | Native_nodes | Cycle | Body_effects | Shape_parameters
type violation = { head : int; rule : rule }

let describe_violation { head; rule } =
  let reason = match rule with
    | Head -> "head agrees with map key and first path PC"
    | Native_nodes -> "every path PC is decoded and native"
    | Cycle -> "path follows native edges and balanced calls back to the head"
    | Body_effects -> "instructions and effects fit an idle shape"
    | Shape_parameters -> "shape and parameters agree with the instructions"
  in
  Printf.sprintf "%#x fails idle rule: %s" head reason

let to_tsv = function Spin -> "spin" | Poll_bcc _ -> "poll" | Affine _ -> "affine"

let next insn = Pc.physical (insn.pc + insn.length)
let max_path = 32

let invariant_address ~allow_stack : ea -> Sem.expr option = function
  | Absolute address -> Some (Const address)
  | Ind reg when allow_stack || reg <> 7 -> Some (Reg (A reg))
  | Disp { reg; disp } when allow_stack || reg <> 7 ->
      Some (Bin (Add, Reg (A reg), Const disp))
  | _ -> None

(* Keep only operations on the loaded data register. A single-register
   long multiply preserves the affine form modulo 2^32. *)
let affine_arithmetic register { op; _ } =
  match op with
  | Alu ((Add | Sub), Long, Immediate _, Register (D dest))
  | Mul_long { operand = Immediate _; low = dest; high = None; _ } ->
      dest = register
  | _ -> false

let affine_body = function
  | { op = Move (Long, source, Register (D register)); _ } :: rest ->
      let word = invariant_address ~allow_stack:false source in
      let rec after_arithmetic = function
        | insn :: rest when affine_arithmetic register insn -> after_arithmetic rest
        | rest -> rest
      in
      (match word, rest with
       | Some word, first :: tail when affine_arithmetic register first ->
           (match after_arithmetic tail with
            | { op = Move (Long, Register (D src), dest); _ } :: swaps
              when src = register && invariant_address ~allow_stack:false dest = Some word
                && List.for_all (fun insn -> insn.op = Swap register) swaps ->
                Some (Affine { word })
            | _ -> None)
       | _ -> None)
  | _ -> None

let classify = function
  | [{ op = Bra target; pc; _ }] when Pc.physical target = pc -> Some Spin
  | [{ op = Unary (Tst, size, ea); _ }; { op = Bcc (cond, _); _ }] ->
      Option.map (fun address -> Poll_bcc { address; size; cond })
        (invariant_address ~allow_stack:true ea)
  | { op = Jsr _; _ } :: _ as path ->
      List.filter (fun insn ->
        match insn.op with Jsr _ | Rts | Bra _ -> false | _ -> true) path
      |> affine_body
  | _ -> None

type step = Continue of int * int list | Close

(* A return edge is recovered from the matched call's return PC. All
   other transfers use decoded static flow. Only the final BRA can close
   an affine path. *)
let affine_step ~head stack insn =
  let flow = Flow.of_insn insn in
  match insn.op, flow.targets, flow.escape, stack with
  | Jsr _, [target], None, _ -> Some (Continue (target, next insn :: stack))
  | Rts, [], Some Return, return_pc :: stack -> Some (Continue (return_pc, stack))
  | Bra _, [target], None, [] when target = head -> Some Close
  | (Jsr _ | Rts | Bra _), _, _, _ -> None
  | _, [], None, _ when flow.falls_through -> Some (Continue (next insn, stack))
  | _ -> None

let affine_path program ~native head =
  let rec walk remaining path stack pc =
    if remaining = 0 || not (native pc) then None
    else
      Option.bind (Program.insn program pc) (fun insn ->
        let path = insn :: path in
        match affine_step ~head stack insn with
        | Some Close -> Some (List.rev path)
        | Some (Continue (pc, stack)) -> walk (remaining - 1) path stack pc
        | None -> None)
  in
  walk max_path [] [] head

let find (program : Program.t) ~native =
  Pc.Map.filter_map (fun head decoded ->
    match decoded with
    | Decode.Insn insn ->
        let path = match insn.op with
          | Bra target when Pc.physical target = head && native head -> Some [insn]
          | Unary (Tst, _, _) when native head && native (next insn) ->
              (match Program.insn program (next insn) with
               | Some ({ op = Bcc (_, target); _ } as branch) when Pc.physical target = head ->
                   Some [insn; branch]
               | _ -> None)
          | Jsr _ -> affine_path program ~native head
          | _ -> None
        in
        Option.bind path (fun path ->
          Option.map (fun shape -> { head; path = List.map (fun insn -> insn.pc) path; shape })
            (classify path))
    | _ -> None)
    program.decoded

let cfg_edge (cfg : Cfg.t) insn target =
  List.mem target (Pc.Map.find insn.pc cfg.nodes).succ

let check_cycle cfg head = function
  | [{ op = Bra target; _ } as branch] ->
      Pc.physical target = head && cfg_edge cfg branch head
  | [test; ({ op = Bcc (_, target); _ } as branch)] ->
      Flow.of_insn test = Flow.next && next test = branch.pc && Pc.physical target = head
      && cfg_edge cfg test branch.pc && cfg_edge cfg branch head
  | path ->
      let rec replay stack = function
        | [insn] -> affine_step ~head stack insn = Some Close && cfg_edge cfg insn head
        | insn :: (following :: _ as rest) ->
            (match affine_step ~head stack insn with
             | Some (Continue (target, stack)) when target = following.pc ->
                 (match insn.op with
                  | Rts -> List.mem Cfg.Return (Pc.Map.find insn.pc cfg.Cfg.nodes).boundaries
                  | _ -> cfg_edge cfg insn target)
                 && replay stack rest
             | _ -> false)
        | [] -> false
      in
      List.length path <= max_path && replay [] path

let check_all program cfg loops =
  let violations = Pc.Map.fold (fun head (loop : t) violations ->
    let failure =
      if loop.head <> head || List.nth_opt loop.path 0 <> Some head then Some Head
      else
        let instructions = List.filter_map (fun pc ->
          if Cfg.native cfg pc then Program.insn program pc else None) loop.path in
        if List.length loop.path <> List.length instructions then Some Native_nodes
        else if not (check_cycle cfg head instructions) then Some Cycle
        else match classify instructions with
          | None -> Some Body_effects
          | Some shape when shape <> loop.shape -> Some Shape_parameters
          | Some _ -> None
    in
    match failure with None -> violations | Some rule -> { head; rule } :: violations)
    loops [] in
  match violations with [] -> Ok () | _ -> Error (List.rev violations)
