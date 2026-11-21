open Insn

(* ---- addresses ---- *)

let hex_digits text = text <> "" && String.for_all (function
  | '0' .. '9' | 'a' .. 'f' | 'A' .. 'F' -> true | _ -> false) text

let parse_address text =
  let text = String.trim text in
  let digits =
    if String.starts_with ~prefix:"0x" text || String.starts_with ~prefix:"0X" text then
      String.sub text 2 (String.length text - 2)
    else if String.starts_with ~prefix:"$" text then String.sub text 1 (String.length text - 1)
    else text
  in
  let digits = String.concat "" (String.split_on_char '_' digits) in
  if not (hex_digits digits) || String.length digits > 8 then
    Error.fail "cannot read %S as a hexadecimal address" text;
  int_of_string ("0x" ^ digits)

let regions = [
  "rom", 0x000000, 0x200000;
  "ram", 0x400000, 0x440000;
  "palette", 0x440000, 0x448000;
  "io", 0x4a0000, 0x4a0020;
  "timer", 0x4c0000, 0x4c0004;
  "sprites", 0x600000, 0x610000;
  "pf0", 0x610000, 0x612000;
  "pf1", 0x612000, 0x614000;
  "pf2", 0x614000, 0x616000;
  "pf3", 0x616000, 0x618000;
  "pf2-alt", 0x618000, 0x61a000;
  "pf3-alt", 0x61a000, 0x61c000;
  "text", 0x61c000, 0x61e000;
  "charram", 0x61e000, 0x620000;
  "lines", 0x620000, 0x630000;
  "pivot", 0x630000, 0x640000;
  "control", 0x660000, 0x660040;
  "shared", 0xc00000, 0xc00800;
  "sound-reset", 0xc80000, 0xc80104;
]

(* Work RAM is 128 KiB mirrored across 0x400000..0x43ffff. *)
let canonical address =
  let address = address land 0xffffffff in
  if address >= 0x400000 && address < 0x440000 then 0x400000 + ((address - 0x400000) land 0x1ffff)
  else address

(* ---- raw reads ---- *)

type region = Rom_manifest.region

let region_names = [
  "rom", Rom_manifest.Rom; "sound", Sound; "sprites", Sprites; "sprites_hi", Sprites_hi;
  "tiles", Tiles; "tiles_hi", Tiles_hi; "samples", Samples; "eeprom", Eeprom;
]

let region_of_name name =
  match List.assoc_opt name region_names with
  | Some region -> region
  | None ->
      Error.fail "unknown region %S (expected %s)" name
        (String.concat ", " (List.map fst region_names))

let region_name region = fst (List.find (fun (_, r) -> r = region) region_names)

let hex_of_bytes bytes =
  let buffer = Buffer.create (2 * String.length bytes) in
  String.iter (fun c -> Printf.bprintf buffer "%02x" (Char.code c)) bytes;
  Buffer.contents buffer

let bytes_of_hex text =
  let digits = Buffer.create (String.length text) in
  String.iter (function
    | ' ' | '\t' | '\r' | '\n' -> ()
    | ('0' .. '9' | 'a' .. 'f' | 'A' .. 'F') as digit -> Buffer.add_char digits digit
    | _ -> Error.fail "invalid hex byte string")
    text;
  let digits = Buffer.contents digits in
  if String.length digits mod 2 <> 0 then Error.fail "hex byte strings need whole bytes";
  String.init (String.length digits / 2) (fun i ->
    Char.chr (int_of_string ("0x" ^ String.sub digits (2 * i) 2)))

let read_bytes (image : Image.t) ~addr ~len =
  if len < 0 || addr < image.base || addr - image.base + len > Image.length image then
    Error.fail "read of %d bytes at %#x is outside the region (%#x..%#x)" len addr
      image.base (image.base + Image.length image);
  String.sub image.bytes (addr - image.base) len

let rom_read ~region image ~addr ~len =
  Json.Object [
    "region", String region;
    "addr", Int addr;
    "length", Int len;
    "size", Int (Image.length image);
    "bytes", String (hex_of_bytes (read_bytes image ~addr ~len));
  ]

(* ---- listings ---- *)

type line = { pc : Pc.t; length : int; bytes : string; decoded : Decode.decoded }

let decoded_length = function
  | Decode.Insn insn -> insn.length
  | Illegal illegal -> illegal.length

let disassemble (image : Image.t) ~pc ?count ?stop () =
  let limit = Option.value count ~default:max_int in
  let end_of_image = image.base + Image.length image in
  let rec loop pc taken acc =
    let finished = taken >= limit || pc >= end_of_image
      || (match stop with Some stop -> pc >= stop | None -> false) in
    if finished then List.rev acc
    else
      let decoded = Decode.at image pc in
      let length = max 2 (decoded_length decoded) in
      let length = min length (end_of_image - pc) in
      let bytes = String.sub image.bytes (pc - image.base) length in
      loop (pc + length) (taken + 1) ({ pc; length; bytes; decoded } :: acc)
  in
  if pc < image.base || pc >= end_of_image then
    Error.fail "pc %#x is outside the image (%#x..%#x)" pc image.base end_of_image;
  loop pc 0 []

let reason_name : Decode.reason -> string = function
  | Unassigned -> "unassigned"
  | Illegal_opcode -> "illegal-opcode"
  | A_line -> "a-line"
  | Truncated -> "truncated"
  | Mame_default Breakpoint -> "breakpoint"
  | Mame_default F_line -> "f-line"
  | Mame_default (Movec_control code) -> Printf.sprintf "movec-control-%#x" code

let word_directive bytes =
  let words = List.init (String.length bytes / 2) (fun i ->
    Printf.sprintf "$%04x" (String.get_uint16_be bytes (2 * i))) in
  match words with
  | [] -> "dc.b " ^ String.concat "," (List.init (String.length bytes) (fun i ->
      Printf.sprintf "$%02x" (Char.code bytes.[i])))
  | _ -> "dc.w " ^ String.concat "," words

let line_text line =
  match line.decoded with
  | Decode.Insn insn -> Format.to_string insn
  | Illegal _ -> word_directive line.bytes

let spaced_hex bytes =
  String.concat " " (List.init (String.length bytes / 2) (fun i ->
    Printf.sprintf "%04x" (String.get_uint16_be bytes (2 * i))))

let line_to_text line =
  let bytes = spaced_hex line.bytes in
  let note = match line.decoded with
    | Illegal { reason; vector; _ } ->
        Printf.sprintf "  ; %s (vector %d)" (reason_name reason) vector
    | Insn _ -> ""
  in
  Printf.sprintf "%06x:  %-24s %s%s" line.pc bytes (line_text line) note

let escape_name = function
  | Flow.Return -> "return"
  | Exception_return -> "exception-return"
  | Computed _ -> "computed"
  | Module_call _ -> "module-call"
  | Raises -> "raises"
  | Halt -> "halt"

let ints values = Json.Array (List.map (fun value -> Json.Int value) values)

let reference_json (reference : Format.reference) =
  let option name = function Some value -> [name, Json.Int value] | None -> [] in
  Json.Object (
    ["role", Json.String (Format.role_name reference.role)]
    @ option "target" reference.target @ option "a5_disp" reference.a5_disp
    @ ["size", Json.Int reference.size; "indexed", Json.Bool reference.indexed])

let line_to_json ?a5 line =
  let base = [
    "pc", Json.Int line.pc;
    "length", Json.Int line.length;
    "bytes", Json.String (hex_of_bytes line.bytes);
  ] in
  match line.decoded with
  | Decode.Illegal { reason; vector; _ } ->
      Json.Object (base @ [
        "ok", Json.Bool false;
        "reason", String (reason_name reason);
        "vector", Int vector;
        "text", String (line_text line);
      ])
  | Insn insn ->
      let flow = Flow.of_insn insn in
      Json.Object (base @ [
        "ok", Json.Bool true;
        "op_name", String (Insn.op_name insn.op);
        "mnemonic", String (Format.mnemonic insn.op);
        "operands", String (Format.operands insn.op);
        "text", String (Format.to_string insn);
        "targets", ints (List.sort_uniq Int.compare flow.targets);
        "falls_through", Bool flow.falls_through;
        "escape", (match flow.escape with Some escape -> String (escape_name escape) | None -> Null);
        "refs", Array (List.map reference_json (Format.references ?a5 insn));
      ])

let disasm_json ?a5 lines = Json.Array (List.map (line_to_json ?a5) lines)

(* ---- vectors ---- *)

let vector_name slot =
  match slot with
  | 1 -> "reset" | 2 -> "bus-error" | 3 -> "address-error" | 4 -> "illegal"
  | 5 -> "zero-divide" | 6 -> "chk" | 7 -> "trapv" | 8 -> "privilege" | 9 -> "trace"
  | 10 -> "line-a" | 11 -> "line-f" | 24 -> "spurious"
  | n when n >= 25 && n <= 31 -> Printf.sprintf "irq%d" (n - 24)
  | n when n >= 32 && n <= 47 -> Printf.sprintf "trap%d" (n - 32)
  | n -> Printf.sprintf "v%d" n

let vector_slot name =
  let name = String.lowercase_ascii (String.trim name) in
  let name = if String.starts_with ~prefix:"vector:" name then String.sub name 7 (String.length name - 7) else name in
  List.find_opt (fun slot -> vector_name slot = name) (List.init 255 (fun i -> i + 1))

(* ---- routines ---- *)

type kind = Vector | Ram | Config | Call | Indirect

let kind_name = function
  | Vector -> "vector" | Ram -> "ram" | Config -> "config" | Call -> "call" | Indirect -> "indirect"

let kind_rank = function Vector -> 0 | Ram -> 1 | Config -> 2 | Call -> 3 | Indirect -> 4

type edge_kind = Call_edge | Tail | Table_call | Table_jump | Fall

let edge_kind_name = function
  | Call_edge -> "call" | Tail -> "tail" | Table_call -> "table-call"
  | Table_jump -> "table-jump" | Fall -> "fall"

type edge = { src : Pc.t; dst : Pc.t; site : Pc.t; kind : edge_kind }

type routine = {
  entry : Pc.t;
  kinds : kind list;
  vector_names : string list;
  pcs : Pc.t list;
}

type t = {
  program : Program.t;
  profile : Profile.t option;
  crc : int32;
  a5 : int option;
  a5_source : string option;
  a5_candidates : (int * int) list;
  live : Pc.Set.t;
  routines : routine Pc.Map.t;
  owners : Pc.t list Pc.Map.t;
  callers : edge list Pc.Map.t;
  callees : edge list Pc.Map.t;
  unresolved : Pc.t list;
}

(* Calls, traps and module calls return to the following instruction. *)
let continues (insn : Insn.t) (flow : Flow.t) =
  flow.falls_through
  || (match insn.op with Bsr _ | Jsr _ | Callm _ | Trap _ | Trapcc _ -> true | _ -> false)

let is_call = function Bsr _ | Jsr _ -> true | _ -> false

let table_candidates (program : Program.t) pc =
  match Pc.Map.find_opt pc program.tables with
  | Some table -> table.candidates
  | None -> []

let successors program (insn : Insn.t) =
  let flow = Flow.of_insn insn in
  let next = if continues insn flow then [Pc.physical (insn.pc + insn.length)] else [] in
  List.filter (fun pc -> Option.is_some (Program.insn program pc))
    (flow.targets @ table_candidates program insn.pc @ next)

let reachable program seeds =
  let rec visit live = function
    | [] -> live
    | pc :: rest when Pc.Set.mem pc live -> visit live rest
    | pc :: rest ->
        (match Program.insn program pc with
         | None -> visit live rest
         | Some insn -> visit (Pc.Set.add pc live) (successors program insn @ rest))
  in
  visit Pc.Set.empty (List.of_seq (Pc.Set.to_seq seeds))

let detect_a5 program live =
  let counts = Hashtbl.create 4 in
  Pc.Set.iter (fun pc ->
    match Program.insn program pc with
    | Some { op = Lea ((Absolute value | Immediate value), 5)
                | Movea (Long, (Immediate value | Absolute value), 5); _ } ->
        let value = value land 0xffffffff in
        Hashtbl.replace counts value (1 + Option.value (Hashtbl.find_opt counts value) ~default:0)
    | _ -> ())
    live;
  Hashtbl.to_seq counts |> List.of_seq
  |> List.sort (fun (a, ca) (b, cb) -> match Int.compare cb ca with 0 -> Int.compare a b | n -> n)

let vector_entries (program : Program.t) =
  let rom = program.game.rom in
  List.filter_map (fun slot ->
    let pc = Pc.physical (Image.u32 rom (4 * slot)) in
    if Pc.Set.mem pc program.seeds.vectors && Option.is_some (Program.insn program pc)
    then Some (pc, slot) else None)
    (List.init 255 (fun i -> i + 1))

let build_routines program ~hits =
  let seeds : Program.seeds = program.Program.seeds in
  let vectors = vector_entries program in
  let base_seeds = Pc.Set.(union seeds.vectors (union seeds.configured seeds.ram_entries)) in
  let base_live = reachable program base_seeds in
  let hit_seeds = Pc.Set.filter (fun pc -> Option.is_some (Program.insn program pc)) hits in
  let live = reachable program (Pc.Set.union base_seeds hit_seeds) in
  let kinds = Hashtbl.create 1024 and names = Hashtbl.create 64 in
  let add pc kind =
    if Option.is_some (Program.insn program pc) then
      let known = Option.value (Hashtbl.find_opt kinds pc) ~default:[] in
      if not (List.mem kind known) then Hashtbl.replace kinds pc (kind :: known)
  in
  List.iter (fun (pc, slot) ->
    add pc Vector;
    Hashtbl.replace names pc (vector_name slot :: Option.value (Hashtbl.find_opt names pc) ~default:[]))
    vectors;
  Pc.Set.iter (fun pc -> add pc Config) seeds.configured;
  Pc.Set.iter (fun pc -> add pc Ram) seeds.ram_entries;
  let called = ref Pc.Set.empty in
  Pc.Set.iter (fun pc ->
    match Program.insn program pc with
    | Some ({ op = Bsr _ | Jsr _; _ } as insn) ->
        let flow = Flow.of_insn insn in
        List.iter (fun target -> add target Call) flow.targets;
        List.iter (fun target -> add target Indirect) (table_candidates program pc)
    | _ -> ())
    live;
  Pc.Set.iter (fun pc ->
    Pc.Set.iter (fun next -> called := Pc.Set.add next !called)
      (match Program.insn program pc with
       | Some insn -> Pc.Set.of_list (successors program insn)
       | None -> Pc.Set.empty))
    live;
  Pc.Set.iter (fun pc ->
    if not (Pc.Set.mem pc base_live) && not (Pc.Set.mem pc !called) then add pc Indirect)
    hit_seeds;
  let entries = Hashtbl.fold (fun pc _ set -> Pc.Set.add pc set) kinds Pc.Set.empty in
  let claim entry =
    let seen = Hashtbl.create 64 in
    let rec visit = function
      | [] -> ()
      | pc :: rest when Hashtbl.mem seen pc -> visit rest
      | pc :: rest ->
          (match Program.insn program pc with
           | None -> visit rest
           | Some insn ->
               Hashtbl.replace seen pc insn;
               let flow = Flow.of_insn insn in
               let local = (if is_call insn.op then [] else flow.targets)
                 @ (match insn.op with Jmp _ -> table_candidates program pc | _ -> [])
                 @ (if continues insn flow then [Pc.physical (pc + insn.length)] else []) in
               let local = List.filter (fun target ->
                 (target = entry || not (Pc.Set.mem target entries))
                 && Option.is_some (Program.insn program target)) local in
               visit (local @ rest))
    in
    visit [entry];
    Hashtbl.fold (fun pc insn acc -> (pc, insn) :: acc) seen []
    |> List.sort (fun (a, _) (b, _) -> Int.compare a b)
  in
  let routines, owners, edges =
    Pc.Set.fold (fun entry (routines, owners, edges) ->
      let body = claim entry in
      let pcs = List.map fst body in
      let kinds = Option.value (Hashtbl.find_opt kinds entry) ~default:[]
        |> List.sort (fun a b -> Int.compare (kind_rank a) (kind_rank b)) in
      let routine = { entry; kinds;
                      vector_names = List.rev (Option.value (Hashtbl.find_opt names entry) ~default:[]);
                      pcs } in
      let owners = List.fold_left (fun owners pc ->
        Pc.Map.update pc (fun known -> Some (entry :: Option.value known ~default:[])) owners)
        owners pcs in
      let edge kind site dst = { src = entry; dst; site; kind } in
      let edges = List.fold_left (fun edges (pc, (insn : Insn.t)) ->
        let flow = Flow.of_insn insn in
        let to_entries kind targets =
          List.filter_map (fun target ->
            if Pc.Set.mem target entries && (kind = Call_edge || kind = Table_call || target <> entry)
            then Some (edge kind pc target) else None) targets
        in
        let table = table_candidates program pc in
        let transfers =
          match insn.op with
          | Bsr _ | Jsr _ -> to_entries Call_edge flow.targets @ to_entries Table_call table
          | Jmp _ -> to_entries Tail flow.targets @ to_entries Table_jump table
          | _ -> to_entries Tail flow.targets
        in
        let falls =
          if continues insn flow && not (is_call insn.op) then
            to_entries Fall [Pc.physical (pc + insn.length)]
          else []
        in
        transfers @ falls @ edges)
        edges body in
      Pc.Map.add entry routine routines, owners, edges)
      entries (Pc.Map.empty, Pc.Map.empty, [])
  in
  let edges = List.sort_uniq compare edges in
  let group key = List.fold_left (fun map edge ->
    Pc.Map.update (key edge) (fun known -> Some (edge :: Option.value known ~default:[])) map)
    Pc.Map.empty (List.rev edges) in
  let unresolved =
    Pc.Set.fold (fun pc acc ->
      match Program.insn program pc with
      | Some insn ->
          (match (Flow.of_insn insn).escape with
           | Some (Computed _) when not (Pc.Map.mem pc program.tables) -> pc :: acc
           | _ -> acc)
      | None -> acc)
      live []
    |> List.rev
  in
  live, routines, owners, group (fun edge -> edge.dst), group (fun edge -> edge.src), unresolved

let make_of_program ?profile ?a5 (program : Program.t) =
  let crc = Image.crc32 program.game.rom in
  let hits =
    match profile with
    | None -> Pc.Set.empty
    | Some profile ->
        Profile.hot profile Main program.game.rom ~exclusions:program.game.exclusions
  in
  let live, routines, owners, callers, callees, unresolved = build_routines program ~hits in
  let candidates = detect_a5 program live in
  let a5, a5_source =
    match a5, candidates with
    | Some value, _ -> Some value, Some "explicit"
    | None, (value, _) :: _ -> Some value, Some "detected"
    | None, [] -> None, None
  in
  { program; profile; crc; a5; a5_source; a5_candidates = candidates; live; routines; owners;
    callers; callees; unresolved }

let make ?profile ?a5 game = make_of_program ?profile ?a5 (Game.discover game)

(* ---- lookup ---- *)

let hits t pc =
  match t.profile with
  | None -> None
  | Some profile -> Profile.count profile Hit Main ~crc:t.crc ~pc

let routine_name entry = Printf.sprintf "sub_%06x" entry

(* The routine claiming a PC, else the nearest entry below it. *)
let owner t pc =
  match Pc.Map.find_opt pc t.owners with
  | Some entries -> Some (List.fold_left (fun best entry ->
      if entry <= pc && (best < 0 || entry > best) then entry else best) (-1) entries
      |> fun best -> if best < 0 then List.hd entries else best)
  | None -> Option.map fst (Pc.Map.find_last_opt (fun entry -> entry <= pc) t.routines)

let option_int = function Some value -> Json.Int value | None -> Json.Null
let option_hits = function Some count -> Json.Int64 (if count < 0L then Int64.max_int else count) | None -> Json.Null

let ranges t routine =
  let rec merge acc = function
    | [] -> List.rev acc
    | pc :: rest ->
        let length = match Program.insn t.program pc with Some insn -> insn.length | None -> 2 in
        (match acc with
         | (start, stop) :: before when stop = pc -> merge ((start, pc + length) :: before) rest
         | _ -> merge ((pc, pc + length) :: acc) rest)
  in
  merge [] routine.pcs

let routine_json t routine =
  let ranges = ranges t routine in
  let bytes = List.fold_left (fun sum (start, stop) -> sum + stop - start) 0 ranges in
  let edges map = Pc.Map.find_opt routine.entry map |> Option.value ~default:[] in
  let unique f edges = List.sort_uniq Int.compare (List.map f edges) in
  let total_hits = List.fold_left (fun sum pc ->
    match hits t pc with Some count -> Int64.add sum count | None -> sum) 0L routine.pcs in
  let region = if routine.entry >= 0x400000 then "ram" else "rom" in
  Json.Object ([
    "entry", Json.Int routine.entry;
    "name", String (routine_name routine.entry);
    "kind", String (kind_name (List.hd routine.kinds));
    "kinds", Array (List.map (fun kind -> Json.String (kind_name kind)) routine.kinds);
    "vectors", Array (List.map (fun name -> Json.String name) routine.vector_names);
    "region", String region;
    "instructions", Int (List.length routine.pcs);
    "bytes", Int bytes;
    "ranges", Array (List.map (fun (start, stop) -> Json.Array [Int start; Int stop]) ranges);
    "callers", ints (unique (fun edge -> edge.src) (edges t.callers));
    "callees", ints (unique (fun edge -> edge.dst) (edges t.callees));
  ] @ (match t.profile with
       | None -> []
       | Some _ -> ["entry_hits", option_hits (hits t routine.entry);
                    "hits", option_hits (Some total_hits)]))

(* ---- inspect ---- *)

let ram_code_json t =
  let program = t.program in
  let specs = program.game.config.ram_code in
  Json.Array (List.map (fun (ram : Ram_code.routine) ->
    let spec = List.find_opt (fun (spec : Config.ram_code) -> spec.destination = ram.destination) specs in
    let instructions = Pc.Map.fold (fun _ decoded count ->
      match decoded with Decode.Insn _ -> count + 1 | Illegal _ -> count) ram.decoded 0 in
    Json.Object ([
      "destination", Json.Int ram.destination;
      "entry", Int ram.entry;
      "length", Int (Image.length ram.payload);
      "instructions", Int instructions;
      "illegal_words", Int (Pc.Map.cardinal ram.decoded - instructions);
      "exits", ints (Pc.Set.elements (Ram_code.exits [ram]));
      "callers", ints (Option.value (Option.map (fun (e : edge list) -> List.sort_uniq Int.compare
        (List.map (fun edge -> edge.src) e)) (Pc.Map.find_opt ram.entry t.callers)) ~default:[]);
    ] @ (match spec with
         | Some spec -> ["source", Json.Int spec.source;
                         "transform", Json.String (match spec.transform with Negate_be32 -> "negate_be32")]
         | None -> [])))
    (Code_space.ram program.space))

let site_json t pc =
  let text = match Program.insn t.program pc with Some insn -> Format.to_string insn | None -> "?" in
  Json.Object (["pc", Json.Int pc; "routine", option_int (owner t pc); "text", Json.String text]
               @ (match hits t pc with Some h -> ["hits", option_hits (Some h)] | None -> []))

let vectors_json (program : Program.t) =
  let rom = program.game.rom in
  Json.Array (List.init 63 (fun i ->
    let slot = i + 1 in
    let raw = Image.u32 rom (4 * slot) in
    let pc = Pc.physical raw in
    Json.Object [
      "number", Json.Int slot;
      "name", String (vector_name slot);
      "raw", Int raw;
      "target", Int pc;
      "valid", Bool (pc >= 0x400 && pc land 1 = 0 && Option.is_some (Program.insn program pc));
    ]))

let a5_json t =
  Json.Object [
    "value", option_int t.a5;
    "source", (match t.a5_source with Some source -> Json.String source | None -> Null);
    "candidates", Array (List.map (fun (value, count) ->
      Json.Object ["value", Json.Int value; "count", Json.Int count]) t.a5_candidates);
  ]

let inspect_json ?(routines = true) t =
  let program = t.program in
  let game = program.game in
  let all = Pc.Map.bindings t.routines |> List.map snd in
  let by_kind = List.map (fun kind ->
    kind_name kind, Json.Int (List.length (List.filter (fun r -> List.hd r.kinds = kind) all)))
    [Vector; Ram; Config; Call; Indirect] in
  let summary = Json.Object [
    "by_kind", Json.Object by_kind;
    "live_instructions", Json.Int (Pc.Set.cardinal t.live);
    "decoded_instructions", Json.Int (Pc.Map.fold (fun _ decoded n ->
      match decoded with Decode.Insn _ -> n + 1 | Illegal _ -> n) program.decoded 0);
    "jump_tables", Json.Int (Pc.Map.cardinal program.tables);
  ] in
  Json.Object ([
    "game", Json.String game.config.game.id;
    "rom_size", Int (Image.length game.rom);
    "rom_crc32", String (Printf.sprintf "%08lx" t.crc);
    "rom_sha256", String (Digestif.SHA256.(to_hex (digest_string game.rom.bytes)));
    "a5", (match t.a5 with Some value -> Json.Int value | None -> Null);
    "a5_info", a5_json t;
    "vectors", vectors_json program;
    "routine_count", Int (Pc.Map.cardinal t.routines);
    "summary", summary;
  ] @ (if routines then ["routines", Json.Array (List.map (routine_json t) all)] else [])
    @ [
    "ram_code", ram_code_json t;
    "unresolved_indirect_sites", Array (List.map (site_json t) t.unresolved);
    "profile", (match t.profile with
      | None -> Null
      | Some profile ->
          let stats = Profile.stats profile in
          Json.Object ["identities", Int stats.identities; "executed", Int stats.executed;
                       "cold_misses", Int stats.cold_misses]);
  ])

(* ---- xref ---- *)

type matcher = {
  describe : Json.t;
  matches : Format.reference -> bool;
}

let fold_range lo hi =
  if lo >= 0x400000 && hi < 0x440000 then canonical lo, canonical hi else lo, hi

let signed_offset text =
  let text = String.trim text in
  let negative, rest =
    if String.length text > 0 && (text.[0] = '-' || text.[0] = '+') then
      text.[0] = '-', String.sub text 1 (String.length text - 1)
    else false, text
  in
  let value = parse_address rest in
  if negative then -value else value

let overlaps ~lo ~hi (reference : Format.reference) =
  match reference.target with
  | None -> false
  | Some target ->
      let size = max 1 reference.size in
      let first = canonical target in
      let last = first + size - 1 in
      let lo, hi = fold_range lo hi in
      last >= lo && first <= hi

let parse_target t text =
  let lowered = String.lowercase_ascii (String.trim text) in
  let compact = String.concat "" (String.split_on_char ' ' lowered) in
  let range name lo hi =
    { describe = Json.Object ["kind", Json.String name; "start", Json.Int lo; "end", Json.Int hi];
      matches = overlaps ~lo ~hi }
  in
  match List.find_opt (fun (name, _, _) -> name = lowered) regions with
  | Some (name, start, stop) -> range ("region:" ^ name) start (stop - 1)
  | None when String.length compact > 2 && String.starts_with ~prefix:"a5" compact
              && (compact.[2] = '+' || compact.[2] = '-') ->
      let offset = signed_offset (String.sub compact 2 (String.length compact - 2)) in
      let direct (reference : Format.reference) =
        match reference.a5_disp with
        | Some disp -> disp <= offset && offset < disp + max 1 reference.size
        | None -> false
      in
      (match t.a5 with
       | Some a5 ->
           let address = (a5 + offset) land 0xffffffff in
           { describe = Json.Object ["kind", Json.String "a5"; "offset", Json.Int offset;
                                     "address", Json.Int address];
             matches = (fun reference -> direct reference || overlaps ~lo:address ~hi:address reference) }
       | None ->
           { describe = Json.Object ["kind", Json.String "a5"; "offset", Json.Int offset;
                                     "address", Json.Null];
             matches = direct })
  | None ->
      (match String.index_opt compact '-' with
       | Some split when split > 0 && not (String.starts_with ~prefix:"a5" compact) ->
           let lo = parse_address (String.sub compact 0 split) in
           let hi = parse_address (String.sub compact (split + 1) (String.length compact - split - 1)) in
           if hi < lo then Error.fail "range end %#x is below its start %#x" hi lo;
           range "range" lo hi
       | _ ->
           let address = parse_address compact in
           range "address" address address)

let role_filter = function
  | None | Some "all" -> fun _ -> true
  | Some "read" -> fun role -> role = Format.Read
  | Some "write" -> fun role -> role = Format.Write || role = Format.Rmw
  | Some "rmw" -> fun role -> role = Format.Rmw
  | Some "call" -> fun role -> role = Format.Call
  | Some "branch" -> fun role -> role = Format.Branch
  | Some "lea" -> fun role -> role = Format.Lea
  | Some other -> Error.fail "unknown --role %S (expected all, read, write, rmw, call, branch, lea)" other

let xref_json ?role ?(limit = 500) t ~target =
  let matcher = parse_target t target in
  let wanted = role_filter role in
  let rows = Pc.Set.fold (fun pc acc ->
    match Program.insn t.program pc with
    | None -> acc
    | Some insn ->
        List.fold_left (fun acc (reference : Format.reference) ->
          if wanted reference.role && matcher.matches reference then (insn, reference) :: acc else acc)
          acc (Format.references ?a5:t.a5 insn))
    t.live [] |> List.rev in
  let total = List.length rows in
  let shown = List.filteri (fun i _ -> i < limit) rows in
  let row ((insn : Insn.t), (reference : Format.reference)) =
    Json.Object ([
      "pc", Json.Int insn.pc;
      "routine", option_int (owner t insn.pc);
      "op_name", String (Insn.op_name insn.op);
      "text", String (Format.to_string insn);
      "role", String (Format.role_name reference.role);
      "target", option_int reference.target;
      "a5_disp", option_int reference.a5_disp;
      "size", Int reference.size;
      "indexed", Bool reference.indexed;
    ] @ (match hits t insn.pc with Some count -> ["hits", option_hits (Some count)] | None -> []))
  in
  Json.Object [
    "target", String target;
    "match", matcher.describe;
    "a5", option_int t.a5;
    "total", Int total;
    "truncated", Bool (total > limit);
    "refs", Array (List.map row shown);
  ]

(* ---- flow ---- *)

let edge_json t ~towards (edge : edge) =
  let routine = if towards then edge.src else edge.dst in
  Json.Object (["routine", Json.Int routine; "site", Json.Int edge.site;
                "kind", Json.String (edge_kind_name edge.kind)]
               @ (match hits t edge.site with Some h -> ["hits", option_hits (Some h)] | None -> []))

let flow_json ?(depth = 2) t ~target =
  if depth < 0 then Error.fail "--depth must not be negative";
  let target_pc () =
    match vector_slot target with
    | Some slot -> Pc.physical (Image.u32 t.program.game.rom (4 * slot))
    | None -> parse_address target
  in
  if String.lowercase_ascii (String.trim target) = "?dispatch" then
    let indirect = Pc.Map.fold (fun entry routine acc ->
      if List.hd routine.kinds = Indirect then entry :: acc else acc) t.routines [] |> List.rev in
    Json.Object [
      "target", Json.String target;
      "dispatch", Bool true;
      "callers", Array (List.map (fun pc ->
        Json.Object ["routine", option_int (owner t pc); "site", Json.Int pc;
                     "kind", Json.String "dispatch"]) t.unresolved);
      "callees", Array (List.map (fun entry ->
        Json.Object ["routine", Json.Int entry; "site", Json.Null; "kind", Json.String "dispatch";
                     "children", Json.Array []]) indirect);
      "unresolved", Array (List.map (site_json t) t.unresolved);
    ]
  else
    let pc = target_pc () in
    let entry =
      match Pc.Map.mem pc t.routines, owner t pc with
      | true, _ -> pc
      | false, Some entry when Pc.Map.mem pc t.owners -> entry
      | _ -> Error.fail "%#x is not inside any discovered routine" pc
    in
    let routine = Pc.Map.find entry t.routines in
    let edges map key = Pc.Map.find_opt key map |> Option.value ~default:[] in
    let seen = Hashtbl.create 64 in
    let shown = ref [entry] in
    let rec tree remaining entry =
      Hashtbl.replace seen entry ();
      List.map (fun (edge : edge) ->
        let repeated = Hashtbl.mem seen edge.dst in
        if not repeated then shown := edge.dst :: !shown;
        let children =
          if repeated || remaining <= 1 then Json.Array []
          else Json.Array (tree (remaining - 1) edge.dst)
        in
        match edge_json t ~towards:false edge with
        | Json.Object fields ->
            Json.Object (fields @ ["repeated", Json.Bool repeated; "children", children])
        | json -> json)
        (edges t.callees entry)
    in
    let callees = if depth = 0 then [] else tree depth entry in
    let visited_unresolved = ref (List.concat_map (fun shown ->
      List.filter (fun pc -> List.mem pc t.unresolved) (Pc.Map.find shown t.routines).pcs) !shown) in
    let unresolved = List.sort_uniq Int.compare !visited_unresolved in
    Json.Object [
      "target", Json.String target;
      "pc", Json.Int pc;
      "entry", Int entry;
      "name", String (routine_name entry);
      "exact", Bool (pc = entry);
      "kind", String (kind_name (List.hd routine.kinds));
      "vectors", Array (List.map (fun name -> Json.String name) routine.vector_names);
      "callers", Array (List.map (edge_json t ~towards:true) (edges t.callers entry));
      "callees", Array callees;
      "unresolved", Array (List.map (site_json t) unresolved);
    ]
