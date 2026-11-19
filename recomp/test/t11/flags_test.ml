open F3recomp

type kind = Logic | Add | Sub | Cmp

type operation = {
  kind : kind;
  src : int;
  dst : int;
  width : Sem.width;
  result : int;
}

let pending_flags { kind; src; dst; width; result } sr =
  let mask = Sem.mask width and sign = Sem.sign width in
  let bit value mask = if value then mask else 0 in
  let nz = bit (result land mask = 0) 4 lor bit (result land sign <> 0) 8 in
  match kind with
  | Logic -> sr land 16 lor nz
  | Add ->
      let carry = (result land mask) < (src land mask) in
      let overflow = (src lxor result) land (dst lxor result) land sign <> 0 in
      nz lor bit carry 17 lor bit overflow 2
  | Sub | Cmp ->
      let carry = (dst land mask) < (src land mask) in
      let overflow = (dst lxor src) land (dst lxor result) land sign <> 0 in
      let x = if kind = Sub then bit carry 16 else sr land 16 in
      x lor nz lor bit carry 1 lor bit overflow 2

let eager_flags { kind; src; dst; width; result } old =
  let modulus = Sem.mask width + 1 and sign = Sem.sign width in
  let signed value = if value >= sign then value - modulus else value in
  let carry, overflow = match kind with
    | Logic -> false, false
    | Add ->
        let sum = signed dst + signed src in
        dst + src >= modulus, sum < -sign || sum >= sign
    | Sub | Cmp ->
        let difference = signed dst - signed src in
        dst - src < 0, difference < -sign || difference >= sign
  in
  List.fold_left
    (fun flags (set, bit) -> if set then flags lor bit else flags)
    0 [carry, 1; overflow, 2; result = 0, 4; result >= sign, 8;
       (if kind = Add || kind = Sub then carry else old land 16 <> 0), 16]

let producer { kind; src; dst; width; result } =
  let arithmetic = { Sem.src = Const src; dst = Const dst; result = Const result; width } in
  match kind with
  | Logic -> Sem.Logic { result = Const result; width }
  | Add -> Add arithmetic
  | Sub -> Sub arithmetic
  | Cmp -> Cmp arithmetic

let rec eval : Sem.expr -> int = function
  | Const value -> value
  | Bin (And, a, b) -> eval a land eval b
  | Bin (Lt, a, b) -> Bool.to_int (eval a < eval b)
  | _ -> Alcotest.fail "unexpected specialised X expression"

let flag_bits = [Ccr.X, 16; N, 8; Z, 4; V, 2; C, 1]
let live_set bits =
  List.fold_left (fun live (flag, bit) ->
      if bits land bit <> 0 then Ccr.union live (Ccr.singleton flag) else live)
    Ccr.none flag_bits

let operations =
  List.concat_map (fun width ->
      let sign = Sem.sign width and mask = Sem.mask width in
      List.concat_map (fun kind ->
          let inputs = match kind with
            | Logic -> [0, 0; 0, 1; 0, sign; 0, mask]
            | Add -> [0, 0; 1, mask; 1, sign - 1; sign, sign; 1, 0]
            | Sub | Cmp -> [0, 0; 1, 0; 1, sign; mask, sign - 1; 0, 1]
          in
          List.map (fun (src, dst) ->
              let result = (match kind with
                  | Logic -> dst | Add -> dst + src | Sub | Cmp -> dst - src)
                  land mask in
              { kind; src; dst; width; result }) inputs)
        [Logic; Add; Sub; Cmp])
    [Sem.Byte; Word; Long]
  |> Array.of_list

type state = { sr : int; pending : int; eager : int; claim : int }

(* Pending payloads are interchangeable when their flush agrees for both SR.X
   values. Intern that observable state to keep the length-four search small. *)
let pending_key operation =
  pending_flags operation 0 lor (pending_flags operation 16 lsl 5)

let flush sr pending =
  if pending = -1 then sr
  else if sr land 16 = 0 then pending land 31 else pending lsr 5

let exhaustive () =
  let transitions =
    Array.map (fun op ->
        let ir = producer op in
        let key = pending_key op in
        let eager = Array.init 32 (eager_flags op) in
        let written = match op.kind with Add | Sub -> 31 | Logic | Cmp -> 15 in
        Array.init 32 (fun live ->
            let { Flags.x; pending } = Flags.specialise ir ~live:(live_set live) in
            let update_pending = match pending with
              | Keep -> Fun.id
              | Clear -> fun _ -> -1
              | Install value ->
                  if ir <> value then Alcotest.fail "installed producer payload changed";
                  fun _ -> key
            in
            let update_sr = match x with
              | None -> Fun.id
              | Some value ->
                  let x = eval value lsl 4 in
                  fun sr -> sr land 15 lor x
            in
            (live land (lnot written), fun state ->
              { sr = update_sr state.sr; pending = update_pending state.pending;
                eager = eager.(state.eager); claim = live })))
      operations
  in
  let seen = Hashtbl.create 4096 in
  let frontier = Queue.create () in
  let add queue state =
    if not (Hashtbl.mem seen state) then (Hashtbl.add seen state (); Queue.add state queue)
  in
  let entry_keys = -1 :: Array.to_list (Array.map pending_key operations) in
  for sr = 0 to 31 do
    List.iter (fun pending ->
      let eager = flush sr pending in
      if (eager lxor sr) land 16 = 0 then add frontier { sr; pending; eager; claim = 31 }
    ) entry_keys
  done;
  let entry_count = Queue.length frontier in
  let checks = ref 0 in
  let check depth next state =
    incr checks;
    let actual = flush state.sr state.pending in
    if (actual lxor state.eager) land state.claim <> 0 then
      Alcotest.failf "depth %d live=%02x eager=%02x materialised=%02x pending=%d"
        depth state.claim state.eager actual state.pending;
    if state.claim land 16 <> 0 && (actual lxor state.sr) land 16 <> 0 then
      Alcotest.failf "depth %d violated the live-X representation invariant" depth;
    add next state
  in
  let rec visit depth frontier =
    if depth <= 4 then (
      let next = Queue.create () in
      Queue.iter (fun state ->
          Array.iter (Array.iter (fun (needed, apply) ->
              if needed land state.claim = needed then check depth next (apply state))) transitions;
          (* Materialised partial and full writes, plus a no-op or a flag read.
             The comparison on each transition observes every flag in the claim. *)
          for claim = 0 to 31 do
            if claim land state.claim = claim then check depth next { state with claim };
            let retained = claim land (lnot 4) in
            if retained land state.claim = retained then
              List.iter (fun z ->
                  let set_z ccr = ccr land (lnot 4) lor z in
                  check depth next { sr = set_z (flush state.sr state.pending);
                                     pending = -1; eager = set_z state.eager; claim })
                [0; 4];
            for ccr = 0 to 31 do
              check depth next { sr = ccr; pending = -1; eager = ccr; claim }
            done
          done)
        frontier;
      visit (depth + 1) next)
  in
  visit 1 frontier;
  Printf.printf "%d concrete producers, %d entry states, %d reachable states, %d checked transitions\n"
    (Array.length operations) entry_count (Hashtbl.length seen) !checks

let () =
  Alcotest.run "lazy flags"
    ["specialise", [Alcotest.test_case "certified sequences through length four" `Quick exhaustive]]
