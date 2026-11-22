(* Protocol-level tests of [f3recomp mcp] over stdio: structuredContent shape, protocol version negotiation, JSON-RPC
   error codes versus [isError] results, omitted arguments, and strict input schemas. None needs a ROM or a runtime. *)

open F3recomp
open Json

let f3recomp = "../../bin/main.exe"

(* ---- a minimal MCP client over a child process ---- *)

type client = { pid : int; to_server : Unix.file_descr; from_server : Unix.file_descr; mutable buffer : string; mutable next_id : int }

let spawn () =
  let stdin_r, stdin_w = Unix.pipe ~cloexec:true () in
  let stdout_r, stdout_w = Unix.pipe ~cloexec:true () in
  let pid = Unix.create_process f3recomp [| f3recomp; "mcp" |] stdin_r stdout_w Unix.stderr in
  List.iter Unix.close [ stdin_r; stdout_w ];
  { pid; to_server = stdin_w; from_server = stdout_r; buffer = ""; next_id = 0 }

let send_line client line =
  let data = Bytes.of_string (line ^ "\n") in
  let rec go offset =
    if offset < Bytes.length data then go (offset + Unix.write client.to_server data offset (Bytes.length data - offset))
  in
  go 0

let read_line client =
  let chunk = Bytes.create 65536 in
  let rec go () =
    match String.index_opt client.buffer '\n' with
    | Some i ->
        let line = String.sub client.buffer 0 i in
        client.buffer <- String.sub client.buffer (i + 1) (String.length client.buffer - i - 1);
        line
    | None -> (
        match Unix.select [ client.from_server ] [] [] 60. with
        | [], _, _ -> Alcotest.fail "timed out waiting for the server"
        | _ ->
            let n = Unix.read client.from_server chunk 0 (Bytes.length chunk) in
            if n = 0 then Alcotest.fail "server closed its stdout";
            client.buffer <- client.buffer ^ Bytes.sub_string chunk 0 n;
            go ())
  in
  go ()

let parse line = match Json.parse line with Ok json -> json | Error m -> Alcotest.failf "invalid JSON (%s): %s" m line

(* The whole response to a request; notifications met on the way are dropped. *)
let request_raw client meth params =
  client.next_id <- client.next_id + 1;
  let id = client.next_id in
  send_line client
    (Json.to_string (Object [ "jsonrpc", String "2.0"; "id", Int id; "method", String meth; "params", params ]));
  let rec wait () =
    let message = parse (read_line client) in
    if Json.member "id" message = Some (Int id) then message else wait ()
  in
  wait ()

let field key json =
  match Json.member key json with Some v -> v | None -> Alcotest.failf "missing %S in %s" key (Json.to_string json)

let result_of response =
  match Json.member "result" response with Some r -> r | None -> Alcotest.failf "request failed: %s" (Json.to_string response)

let error_code response = match Json.member "error" response with
  | Some error -> (match field "code" error with Int code -> code | _ -> Alcotest.fail "error code is not an int")
  | None -> Alcotest.failf "expected a JSON-RPC error: %s" (Json.to_string response)

let initialize ?(version = "2025-11-25") client =
  let response =
    request_raw client "initialize"
      (Object [ "protocolVersion", String version; "capabilities", Object [];
                "clientInfo", Object [ "name", String "mcp_protocol_test"; "version", String "0" ] ])
  in
  send_line client (Json.to_string (Object [ "jsonrpc", String "2.0"; "method", String "notifications/initialized" ]));
  result_of response

let finish client =
  Unix.close client.to_server;
  ignore (Unix.waitpid [] client.pid);
  Unix.close client.from_server

let with_server ?version body =
  let client = spawn () in
  Fun.protect ~finally:(fun () -> try Unix.kill client.pid Sys.sigkill with Unix.Unix_error _ -> ())
    (fun () -> let init = initialize ?version client in body client init; finish client)

let call_raw client params = request_raw client "tools/call" params
let call_args name arguments = Object [ "name", String name; "arguments", arguments ]
let disasm_args = Object [ "pc", Int 0x1000; "hex", String "4e75" ]

(* ---- tests ---- *)

let disasm_structured_is_object () =
  with_server (fun client _ ->
    let result = result_of (call_raw client (call_args "disasm" disasm_args)) in
    let structured = field "structuredContent" result in
    (match structured with Object _ -> () | other -> Alcotest.failf "structuredContent is not an object: %s" (Json.to_string other));
    (match field "lines" structured with
     | Array [ line ] -> Alcotest.(check bool) "rts" true (Json.member "text" line = Some (String "rts"))
     | other -> Alcotest.failf "expected one line: %s" (Json.to_string other)))

let protocol_version_negotiation () =
  let version_for requested =
    let result = ref "" in
    with_server ~version:requested (fun _ init -> match field "protocolVersion" init with String v -> result := v | _ -> ());
    !result
  in
  Alcotest.(check string) "unsupported version gets the latest" "2025-11-25" (version_for "1900-01-01");
  Alcotest.(check string) "a supported version is echoed" "2025-03-26" (version_for "2025-03-26");
  Alcotest.(check string) "the latest is echoed" "2025-11-25" (version_for "2025-11-25");
  Alcotest.(check string) "an older supported version is echoed" "2024-11-05" (version_for "2024-11-05")

let rpc_error_codes () =
  with_server (fun client _ ->
    Alcotest.(check int) "unknown method" (-32601) (error_code (request_raw client "no/such/method" (Object [])));
    Alcotest.(check int) "missing name" (-32602) (error_code (call_raw client (Object [ "arguments", Object [] ])));
    Alcotest.(check int) "name of the wrong type" (-32602)
      (error_code (call_raw client (Object [ "name", Int 3; "arguments", Object [] ])));
    Alcotest.(check int) "arguments is a string" (-32602)
      (error_code (call_raw client (Object [ "name", String "disasm"; "arguments", String "pc=0" ])));
    Alcotest.(check int) "arguments is an array" (-32602)
      (error_code (call_raw client (Object [ "name", String "disasm"; "arguments", Array [] ])));
    Alcotest.(check int) "unknown tool" (-32602) (error_code (call_raw client (call_args "no_such_tool" (Object []))));
    Alcotest.(check int) "runtime tool without a session" (-32602)
      (error_code (call_raw client (call_args "rt_step" (Object []))));
    (* A tool that exists and fails is a successful JSON-RPC response with isError. *)
    let failed = result_of (call_raw client (call_args "disasm" (Object []))) in
    Alcotest.(check bool) "missing argument is isError" true (Json.member "isError" failed = Some (Bool true));
    let bogus = result_of (call_raw client (call_args "disasm" (Object [ "pc", Int 0; "bogus", Int 1 ]))) in
    Alcotest.(check bool) "unknown argument is isError" true (Json.member "isError" bogus = Some (Bool true));
    (* The server keeps serving after every one of those. *)
    Alcotest.(check bool) "still serving" true (result_of (request_raw client "ping" (Object [])) = Object []))

let omitted_arguments () =
  with_server (fun client _ ->
    let result = result_of (request_raw client "tools/call" (Object [ "name", String "session_list" ])) in
    Alcotest.(check bool) "not an error" true (Json.member "isError" result <> Some (Bool true));
    ignore (field "sessions" (field "structuredContent" result));
    let null_args = result_of (request_raw client "tools/call" (Object [ "name", String "session_list"; "arguments", Null ])) in
    Alcotest.(check bool) "null arguments are {}" true (Json.member "isError" null_args <> Some (Bool true)))

let strict_schemas () =
  with_server (fun client _ ->
    let tools = match field "tools" (result_of (request_raw client "tools/list" (Object []))) with
      | Array tools -> tools | _ -> Alcotest.fail "tools is not an array" in
    Alcotest.(check bool) "tools are listed" true (tools <> []);
    List.iter
      (fun tool ->
        let name = match field "name" tool with String s -> s | _ -> "?" in
        Alcotest.(check bool) (name ^ " has additionalProperties:false") true
          (Json.member "additionalProperties" (field "inputSchema" tool) = Some (Bool false)))
      tools)

let () =
  Alcotest.run "mcp_protocol"
    [ ( "protocol",
        [ Alcotest.test_case "disasm structuredContent is an object" `Quick disasm_structured_is_object;
          Alcotest.test_case "protocol version negotiation" `Quick protocol_version_negotiation;
          Alcotest.test_case "JSON-RPC error codes versus isError" `Quick rpc_error_codes;
          Alcotest.test_case "omitted arguments" `Quick omitted_arguments;
          Alcotest.test_case "input schemas reject unknown keys" `Quick strict_schemas ] ) ]
