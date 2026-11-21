type t =
  | Null
  | Bool of bool
  | Int of int
  | Int64 of int64
  | Float of float
  | String of string
  | Array of t list
  | Object of (string * t) list

let escape buffer text =
  Buffer.add_char buffer '"';
  String.iter
    (function
      | '"' -> Buffer.add_string buffer "\\\""
      | '\\' -> Buffer.add_string buffer "\\\\"
      | '\n' -> Buffer.add_string buffer "\\n"
      | '\r' -> Buffer.add_string buffer "\\r"
      | '\t' -> Buffer.add_string buffer "\\t"
      | '\b' -> Buffer.add_string buffer "\\b"
      | '\012' -> Buffer.add_string buffer "\\f"
      | c when Char.code c < 0x20 -> Printf.bprintf buffer "\\u%04x" (Char.code c)
      | c -> Buffer.add_char buffer c)
    text;
  Buffer.add_char buffer '"'

let float_text value =
  if Float.is_integer value && Float.abs value < 1e15 then Printf.sprintf "%.0f" value
  else Printf.sprintf "%.17g" value

let rec write buffer = function
  | Null -> Buffer.add_string buffer "null"
  | Bool value -> Buffer.add_string buffer (if value then "true" else "false")
  | Int value -> Buffer.add_string buffer (string_of_int value)
  | Int64 value -> Buffer.add_string buffer (Int64.to_string value)
  | Float value ->
      Buffer.add_string buffer (if Float.is_finite value then float_text value else "null")
  | String text -> escape buffer text
  | Array items ->
      Buffer.add_char buffer '[';
      List.iteri (fun i item ->
        if i > 0 then Buffer.add_char buffer ',';
        write buffer item) items;
      Buffer.add_char buffer ']'
  | Object fields ->
      Buffer.add_char buffer '{';
      List.iteri (fun i (key, value) ->
        if i > 0 then Buffer.add_char buffer ',';
        escape buffer key;
        Buffer.add_char buffer ':';
        write buffer value) fields;
      Buffer.add_char buffer '}'

let to_string json =
  let buffer = Buffer.create 256 in
  write buffer json;
  Buffer.contents buffer

let member key = function
  | Object fields -> List.assoc_opt key fields
  | _ -> None

let hex value = String (Printf.sprintf "0x%06x" value)

exception Syntax of string

type input = { text : string; mutable pos : int }

let fail input fmt =
  Printf.ksprintf (fun message ->
    raise (Syntax (Printf.sprintf "%s at offset %d" message input.pos))) fmt

let peek input = if input.pos < String.length input.text then Some input.text.[input.pos] else None

let rec skip_space input =
  match peek input with
  | Some (' ' | '\t' | '\r' | '\n') -> input.pos <- input.pos + 1; skip_space input
  | _ -> ()

let expect input char =
  if peek input = Some char then input.pos <- input.pos + 1
  else fail input "expected '%c'" char

let literal input word value =
  let length = String.length word in
  if input.pos + length <= String.length input.text
     && String.sub input.text input.pos length = word
  then (input.pos <- input.pos + length; value)
  else fail input "invalid literal"

let add_utf8 buffer code =
  if code < 0x80 then Buffer.add_char buffer (Char.chr code)
  else if code < 0x800 then (
    Buffer.add_char buffer (Char.chr (0xc0 lor (code lsr 6)));
    Buffer.add_char buffer (Char.chr (0x80 lor (code land 0x3f))))
  else if code < 0x10000 then (
    Buffer.add_char buffer (Char.chr (0xe0 lor (code lsr 12)));
    Buffer.add_char buffer (Char.chr (0x80 lor ((code lsr 6) land 0x3f)));
    Buffer.add_char buffer (Char.chr (0x80 lor (code land 0x3f))))
  else (
    Buffer.add_char buffer (Char.chr (0xf0 lor (code lsr 18)));
    Buffer.add_char buffer (Char.chr (0x80 lor ((code lsr 12) land 0x3f)));
    Buffer.add_char buffer (Char.chr (0x80 lor ((code lsr 6) land 0x3f)));
    Buffer.add_char buffer (Char.chr (0x80 lor (code land 0x3f))))

let hex4 input =
  if input.pos + 4 > String.length input.text then fail input "truncated \\u escape";
  let digits = String.sub input.text input.pos 4 in
  if not (String.for_all (function '0' .. '9' | 'a' .. 'f' | 'A' .. 'F' -> true | _ -> false) digits)
  then fail input "invalid \\u escape";
  input.pos <- input.pos + 4;
  int_of_string ("0x" ^ digits)

let unicode_escape input buffer =
  let high = hex4 input in
  if high >= 0xd800 && high < 0xdc00 then (
    if peek input = Some '\\' && input.pos + 1 < String.length input.text
       && input.text.[input.pos + 1] = 'u'
    then (
      input.pos <- input.pos + 2;
      let low = hex4 input in
      if low < 0xdc00 || low > 0xdfff then fail input "invalid surrogate pair";
      add_utf8 buffer (0x10000 + ((high - 0xd800) lsl 10) + (low - 0xdc00)))
    else fail input "lone surrogate")
  else if high >= 0xdc00 && high < 0xe000 then fail input "lone surrogate"
  else add_utf8 buffer high

let string input =
  expect input '"';
  let buffer = Buffer.create 16 in
  let rec loop () =
    match peek input with
    | None -> fail input "unterminated string"
    | Some '"' -> input.pos <- input.pos + 1
    | Some '\\' ->
        input.pos <- input.pos + 1;
        (match peek input with
         | None -> fail input "unterminated escape"
         | Some c ->
             input.pos <- input.pos + 1;
             (match c with
              | '"' -> Buffer.add_char buffer '"'
              | '\\' -> Buffer.add_char buffer '\\'
              | '/' -> Buffer.add_char buffer '/'
              | 'b' -> Buffer.add_char buffer '\b'
              | 'f' -> Buffer.add_char buffer '\012'
              | 'n' -> Buffer.add_char buffer '\n'
              | 'r' -> Buffer.add_char buffer '\r'
              | 't' -> Buffer.add_char buffer '\t'
              | 'u' -> unicode_escape input buffer
              | _ -> input.pos <- input.pos - 1; fail input "invalid escape"));
        loop ()
    | Some c when Char.code c < 0x20 -> fail input "control character in string"
    | Some c -> Buffer.add_char buffer c; input.pos <- input.pos + 1; loop ()
  in
  loop ();
  Buffer.contents buffer

let number input =
  let start = input.pos in
  let digits () =
    let from = input.pos in
    while (match peek input with Some '0' .. '9' -> true | _ -> false) do
      input.pos <- input.pos + 1
    done;
    if input.pos = from then fail input "expected digit"
  in
  if peek input = Some '-' then input.pos <- input.pos + 1;
  (match peek input with
   | Some '0' -> input.pos <- input.pos + 1
   | _ -> digits ());
  let integral = ref true in
  if peek input = Some '.' then (integral := false; input.pos <- input.pos + 1; digits ());
  (match peek input with
   | Some ('e' | 'E') ->
       integral := false;
       input.pos <- input.pos + 1;
       (match peek input with Some ('+' | '-') -> input.pos <- input.pos + 1 | _ -> ());
       digits ()
   | _ -> ());
  let text = String.sub input.text start (input.pos - start) in
  if !integral then
    match int_of_string_opt text with
    | Some n -> Int n
    | None ->
        (match Int64.of_string_opt text with Some n -> Int64 n | None -> Float (float_of_string text))
  else Float (float_of_string text)

let max_depth = 512

let rec value input depth =
  if depth > max_depth then fail input "nesting too deep";
  skip_space input;
  match peek input with
  | None -> fail input "unexpected end of input"
  | Some '{' ->
      input.pos <- input.pos + 1;
      skip_space input;
      if peek input = Some '}' then (input.pos <- input.pos + 1; Object [])
      else
        let rec fields acc =
          skip_space input;
          let key = string input in
          skip_space input;
          expect input ':';
          let item = value input (depth + 1) in
          skip_space input;
          match peek input with
          | Some ',' -> input.pos <- input.pos + 1; fields ((key, item) :: acc)
          | Some '}' -> input.pos <- input.pos + 1; Object (List.rev ((key, item) :: acc))
          | _ -> fail input "expected ',' or '}'"
        in
        fields []
  | Some '[' ->
      input.pos <- input.pos + 1;
      skip_space input;
      if peek input = Some ']' then (input.pos <- input.pos + 1; Array [])
      else
        let rec items acc =
          let item = value input (depth + 1) in
          skip_space input;
          match peek input with
          | Some ',' -> input.pos <- input.pos + 1; items (item :: acc)
          | Some ']' -> input.pos <- input.pos + 1; Array (List.rev (item :: acc))
          | _ -> fail input "expected ',' or ']'"
        in
        items []
  | Some '"' -> String (string input)
  | Some 't' -> literal input "true" (Bool true)
  | Some 'f' -> literal input "false" (Bool false)
  | Some 'n' -> literal input "null" Null
  | Some ('-' | '0' .. '9') -> number input
  | Some c -> fail input "unexpected character '%c'" c

let parse text =
  let input = { text; pos = 0 } in
  match value input 0 with
  | json ->
      skip_space input;
      if input.pos = String.length text then Ok json
      else Error (Printf.sprintf "trailing data at offset %d" input.pos)
  | exception Syntax message -> Error message
