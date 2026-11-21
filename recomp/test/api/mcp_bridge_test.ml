(* Tests of the runtime bridge inside [f3recomp mcp]: concurrency and timeouts, session routing and failover,
   rt_init / a5 handling, tool_path resolution, and the one-shot runners.

   No real runtime is needed. This executable is also the fake runtime: a tool_path script execs it as
   [mcp_bridge_test.exe fake-child <mode> mcp --no-init], and it then speaks just enough MCP over stdio. *)

open F3recomp
open Json

(* ---- the fake runtime ---- *)

let fake_child mode =
  let write json =
    print_string (Json.to_string json);
    print_char '\n';
    flush stdout
  in
  let reply id result = write (Object [ "jsonrpc", String "2.0"; "id", id; "result", result ]) in
  let text_result ?(error = false) text =
    Object [ "content", Array [ Object [ "type", String "text"; "text", String text ] ]; "isError", Bool error ]
  in
  let tool name =
    Object [ "name", String name; "description", String name;
             "inputSchema", Object [ "type", String "object"; "properties", Object [] ] ]
  in
  let only = if mode = "normal-a" then "only_a" else "only_b" in
  let tools = List.map tool [ "echo"; "hang"; "step"; "read"; "init"; "die"; only ] in
  let rec loop () =
    match input_line stdin with
    | exception End_of_file -> ()
    | line ->
        (match Json.parse line with
         | Error _ -> ()
         | Ok message -> (
             let meth = match Json.member "method" message with Some (String m) -> m | _ -> "" in
             let id = Json.member "id" message in
             let params = Option.value (Json.member "params" message) ~default:(Object []) in
             match (meth, id) with
             | "initialize", Some id ->
                 if mode <> "stall-handshake" then
                   let version =
                     if mode = "bad-protocol" then String "1999-01-01"
                     else Option.value (Json.member "protocolVersion" params) ~default:(String "2025-11-25")
                   in
                   reply id
                     (Object [ "protocolVersion", version; "capabilities", Object [ "tools", Object [] ];
                               "serverInfo", Object [ "name", String "fake"; "version", String "0" ] ])
             | "tools/list", Some id -> reply id (Object [ "tools", Array tools ])
             | "tools/call", Some id -> (
                 let name = match Json.member "name" params with Some (String n) -> n | _ -> "" in
                 let arguments = Option.value (Json.member "arguments" params) ~default:(Object []) in
                 match name with
                 | "hang" | "step" -> ()
                 | "die" -> exit 0
                 | "init" -> reply id (text_result ("init " ^ Json.to_string arguments))
                 | "read" -> reply id (text_result (Json.to_string arguments))
                 | _ -> reply id (text_result ("from " ^ mode ^ " " ^ name))
                 )
             | _ -> ()));
        loop ()
  in
  loop ()

let () =
  if Array.length Sys.argv > 3 && Sys.argv.(1) = "fake-child" then begin
    fake_child Sys.argv.(2);
    exit 0
  end

(* ---- files ---- *)

let write_file path contents = Out_channel.with_open_bin path (fun out -> output_string out contents)
let read_file path = In_channel.with_open_bin path In_channel.input_all

let rec remove_tree path =
  match Sys.is_directory path with
  | true ->
      Array.iter (fun entry -> remove_tree (Filename.concat path entry)) (Sys.readdir path);
      (try Sys.rmdir path with Sys_error _ -> ())
  | false -> (try Sys.remove path with Sys_error _ -> ())
  | exception Sys_error _ -> ()

let absolute path = if Filename.is_relative path then Filename.concat (Sys.getcwd ()) path else path

(* A scratch repository root: games/fixture/config.toml, roms/fixture/main, bin/ for fake tools. *)
let root = lazy (
  let dir = Filename.temp_file ~temp_dir:(Sys.getcwd ()) "bridge" "" in
  Sys.remove dir;
  Sys.mkdir dir 0o700;
  at_exit (fun () -> remove_tree dir);
  absolute dir)

let path_in_root parts = List.fold_left Filename.concat (Lazy.force root) parts

let mkdirs path =
  let rec go path =
    if not (Sys.file_exists path) then begin
      go (Filename.dirname path);
      Sys.mkdir path 0o700
    end
  in
  go path

let self = absolute Sys.executable_name

(* An executable shell script. *)
let script name body =
  let dir = path_in_root [ "bin" ] in
  mkdirs dir;
  let path = Filename.concat dir name in
  write_file path ("#!/bin/sh\n" ^ body ^ "\n");
  Unix.chmod path 0o755;
  path

let fake_runtime ?(pid_file = "") name mode =
  script name
    ((if pid_file = "" then "" else Printf.sprintf "echo $$ > %s\n" pid_file)
     ^ Printf.sprintf "exec %s fake-child %s \"$@\"" self mode)

(* ---- the fixture game (a5 = 0x410000) ---- *)

let words list =
  let bytes = Bytes.create (2 * List.length list) in
  List.iteri (fun i word -> Bytes.set_uint16_be bytes (2 * i) word) list;
  Bytes.to_string bytes

let rom =
  let bytes = Bytes.make 0x1000 '\000' in
  let put offset data = Bytes.blit_string data 0 bytes offset (String.length data) in
  Bytes.set_int32_be bytes 0 0x410000l;
  Bytes.set_int32_be bytes 4 0x400l;
  Bytes.set_int32_be bytes (4 * 26) 0x500l;
  Bytes.set_int32_be bytes (4 * 27) 0x500l;
  put 0x400 (words [0x4bf9; 0x0041; 0x0000; 0x6100; 0x0018; 0x33fc; 0x0001; 0x004a; 0x0000;
                    0x3039; 0x0041; 0x0010; 0x4eb9; 0x0000; 0x0430; 0x4e75]);
  put 0x420 (words [0x3b7c; 0x00ff; 0x0010; 0x4a6d; 0x0010; 0x4e75]);
  put 0x430 (words [0x206d; 0x0020; 0x4e90; 0x4e75]);
  put 0x500 (words [0x4e73]);
  Bytes.to_string bytes

let install_fixture () =
  let rom_dir = path_in_root [ "roms"; "fixture" ] and config_dir = path_in_root [ "games"; "fixture" ] in
  mkdirs rom_dir;
  mkdirs config_dir;
  write_file (Filename.concat rom_dir "main") rom;
  let crc = Image.crc32 { Image.base = 0; bytes = rom } in
  write_file (Filename.concat config_dir "config.toml") (Printf.sprintf {|[game]
id = "fixture"
[rom]
size = 0x1000
interleave = 1
[[rom.lanes]]
file = "main"
size = 0x1000
crc = "%08lx"
sha1 = "%s"
[sprites]
size = 0
[sprites_hi]
size = 0
[tiles]
size = 0
[tiles_hi]
size = 0
[sound]
size = 0
[samples]
size = 0
|} crc (Digestif.SHA1.(to_hex (digest_string rom))))

let fixture = lazy (install_fixture ())

(* ---- JSON access ---- *)

let field key json =
  match Json.member key json with
  | Some value -> value
  | None -> Alcotest.failf "missing %S in %s" key (Json.to_string json)

let items = function Array items -> items | json -> Alcotest.failf "not an array: %s" (Json.to_string json)
let str = function String s -> s | json -> Alcotest.failf "not a string: %s" (Json.to_string json)

let contains ~needle haystack =
  let n = String.length needle and h = String.length haystack in
  let rec scan i = i + n <= h && (String.sub haystack i n = needle || scan (i + 1)) in
  scan 0

let check_contains what ~needle haystack =
  if not (contains ~needle haystack) then Alcotest.failf "%s: expected %S in %S" what needle haystack

(* ---- a minimal MCP client over [f3recomp mcp] ---- *)

type client = {
  pid : int;
  to_server : Unix.file_descr;
  from_server : Unix.file_descr;
  stderr_file : string;
  mutable buffer : string;
  mutable next_id : int;
  mutable inbox : Json.t list; (* messages not consumed yet, oldest first *)
}

let spawn argv =
  let stderr_file = Filename.concat (path_in_root []) (Printf.sprintf "stderr-%d" (Hashtbl.hash (Unix.gettimeofday (), argv))) in
  let stdin_r, stdin_w = Unix.pipe ~cloexec:true () in
  let stdout_r, stdout_w = Unix.pipe ~cloexec:true () in
  let err = Unix.openfile stderr_file [ O_WRONLY; O_CREAT; O_TRUNC ] 0o600 in
  let pid = Unix.create_process (List.hd argv) (Array.of_list argv) stdin_r stdout_w err in
  List.iter Unix.close [ stdin_r; stdout_w; err ];
  { pid; to_server = stdin_w; from_server = stdout_r; stderr_file; buffer = ""; next_id = 0; inbox = [] }

let send_line client line =
  let data = Bytes.of_string (line ^ "\n") in
  let rec go offset =
    if offset < Bytes.length data then go (offset + Unix.write client.to_server data offset (Bytes.length data - offset))
  in
  go 0

let read_line ?(timeout = 20.) client =
  let chunk = Bytes.create 65536 in
  let rec go () =
    match String.index_opt client.buffer '\n' with
    | Some i ->
        let line = String.sub client.buffer 0 i in
        client.buffer <- String.sub client.buffer (i + 1) (String.length client.buffer - i - 1);
        line
    | None -> (
        match Unix.select [ client.from_server ] [] [] timeout with
        | [], _, _ -> Alcotest.failf "timed out waiting for the server; stderr: %s" (read_file client.stderr_file)
        | _ ->
            let n = Unix.read client.from_server chunk 0 (Bytes.length chunk) in
            if n = 0 then Alcotest.failf "server closed its stdout; stderr: %s" (read_file client.stderr_file);
            client.buffer <- client.buffer ^ Bytes.sub_string chunk 0 n;
            go ())
  in
  go ()

let parse line =
  match Json.parse line with Ok json -> json | Error message -> Alcotest.failf "invalid JSON (%s): %s" message line

let send_request client meth params =
  client.next_id <- client.next_id + 1;
  let id = client.next_id in
  send_line client
    (Json.to_string (Object [ "jsonrpc", String "2.0"; "id", Int id; "method", String meth; "params", params ]));
  id

let is_response id message = Json.member "id" message = Some (Int id) && Json.member "method" message = None

(* The response to request [id], waiting at most [timeout] seconds; other messages stay in the inbox. *)
let await_response ?(timeout = 20.) client id =
  let deadline = Unix.gettimeofday () +. timeout in
  let rec wait () =
    match List.find_opt (is_response id) client.inbox with
    | Some message ->
        client.inbox <- List.filter (fun m -> m != message) client.inbox;
        message
    | None ->
        let remaining = deadline -. Unix.gettimeofday () in
        if remaining <= 0. then Alcotest.failf "no response to request %d; stderr: %s" id (read_file client.stderr_file);
        client.inbox <- client.inbox @ [ parse (read_line ~timeout:remaining client) ];
        wait ()
  in
  wait ()

let has_response client id = List.exists (is_response id) client.inbox

let result_of response =
  match Json.member "result" response with
  | Some result -> result
  | None -> Alcotest.failf "request failed: %s" (Json.to_string response)

let request client meth params = result_of (await_response client (send_request client meth params))

let notify client meth =
  send_line client (Json.to_string (Object [ "jsonrpc", String "2.0"; "method", String meth ]))

let initialize client =
  ignore
    (request client "initialize"
       (Object [ "protocolVersion", String "2025-11-25"; "capabilities", Object [];
                 "clientInfo", Object [ "name", String "mcp_bridge_test"; "version", String "0" ] ]));
  notify client "notifications/initialized"

let expect_notification ?(timeout = 20.) client meth =
  let is_meth message = Json.member "method" message = Some (String meth) in
  let deadline = Unix.gettimeofday () +. timeout in
  let rec wait () =
    match List.find_opt is_meth client.inbox with
    | Some message -> client.inbox <- List.filter (fun m -> m != message) client.inbox
    | None ->
        let remaining = deadline -. Unix.gettimeofday () in
        if remaining <= 0. then Alcotest.failf "no %s notification" meth;
        client.inbox <- client.inbox @ [ parse (read_line ~timeout:remaining client) ];
        wait ()
  in
  wait ()

let call_args name arguments = Object [ "name", String name; "arguments", Object arguments ]
let send_call client name arguments = send_request client "tools/call" (call_args name arguments)
let call client name arguments = request client "tools/call" (call_args name arguments)
let is_error result = Json.member "isError" result = Some (Bool true)

let text_of result =
  String.concat "\n"
    (List.filter_map
       (fun block -> if Json.member "type" block = Some (String "text") then Some (str (field "text" block)) else None)
       (items (field "content" result)))

let ok_call client name arguments =
  let result = call client name arguments in
  if is_error result then Alcotest.failf "%s failed: %s" name (text_of result);
  result

let failed_call client name arguments =
  let result = call client name arguments in
  if not (is_error result) then Alcotest.failf "%s should have failed: %s" name (Json.to_string result);
  text_of result

let tool_names client =
  List.map (fun tool -> str (field "name" tool)) (items (field "tools" (request client "tools/list" (Object []))))

let structured result = field "structuredContent" result

let alive pid = try Unix.kill pid 0; true with Unix.Unix_error _ -> false

(* Closes the server's stdin and waits for it to exit: the end of input must also end in-flight calls and
   every child. *)
let finish client =
  Unix.close client.to_server;
  let deadline = Unix.gettimeofday () +. 15. in
  let rec wait () =
    match Unix.waitpid [ Unix.WNOHANG ] client.pid with
    | 0, _ when Unix.gettimeofday () < deadline -> Unix.sleepf 0.05; wait ()
    | 0, _ -> Unix.kill client.pid Sys.sigkill; ignore (Unix.waitpid [] client.pid); Alcotest.fail "server did not exit after EOF"
    | _, status -> status
  in
  let status = wait () in
  Unix.close client.from_server;
  match status with
  | Unix.WEXITED 0 -> ()
  | _ -> Alcotest.failf "server did not exit cleanly; stderr: %s" (read_file client.stderr_file)

let with_server body =
  Lazy.force fixture;
  Unix.putenv "F3RECOMP_MCP_TIMEOUT" "2";
  let client = spawn [ "../../bin/main.exe"; "mcp"; "--root"; Lazy.force root ] in
  Fun.protect ~finally:(fun () -> try Unix.kill client.pid Sys.sigkill with Unix.Unix_error _ -> ())
    (fun () -> initialize client; body client; finish client)

let open_session ?session_id client tool =
  structured
    (ok_call client "session_open"
       ([ "game", String "fixture"; "tool_path", String tool ]
        @ match session_id with Some id -> [ "session_id", String id ] | None -> []))

let sessions client = items (field "sessions" (structured (ok_call client "session_list" [])))

let session_state client id =
  List.find (fun session -> field "session_id" session = String id) (sessions client)

(* ---- tests ---- *)

(* A bridged call that never returns neither blocks ping nor session_close, ends in a timeout error on its
   own, and does not keep the server alive after the client hangs up. *)
let stalled_call () =
  with_server (fun client ->
    ignore (open_session client (fake_runtime "fake-normal" "normal-a"));
    let hang = send_call client "rt_hang" [] in
    let ping = send_request client "ping" (Object []) in
    ignore (result_of (await_response ~timeout:1.0 client ping));
    Alcotest.(check bool) "the stuck call is still pending" false (has_response client hang);
    (* tools/list and session_list also stay live. *)
    Alcotest.(check bool) "tools/list answers" true (List.mem "rt_echo" (tool_names client));
    Alcotest.(check bool) "session_list answers" true (sessions client <> []);
    let timed_out = await_response ~timeout:10. client hang in
    let result = result_of timed_out in
    Alcotest.(check bool) "timeout is an error result" true (is_error result);
    check_contains "timeout message" ~needle:"timed out" (text_of result);
    (* Calls queue behind a stuck one per session, but the session is still usable afterwards. *)
    Alcotest.(check string) "session still serves" "from normal-a echo" (text_of (ok_call client "rt_echo" []));
    (* A second stuck call, ended by session_close while in flight. *)
    let hang2 = send_call client "rt_hang" [] in
    let closed = request client "tools/call" (call_args "session_close" []) in
    Alcotest.(check bool) "session_close works during a stuck call" false (is_error closed);
    let result2 = result_of (await_response ~timeout:10. client hang2) in
    Alcotest.(check bool) "the in-flight call fails once its session is closed" true (is_error result2);
    (* One more stuck call left in flight when the client hangs up: finish checks the server exits. *)
    ignore (open_session client (fake_runtime "fake-normal" "normal-a"));
    ignore (send_call client "rt_hang" []))

let cancelled_call () =
  with_server (fun client ->
    ignore (open_session client (fake_runtime "fake-normal" "normal-a"));
    let hang = send_call client "rt_hang" [] in
    send_line client
      (Json.to_string
         (Object [ "jsonrpc", String "2.0"; "method", String "notifications/cancelled";
                   "params", Object [ "requestId", Int hang ] ]));
    (* A cancelled request gets no response; the session is free again at once. *)
    Alcotest.(check string) "session serves after a cancel" "from normal-a echo" (text_of (ok_call client "rt_echo" []));
    Alcotest.(check bool) "no response to the cancelled call" false (has_response client hang))

let handshake_timeout () =
  with_server (fun client ->
    let pid_file = path_in_root [ "stall.pid" ] in
    let tool = fake_runtime ~pid_file "fake-stall" "stall-handshake" in
    let started = Unix.gettimeofday () in
    let message = failed_call client "session_open" [ "game", String "fixture"; "tool_path", String tool ] in
    check_contains "handshake timeout" ~needle:"timed out" message;
    Alcotest.(check bool) "bounded by the timeout, not forever" true (Unix.gettimeofday () -. started < 10.);
    let pid = int_of_string (String.trim (read_file pid_file)) in
    Alcotest.(check bool) "the stalled child was cleaned up" false (alive pid);
    Alcotest.(check bool) "no session was registered" true (sessions client = []);
    (* The server is still fine. *)
    Alcotest.(check string) "ping" "{}" (Json.to_string (request client "ping" (Object []))))

(* --game discovers the runtime's tools beside the server: a stalled child delays nothing, and the end of
   input does not wait for it. *)
let pinned_game_stalls () =
  Lazy.force fixture;
  Unix.putenv "F3RECOMP_MCP_TIMEOUT" "30";
  let build = path_in_root [ "build"; "fixture" ] in
  mkdirs build;
  let tool = Filename.concat build "f3rt-tool" in
  write_file tool (Printf.sprintf "#!/bin/sh\nexec %s fake-child stall-handshake \"$@\"\n" self);
  Unix.chmod tool 0o755;
  let client = spawn [ "../../bin/main.exe"; "mcp"; "--root"; Lazy.force root; "--game"; "fixture" ] in
  Fun.protect ~finally:(fun () -> try Unix.kill client.pid Sys.sigkill with Unix.Unix_error _ -> ())
    (fun () ->
       let started = Unix.gettimeofday () in
       initialize client;
       ignore (request client "ping" (Object []));
       Alcotest.(check bool) "initialize does not wait for discovery" true (Unix.gettimeofday () -. started < 5.);
       let ended = Unix.gettimeofday () in
       finish client;
       Alcotest.(check bool) "EOF does not wait out the 30s handshake" true (Unix.gettimeofday () -. ended < 10.))

let bad_protocol () =
  with_server (fun client ->
    let message =
      failed_call client "session_open"
        [ "game", String "fixture"; "tool_path", String (fake_runtime "fake-badproto" "bad-protocol") ]
    in
    check_contains "protocol version" ~needle:"protocolVersion" message;
    check_contains "names the bad version" ~needle:"1999-01-01" message)

(* The default session is the newest live one; explicit ids are checked against their own tools. *)
let failover () =
  with_server (fun client ->
    ignore (open_session ~session_id:"a" client (fake_runtime "fake-a" "normal-a"));
    ignore (open_session ~session_id:"b" client (fake_runtime "fake-b" "normal-b"));
    Alcotest.(check bool) "b is active" true (field "active" (session_state client "b") = Bool true);
    Alcotest.(check bool) "a is not" true (field "active" (session_state client "a") = Bool false);
    Alcotest.(check string) "default goes to the newest" "from normal-b echo" (text_of (ok_call client "rt_echo" []));
    let names = tool_names client in
    Alcotest.(check bool) "b's tools are advertised" true (List.mem "rt_only_b" names && not (List.mem "rt_only_a" names));
    (* Another session's tool is reachable with its id, and refused without it. *)
    Alcotest.(check string) "explicit session, own tool" "from normal-a only_a"
      (text_of (ok_call client "rt_only_a" [ "session_id", String "a" ]));
    check_contains "not in b" ~needle:"not available" (failed_call client "rt_only_a" []);
    (* Kill b's child. *)
    ignore (failed_call client "rt_die" []);
    expect_notification client "notifications/tools/list_changed";
    let b = session_state client "b" and a = session_state client "a" in
    Alcotest.(check bool) "b is dead" true (field "alive" b = Bool false);
    Alcotest.(check bool) "b is not active" true (field "active" b = Bool false);
    Alcotest.(check bool) "a took over" true (field "active" a = Bool true);
    Alcotest.(check string) "default routing skips the dead session" "from normal-a echo"
      (text_of (ok_call client "rt_echo" []));
    let names = tool_names client in
    Alcotest.(check bool) "advertised tools follow" true (List.mem "rt_only_a" names && not (List.mem "rt_only_b" names));
    check_contains "explicit dead session" ~needle:"exited"
      (failed_call client "rt_echo" [ "session_id", String "b" ]);
    (* Closing the dead one with no id would pick a; close it by id. *)
    ignore (ok_call client "session_close" [ "session_id", String "b" ]);
    ignore (ok_call client "session_close" []);
    Alcotest.(check bool) "all closed" true (sessions client = []))

let all_sessions_dead () =
  with_server (fun client ->
    ignore (open_session client (fake_runtime "fake-a" "normal-a"));
    ignore (failed_call client "rt_die" []);
    expect_notification client "notifications/tools/list_changed";
    check_contains "no live session" ~needle:"no live runtime session" (failed_call client "rt_echo" []);
    Alcotest.(check bool) "no runtime tools advertised" false
      (List.exists (String.starts_with ~prefix:"rt_") (tool_names client));
    (* session_close with no id clears the dead one. *)
    ignore (ok_call client "session_close" []);
    Alcotest.(check bool) "closed" true (sessions client = []))

let read_forwarded client address =
  let result = call client "rt_read" [ "addr", String address ] in
  (is_error result, text_of result)

let a5_arithmetic () =
  with_server (fun client ->
    ignore (open_session client (fake_runtime "fake-a" "normal-a"));
    let is_err, forwarded = read_forwarded client "a5+0x10" in
    Alcotest.(check bool) "in range" false is_err;
    check_contains "resolved against a5" ~needle:(Printf.sprintf "%d" (0x410000 + 0x10)) forwarded;
    let is_err, forwarded = read_forwarded client "A5 - 0x10" in
    Alcotest.(check bool) "spaces and case" false is_err;
    check_contains "subtracted" ~needle:(Printf.sprintf "%d" (0x410000 - 0x10)) forwarded;
    let reject address =
      let is_err, message = read_forwarded client address in
      Alcotest.(check bool) (address ^ " is rejected") true is_err;
      check_contains (address ^ " message") ~needle:"32-bit" message
    in
    reject "a5-0x500000" (* below zero *);
    reject "a5+0xffff0000" (* past 0xffffffff *);
    let is_err, message = read_forwarded client "a5+0x100000000" in
    Alcotest.(check bool) "offset past 32 bits is rejected" true is_err;
    check_contains "offset message" ~needle:"0x100000000" message;
    (* Nothing out of range reached the child; an absolute address is untouched. *)
    let is_err, forwarded = read_forwarded client "0x1234" in
    Alcotest.(check bool) "plain addresses pass" false is_err;
    check_contains "verbatim" ~needle:"0x1234" forwarded)

(* rt_init changes what a5 was inferred from. *)
let rt_init_refreshes_a5 () =
  with_server (fun client ->
    ignore (open_session client (fake_runtime "fake-a" "normal-a"));
    let empty = path_in_root [ "roms"; "empty" ] in
    mkdirs empty;
    let _, forwarded = read_forwarded client "a5+0x10" in
    check_contains "a5 known" ~needle:"4259856" forwarded;
    (* A ROM directory without the game's ROMs: a5 cannot be resolved there, and must not fall back to the old one. *)
    let init = ok_call client "rt_init" [ "rom_dir", String empty ] in
    check_contains "init was forwarded" ~needle:"empty" (text_of init);
    let is_err, message = read_forwarded client "a5+0x10" in
    Alcotest.(check bool) "stale a5 is not reused" true is_err;
    check_contains "says a5 cannot be resolved" ~needle:"cannot resolve an a5-relative address" message;
    check_contains "names the new rom_dir" ~needle:"empty" message;
    (* Back to the real directory: resolvable again. *)
    ignore (ok_call client "rt_init" [ "rom_dir", String (path_in_root [ "roms"; "fixture" ]) ]);
    let is_err, _ = read_forwarded client "a5+0x10" in
    Alcotest.(check bool) "resolvable again" false is_err;
    (* A different ROM set: a5 from this config no longer applies. *)
    ignore (ok_call client "rt_init" [ "set", String "othersetname" ]);
    let is_err, message = read_forwarded client "a5+0x10" in
    Alcotest.(check bool) "set change blocks a5" true is_err;
    check_contains "names the set" ~needle:"othersetname" message;
    (* init without a rom_dir keeps the session's ROM directory. *)
    let init = ok_call client "rt_init" [ "set", String "fixture" ] in
    check_contains "rom_dir injected" ~needle:"fixture" (text_of init);
    let is_err, _ = read_forwarded client "a5+0x10" in
    Alcotest.(check bool) "original set resolves again" false is_err)

let slashless_tool_path () =
  with_server (fun client ->
    ignore (fake_runtime "f3fake-tool" "normal-a");
    (* Not on PATH (the server's cwd has no such file either): refused, not looked up in the cwd. *)
    let message = failed_call client "session_open" [ "game", String "fixture"; "tool_path", String "f3fake-tool" ] in
    check_contains "slashless name needs PATH" ~needle:"not found on PATH" message;
    (* A relative path with a slash is taken against the working directory and shown absolute. *)
    let relative = Filename.concat (Filename.concat (Filename.basename (Lazy.force root)) "bin") "f3fake-tool" in
    let opened = open_session client relative in
    let binary = str (field "binary" opened) in
    Alcotest.(check bool) "binary is resolved" true (String.length binary > 0);
    Alcotest.(check bool) "session works" false (is_error (call client "rt_echo" []));
    ignore (ok_call client "session_close" []))

let slashless_found_on_path () =
  ignore (fake_runtime "f3fake-path-tool" "normal-a");
  Lazy.force fixture |> ignore;
  Unix.putenv "F3RECOMP_MCP_TIMEOUT" "2";
  let old_path = Sys.getenv_opt "PATH" in
  Unix.putenv "PATH" (path_in_root [ "bin" ] ^ ":" ^ Option.value old_path ~default:"/usr/bin:/bin");
  let client =
    Fun.protect
      ~finally:(fun () -> Unix.putenv "PATH" (Option.value old_path ~default:"/usr/bin:/bin"))
      (fun () -> spawn [ "../../bin/main.exe"; "mcp"; "--root"; Lazy.force root ])
  in
  Fun.protect ~finally:(fun () -> try Unix.kill client.pid Sys.sigkill with Unix.Unix_error _ -> ())
    (fun () ->
       initialize client;
       let opened = open_session client "f3fake-path-tool" in
       Alcotest.(check string) "resolved through PATH to the absolute file"
         (path_in_root [ "bin"; "f3fake-path-tool" ])
         (let binary = str (field "binary" opened) in
          if Filename.is_relative binary then path_in_root [ binary ] else binary);
       Alcotest.(check string) "and it spawns" "from normal-a echo" (text_of (ok_call client "rt_echo" []));
       ignore (ok_call client "session_close" []);
       finish client)

(* ---- one-shot runners ---- *)

let one_shot_args ?(extra = []) tool = [ "game", String "fixture"; "tool_path", String tool ] @ extra

let one_shot_rejects_bad_counts () =
  with_server (fun client ->
    let marker = path_in_root [ "ran-marker" ] in
    let tool = script "marker-tool" (Printf.sprintf "touch %s\necho '{}'" marker) in
    List.iter
      (fun (what, args, needle) ->
        let message = failed_call client "gameplay" (one_shot_args ~extra:args tool) in
        check_contains what ~needle message;
        Alcotest.(check bool) (what ^ ": nothing was spawned") false (Sys.file_exists marker))
      [ ("negative frames", [ "frames", Int (-1) ], "frames");
        ("huge frames", [ "frames", Int 0x80000000 ], "frames");
        ("negative seed", [ "seed", Int (-5) ], "seed");
        ("negative frames via args", [ "args", Array [ String "--frames"; String "-3" ] ], "--frames") ];
    ignore (ok_call client "gameplay" (one_shot_args ~extra:[ "frames", Int 10 ] tool));
    Alcotest.(check bool) "valid frames run" true (Sys.file_exists marker))

let one_shot_signal () =
  with_server (fun client ->
    let tool = script "killed-tool" "kill -KILL $$" in
    let result = call client "gameplay" (one_shot_args tool) in
    Alcotest.(check bool) "a killed run is an error" true (is_error result);
    let body = structured result in
    Alcotest.(check bool) "exit_code is null" true (field "exit_code" body = Null);
    let signal = field "signal" body in
    Alcotest.(check bool) "POSIX number" true (field "number" signal = Int 9);
    Alcotest.(check bool) "POSIX name" true (field "name" signal = String "SIGKILL");
    let tool = script "term-tool" "kill -TERM $$" in
    let signal = field "signal" (structured (call client "motion" (one_shot_args tool))) in
    Alcotest.(check bool) "SIGTERM" true (field "number" signal = Int 15);
    let tool = script "exit-tool" "echo '{\"ok\": true}'; exit 3" in
    let result = call client "sprite_check" (one_shot_args tool) in
    Alcotest.(check bool) "nonzero exit is an error" true (is_error result);
    Alcotest.(check bool) "exit code kept" true (field "exit_code" (structured result) = Int 3);
    Alcotest.(check bool) "no signal field" true (Json.member "signal" (structured result) = None))

let one_shot_stdin_is_isolated () =
  with_server (fun client ->
    let tool = script "stdin-tool" "n=$(cat | wc -c)\necho \"{\\\"stdin_bytes\\\": $n}\"" in
    (* If the child inherited the MCP stream, cat would wait for the client; EOF returns at once. *)
    let result = ok_call client "gameplay" (one_shot_args tool) in
    Alcotest.(check bool) "stdin is empty" true (field "stdin_bytes" (field "result" (structured result)) = Int 0))

let one_shot_output_cap () =
  with_server (fun client ->
    let tool = script "flood-tool" "yes | head -c 40000000" in
    let message = failed_call client "gameplay" (one_shot_args tool) in
    check_contains "cap message" ~needle:"more than" message)

let one_shot_requires_json () =
  with_server (fun client ->
    let tool = script "plain-tool" "echo not json" in
    let result = call client "gameplay" (one_shot_args tool) in
    Alcotest.(check bool) "non-JSON stdout is an error" true (is_error result);
    Alcotest.(check bool) "stdout kept for diagnosis" true
      (contains ~needle:"not json" (Json.to_string (structured result)));
    let tool = script "silent-tool" "exit 0" in
    Alcotest.(check bool) "empty stdout is an error" true (is_error (call client "gameplay" (one_shot_args tool)));
    let tool = script "json-tool" "echo '{\"frames\": 3}'" in
    let result = ok_call client "gameplay" (one_shot_args tool) in
    Alcotest.(check bool) "JSON passes" true (field "frames" (field "result" (structured result)) = Int 3))

let () =
  Alcotest.run "mcp_bridge"
    [ ( "concurrency",
        [ Alcotest.test_case "stalled call keeps the server responsive" `Slow stalled_call;
          Alcotest.test_case "cancelled call" `Quick cancelled_call;
          Alcotest.test_case "handshake timeout" `Slow handshake_timeout;
          Alcotest.test_case "child protocol version" `Quick bad_protocol;
          Alcotest.test_case "--game discovery stalls" `Quick pinned_game_stalls ] );
      ( "sessions",
        [ Alcotest.test_case "failover when the newest child dies" `Quick failover;
          Alcotest.test_case "all sessions dead" `Quick all_sessions_dead ] );
      ( "a5",
        [ Alcotest.test_case "32-bit arithmetic" `Quick a5_arithmetic;
          Alcotest.test_case "rt_init refreshes a5" `Quick rt_init_refreshes_a5 ] );
      ( "tool_path",
        [ Alcotest.test_case "slashless name is not a cwd file" `Quick slashless_tool_path;
          Alcotest.test_case "slashless name found on PATH" `Quick slashless_found_on_path ] );
      ( "one-shot",
        [ Alcotest.test_case "bad counts rejected before spawning" `Quick one_shot_rejects_bad_counts;
          Alcotest.test_case "signals are reported" `Quick one_shot_signal;
          Alcotest.test_case "stdin is isolated" `Quick one_shot_stdin_is_isolated;
          Alcotest.test_case "output is capped" `Quick one_shot_output_cap;
          Alcotest.test_case "stdout must be JSON" `Quick one_shot_requires_json ] ) ]
