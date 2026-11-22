let letters mask = if mask = Ccr.none then "" else Ccr.to_string mask

(* ---- coverage ---- *)

let coverage_counts (game : Game_image.t) =
  Coverage.unsupported ~rom:game.rom ~exclusions:game.exclusions

let coverage_text counts =
  let buffer = Buffer.create 256 in
  List.iter (fun (name, count) -> Printf.bprintf buffer "%s\t%d\n" name count) counts;
  Printf.bprintf buffer "total\t%d\n" (List.fold_left (fun sum (_, n) -> sum + n) 0 counts);
  Buffer.contents buffer

let coverage_json ~game counts =
  Json.Object [
    "game", String game;
    "unsupported", Object (List.map (fun (name, count) -> name, Json.Int count) counts);
    "total", Int (List.fold_left (fun sum (_, n) -> sum + n) 0 counts);
  ]

(* ---- check-irq ---- *)

type irq = {
  game : string;
  interrupt_observes : Ccr.t;
  handlers : (int * Irq_contract.verdict) list;
  undeclared : int -> bool;
}

let check_irq (program : Program.t) =
  let cfg = Program.select program ~profile:None |> Cfg.of_selection in
  let verdicts = Irq_contract.observed program cfg in
  let handlers = List.combine (Irq_contract.handlers program.game.rom) verdicts in
  let undeclared handler =
    match List.assoc handler handlers with
    | Irq_contract.Proven flags -> not (Ccr.subset flags cfg.interrupt_observes)
    | Unknown _ -> false
  in
  { game = program.game.config.game.id; interrupt_observes = cfg.interrupt_observes; handlers;
    undeclared }

let check_irq_text irq =
  let describe (handler, verdict) =
    match verdict with
    | Irq_contract.Proven flags ->
        Printf.sprintf "%#x Proven %s%s" handler (Decisions.flags flags)
          (if irq.undeclared handler then " (undeclared)" else "")
    | Unknown { handler; at; why } -> Printf.sprintf "%#x Unknown at %#x (%s)" handler at why
  in
  Printf.sprintf "%s [interrupt_observes=%s] %s\n" irq.game
    (Decisions.flags irq.interrupt_observes)
    (String.concat ", " (List.map describe irq.handlers))

let check_irq_json irq =
  let handler (pc, verdict) =
    Json.Object (
      ["handler", Json.Int pc]
      @ (match verdict with
          | Irq_contract.Proven flags ->
              ["verdict", Json.String "proven"; "flags", String (letters flags);
               "undeclared", Bool (irq.undeclared pc)]
          | Unknown { handler = _; at; why } ->
              ["verdict", Json.String "unknown"; "at", Int at; "why", String why]))
  in
  Json.Object [
    "game", String irq.game;
    "interrupt_observes", String (letters irq.interrupt_observes);
    "handlers", Array (List.map handler irq.handlers);
  ]

(* ---- decisions ---- *)

let decisions_tsv ?sprite_header ?sprite_stats ?(writers = []) ?profile ~sprite_only
    (program : Program.t) =
  let groups = Sprite_trace.run program ~config:program.game.config ~writers in
  Option.iter (fun path -> Output.write_file path (Sprite_groups.render_hpp groups)) sprite_header;
  Option.iter (fun path ->
      groups.stats
      |> List.map (fun (key, count) -> Printf.sprintf "%s\t%d\n" key count)
      |> String.concat "" |> Output.write_file path) sprite_stats;
  let rows = Sprite_groups.decision_rows groups in
  if sprite_only then Decisions.header ^ "\n" ^ String.concat "" (List.map snd rows)
  else Program.select ~profile program |> Decisions.render ~sprite_rows:rows ~groups

let decisions_json ~game ~sprite_only tsv =
  let columns = String.split_on_char '\t' Decisions.header in
  let nullable text = if text = "-" then Json.Null else Json.String text in
  let row line =
    match String.split_on_char '\t' line with
    | [pc; disposition; length; flow; reads; writes; live_out; idle; chain] ->
        Some (Json.Object [
          "pc", Json.Int (int_of_string pc);
          "disposition", String disposition;
          "length", (match int_of_string_opt length with Some n -> Json.Int n | None -> Null);
          "flow", nullable flow;
          "reads", nullable reads;
          "writes", nullable writes;
          "live_out", nullable live_out;
          "idle", nullable idle;
          "chain_candidates",
            (if chain = "-" then Json.Array []
             else Json.Array (List.map (fun pc -> Json.Int (int_of_string pc))
                                (String.split_on_char ',' chain)));
        ])
    | _ -> None
  in
  let rows = match String.split_on_char '\n' tsv with
    | _header :: lines -> List.filter_map row lines
    | [] -> []
  in
  Json.Object [
    "game", String game;
    "sprite_only", Bool sprite_only;
    "columns", Array (List.map (fun column -> Json.String column) columns);
    "rows", Array rows;
  ]

(* ---- lower ---- *)

let parse_live_out value =
  let value = String.uppercase_ascii value in
  if value = "NONE" then Ccr.none
  else
    String.fold_left
      (fun set char ->
        let flag = match char with
          | 'X' -> Ccr.X | 'N' -> N | 'Z' -> Z | 'V' -> V | 'C' -> C
          | _ -> Error.fail "invalid --live-out %S" value
        in
        Ccr.union set (Ccr.singleton flag))
      Ccr.none value

let parse_hex hex =
  let digits = Buffer.create (String.length hex) in
  String.iter
    (function
      | ' ' | '\t' | '\r' | '\n' -> ()
      | ('0' .. '9' | 'a' .. 'f' | 'A' .. 'F') as digit -> Buffer.add_char digits digit
      | _ -> Error.fail "invalid --hex byte string")
    hex;
  let digits = Buffer.contents digits in
  if String.length digits mod 2 <> 0 then Error.fail "--hex requires whole bytes";
  String.init (String.length digits / 2) (fun i ->
      Char.chr (int_of_string ("0x" ^ String.sub digits (2 * i) 2)))

type lowered = { line : Inspect.line; c : (C.stmt list, string) result }

let lower (image : Image.t) ~pc ~count ~live_out =
  let rec listing pc remaining acc =
    if remaining = 0 then List.rev acc
    else
      let decoded = Decode.at image pc in
      let length = match decoded with Insn insn -> insn.length | Illegal illegal -> illegal.length in
      let bytes =
        if Image.contains_range image pc ~length then (Image.slice image pc ~length).bytes else "" in
      let c =
        decoded |> Semantics.of_decoded ~pc |> Lower.entry ~memory:Memory_path.Inline ~live_out
        |> Result.map (fun (entry : Lower.entry) -> entry.stmts)
      in
      listing (pc + length) (remaining - 1) ({ line = { pc; length; bytes; decoded }; c } :: acc)
  in
  listing pc count []

let lower_text ~count lowered =
  List.map (fun { line; c } ->
      match c with
      | Error reason -> Error.fail ~where:(Printf.sprintf "0x%x" line.pc) "%s" reason
      | Ok stmts ->
          if count = 1 then C.print_stmts stmts
          else Printf.sprintf "/* 0x%x */\n%s" line.pc (C.print_stmt (Block stmts)))
    lowered
  |> String.concat ""

let lower_json ~live_out lowered =
  let instruction { line; c } =
    match Inspect.line_to_json line, c with
    | Json.Object fields, Ok stmts -> Json.Object (fields @ ["c", Json.String (C.print_stmts stmts)])
    | Json.Object fields, Error reason -> Json.Object (fields @ ["error", Json.String reason])
    | json, _ -> json
  in
  Json.Object [
    "pc", Int (match lowered with { line; _ } :: _ -> line.pc | [] -> 0);
    "count", Int (List.length lowered);
    "live_out", String (letters live_out);
    "instructions", Array (List.map instruction lowered);
  ]
