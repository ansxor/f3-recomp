type typ = U8 | U16 | U32 | I8 | I16 | I32

type binop =
  | Add | Sub | And | Or | Xor | Shl | Shr | Eq | Ne | Lt
type expr =
  | UInt of int
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
  | Chain of string
  | Dispatch_pc

type func = { name : string; body : stmt list }

let type_name = function
  | U8 -> "uint8_t" | U16 -> "uint16_t" | U32 -> "uint32_t"
  | I8 -> "int8_t" | I16 -> "int16_t" | I32 -> "int32_t"

let operator = function
  | Or -> "|", 2 | Xor -> "^", 3 | And -> "&", 4
  | Eq -> "==", 5 | Ne -> "!=", 5 | Lt -> "<", 6
  | Shl -> "<<", 7 | Shr -> ">>", 7 | Add -> "+", 8 | Sub -> "-", 8

let rec print_at minimum expr =
  let precedence, text = match expr with
    | UInt value -> 11, Printf.sprintf "0x%xu" (value land 0xffffffff)
    | Name name -> 11, name
    | Field (base, field) -> 10, print_at 10 base ^ "->" ^ field
    | Index (base, index) -> 10, print_at 10 base ^ "[" ^ print_at 0 index ^ "]"
    | Call (name, args) -> 10, name ^ "(" ^ String.concat ", " (List.map (print_at 0) args) ^ ")"
    | Cast (typ, value) -> 9, "(" ^ type_name typ ^ ")" ^ print_at 9 value
    | Not value -> 9, "!" ^ print_at 9 value
    | Bit_not value -> 9, "~" ^ print_at 9 value
    | Bin (op, left, right) ->
        let symbol, level = operator op in
        level, print_at level left ^ " " ^ symbol ^ " " ^ print_at (level + 1) right
    | Select (test, yes, no) ->
        1, print_at 2 test ^ " ? " ^ print_at 0 yes ^ " : " ^ print_at 1 no
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
  | Return -> line "return;"
  | Chain name -> line ("F3_CHAIN(" ^ name ^ ", cpu);")
  | Dispatch_pc -> line "F3_DISPATCH_PC(cpu);"

let print_stmts stmts =
  let buffer = Buffer.create 256 in
  List.iter (write_stmt buffer 0) stmts;
  Buffer.contents buffer

let print_stmt stmt = print_stmts [stmt]

let print_func { name; body } =
  "void " ^ name ^ "(f3_cpu *cpu) " ^ print_stmt (Block body)
