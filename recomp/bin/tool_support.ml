(* Shared plumbing for the MCP tools of [f3recomp mcp]: JSON conversion, argument access, schemas and
   result builders. *)

module Y = Yojson.Safe

(* Raised by tool handlers for bad arguments and failed operations; becomes an [isError] result. *)
exception Tool_error of string

let fail fmt = Printf.ksprintf (fun message -> raise (Tool_error message)) fmt

(* ---- F3recomp.Json <-> Yojson ---- *)

let rec yojson_of_json : F3recomp.Json.t -> Y.t = function
  | Null -> `Null
  | Bool b -> `Bool b
  | Int n -> `Int n
  | Int64 n -> `Intlit (Int64.to_string n)
  | Float f -> `Float f
  | String s -> `String s
  | Array items -> `List (List.map yojson_of_json items)
  | Object fields -> `Assoc (List.map (fun (key, value) -> (key, yojson_of_json value)) fields)

let rec json_of_yojson : Y.t -> F3recomp.Json.t = function
  | `Null -> Null
  | `Bool b -> Bool b
  | `Int n -> Int n
  | `Intlit text -> (
      match Int64.of_string_opt text with Some n -> Int64 n | None -> Float (float_of_string text))
  | `Float f -> Float f
  | `String s -> String s
  | `List items -> Array (List.map json_of_yojson items)
  | `Assoc fields -> Object (List.map (fun (key, value) -> (key, json_of_yojson value)) fields)

(* ---- arguments ---- *)

let fields = function
  | `Assoc fields -> fields
  | `Null -> []
  | _ -> fail "arguments must be an object"

let member key args =
  match List.assoc_opt key (fields args) with Some `Null | None -> None | Some value -> Some value

let string_arg key args =
  match member key args with
  | None -> None
  | Some (`String text) -> Some text
  | Some _ -> fail "%S must be a string" key

let require_string key args =
  match string_arg key args with Some text -> text | None -> fail "missing argument %S" key

let bool_arg key args =
  match member key args with
  | None -> None
  | Some (`Bool value) -> Some value
  | Some _ -> fail "%S must be a boolean" key

let int_arg key args =
  match member key args with
  | None -> None
  | Some (`Int n) -> Some n
  | Some (`String text) -> Some (F3recomp.Inspect.parse_address text)
  | Some _ -> fail "%S must be an integer" key

(* Rejects argument names the tool's schema does not declare, so a typo is an error, not a silent default. *)
let check_known ~schema args =
  let declared =
    match schema with
    | `Assoc schema_fields -> (
        match List.assoc_opt "properties" schema_fields with Some (`Assoc props) -> List.map fst props | _ -> [])
    | _ -> []
  in
  List.iter
    (fun (key, _) -> if not (List.mem key declared) then fail "unknown argument %S" key)
    (fields args)

(* ---- schemas ---- *)

let described description rest = `Assoc (rest @ [ ("description", `String description) ])
let string_prop description = described description [ ("type", `String "string") ]
let bool_prop description = described description [ ("type", `String "boolean") ]
let int_prop description = described description [ ("type", `String "integer") ]

let enum_prop description values =
  described description [ ("type", `String "string"); ("enum", `List (List.map (fun v -> `String v) values)) ]

(* Integers or hex strings ("0x400", "$400"). *)
let address_prop description =
  described description [ ("type", `List [ `String "integer"; `String "string" ]) ]

let object_schema ?(required = []) properties : Y.t =
  `Assoc
    [
      ("type", `String "object");
      ("properties", `Assoc properties);
      ("required", `List (List.map (fun name -> `String name) required));
    ]

(* ---- results ---- *)

let text_content text : Mcp.Types.Content.t = Text { type_ = "text"; text; meta = None }

(* A structured result: the JSON object, also serialised as text for clients that only read content. *)
let structured json = Mcp_sdk.Tool_result.structured ~text:(Y.to_string json) json
let error_result message = Mcp_sdk.Tool_result.error message

(* Runs a tool body, mapping expected failures to an [isError] result. Cancellation is not caught. *)
let protect body =
  match body () with
  | result -> result
  | exception Tool_error message -> error_result message
  | exception F3recomp.Error.Invalid { where; what } ->
      error_result (if where = "" then what else where ^ ": " ^ what)
  | exception Sys_error message -> error_result message
  | exception Unix.Unix_error (error, operation, path) ->
      error_result (Printf.sprintf "%s %s: %s" operation path (Unix.error_message error))
  | exception (Eio.Io _ as exn) -> error_result (Printexc.to_string exn)
  | exception Failure message -> error_result message
  | exception Invalid_argument message -> error_result message
  | exception Not_found -> error_result "not found"
