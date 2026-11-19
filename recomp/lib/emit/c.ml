type typ = U8 | U16 | U32 | I8 | I16 | I32

type binop =
  | Add | Sub | Mul | And | Or | Xor | Shl | Shr | Eq | Ne | Lt
type expr =
  | UInt of int
  | And_then of expr list
  | Name of string
  | Field of expr * string
  | Index of expr * expr
  | Call of string * expr list
  | Cast of typ * expr
  | Not of expr
  | Bit_not of expr
  | Bin of binop * expr * expr
  | Select of expr * expr * expr

type stmt =
  | Decl of typ * string * expr
  | Assign of expr * expr
  | Expr of expr
  | Block of stmt list
  | If of expr * stmt list * stmt list
  | Goto of string
  | Label of string
  | Switch of expr * (int * stmt list) list * stmt list
  | Return
  | Transfer (* A completed instruction whose next PC may be chained. *)
  | Break
  | Chain of string
  | Dispatch_pc
  | Repeat of stmt list

type func = { name : string; body : stmt list }

let type_name = function
  | U8 -> "uint8_t" | U16 -> "uint16_t" | U32 -> "uint32_t"
  | I8 -> "int8_t" | I16 -> "int16_t" | I32 -> "int32_t"

let operator = function
  | Or -> "|", 3 | Xor -> "^", 4 | And -> "&", 5
  | Eq -> "==", 6 | Ne -> "!=", 6 | Lt -> "<", 7
  | Shl -> "<<", 8 | Shr -> ">>", 8 | Add -> "+", 9 | Sub -> "-", 9 | Mul -> "*", 10

(* Operators whose mixed-operator operands GCC's -Wparentheses flags. *)
let warns_unparenthesised = function
  | Or | Xor | And | Shl | Shr | Eq | Ne | Lt -> true
  | Add | Sub | Mul -> false

let rec print_at minimum expr =
  let precedence, text = match expr with
    | UInt value -> 13, Printf.sprintf "0x%xu" (value land 0xffffffff)
    | And_then terms -> 2, String.concat " && " (List.map (print_at 3) terms)
    | Name name -> 13, name
    | Field (base, field) -> 12, print_at 12 base ^ "->" ^ field
    | Index (base, index) -> 12, print_at 12 base ^ "[" ^ print_at 0 index ^ "]"
    | Call (name, args) -> 12, name ^ "(" ^ String.concat ", " (List.map (print_at 0) args) ^ ")"
    | Cast (typ, value) -> 11, "(" ^ type_name typ ^ ")" ^ print_at 11 value
    | Not value -> 11, "!" ^ print_at 11 value
    | Bit_not value -> 11, "~" ^ print_at 11 value
    | Bin (op, left, right) ->
        let symbol, level = operator op in
        let operand minimum operand = match operand with
          | Bin (inner, _, _) when inner <> op && warns_unparenthesised op -> "(" ^ print_at 0 operand ^ ")"
          | _ -> print_at minimum operand
        in
        level, operand level left ^ " " ^ symbol ^ " " ^ operand (level + 1) right
    | Select (test, yes, no) ->
        (* Clang's -Wbitwise-conditional-parentheses flags a bare bitwise test. *)
        let test = match test with
          | Bin ((Or | Xor | And), _, _) -> "(" ^ print_at 0 test ^ ")"
          | _ -> print_at 2 test
        in
        1, test ^ " ? " ^ print_at 0 yes ^ " : " ^ print_at 1 no
  in
  if precedence < minimum then "(" ^ text ^ ")" else text

let print_expr = print_at 0

let rec write_stmt buffer depth stmt =
  let line text =
    Buffer.add_string buffer (String.make (depth * 4) ' ');
    Buffer.add_string buffer text;
    Buffer.add_char buffer '\n'
  in
  let body = List.iter (write_stmt buffer (depth + 1)) in
  let expr = print_expr in
  match stmt with
  | Decl (typ, name, value) -> line (type_name typ ^ " " ^ name ^ " = " ^ expr value ^ ";")
  | Assign (target, value) -> line (expr target ^ " = " ^ expr value ^ ";")
  | Expr value -> line (expr value ^ ";")
  | Block stmts -> line "{"; body stmts; line "}"
  | If (test, yes, no) ->
      line ("if (" ^ expr test ^ ") {");
      body yes;
      if no <> [] then (line "} else {"; body no);
      line "}"
  | Goto label -> line ("goto " ^ label ^ ";")
  | Label label -> line (label ^ ":;")
  | Switch (test, cases, otherwise) ->
      line ("switch (" ^ expr test ^ ") {");
      List.iter
        (fun (value, stmts) -> line ("case " ^ expr (UInt value) ^ ":"); body [Block stmts])
        cases;
      line "default:";
      body [Block otherwise];
      line "}"
  | Return | Transfer -> line "return;"
  | Break -> line "break;"
  | Chain name -> line ("F3_CHAIN(" ^ name ^ ", cpu);")
  | Dispatch_pc -> line "F3_DISPATCH_PC(cpu);"
  | Repeat stmts -> line "for (;;) {"; body stmts; line "}"

let print_stmts stmts =
  let buffer = Buffer.create 256 in
  List.iter (write_stmt buffer 0) stmts;
  Buffer.contents buffer

let print_stmt stmt = print_stmts [stmt]

let print_func { name; body } =
  "void " ^ name ^ "(f3_cpu *cpu) " ^ print_stmt (Block body)

(* Names an expression or statement list refers to. *)
let rec expr_names acc = function
  | And_then terms -> List.fold_left expr_names acc terms
  | UInt _ -> acc
  | Name name -> name :: acc
  | Field (e, _) | Cast (_, e) | Not e | Bit_not e -> expr_names acc e
  | Index (a, b) | Bin (_, a, b) -> expr_names (expr_names acc a) b
  | Call (_, args) -> List.fold_left expr_names acc args
  | Select (a, b, c) -> expr_names (expr_names (expr_names acc a) b) c

let rec stmt_names acc = function
  | Decl (_, _, value) | Expr value -> expr_names acc value
  | Assign (target, value) -> expr_names (expr_names acc target) value
  | Block stmts -> List.fold_left stmt_names acc stmts
  | If (test, yes, no) ->
      List.fold_left stmt_names (List.fold_left stmt_names (expr_names acc test) yes) no
  | Switch (test, cases, otherwise) ->
      let acc = expr_names acc test in
      let acc = List.fold_left (fun acc (_, stmts) -> List.fold_left stmt_names acc stmts) acc cases in
      List.fold_left stmt_names acc otherwise
  | Goto _ | Label _ | Return | Transfer | Break | Chain _ | Dispatch_pc -> acc
  | Repeat stmts -> List.fold_left stmt_names acc stmts

let rec has_call = function
  | And_then terms -> List.exists has_call terms
  | Call _ -> true
  | UInt _ | Name _ -> false
  | Field (e, _) | Cast (_, e) | Not e | Bit_not e -> has_call e
  | Index (a, b) | Bin (_, a, b) -> has_call a || has_call b
  | Select (a, b, c) -> has_call a || has_call b || has_call c

(* Remove declarations nothing reads. A declaration whose value calls into the
   runtime (a bus read) keeps the call as a statement, so the access still
   happens in order. Repeats until no declaration is dropped. *)
let rec drop_unused_decls stmts =
  let used = List.fold_left stmt_names [] stmts in
  let changed = ref false in
  let rec rewrite stmts = List.filter_map (fun stmt -> match stmt with
    | Decl (_, name, value) when not (List.mem name used) ->
        changed := true;
        if has_call value then Some (Expr value) else None
    | Block body -> Some (Block (rewrite body))
    | If (test, yes, no) -> Some (If (test, rewrite yes, rewrite no))
    | Switch (test, cases, otherwise) ->
        Some (Switch (test, List.map (fun (v, body) -> v, rewrite body) cases, rewrite otherwise))
    | Repeat body -> Some (Repeat (rewrite body))
    | stmt -> Some stmt) stmts
  in
  let stmts = rewrite stmts in
  if !changed then drop_unused_decls stmts else stmts
