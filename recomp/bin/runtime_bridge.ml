(* The runtime side of [f3recomp mcp]: an MCP client that spawns [build/<game>/f3rt-tool mcp] sessions and
   bridges their tools, plus the one-shot f3rt-tool commands.

   A session is one child process speaking MCP over its stdin/stdout. While at least one session is open
   (or [--game] pinned the tool list) the child's tools are advertised here as [rt_<name>], each with an
   optional [session_id] argument. Every forwarded call first re-stats the child's binary and refuses to
   forward when it changed on disk since the session opened. *)

open Tool_support

let client_version = "1.0.0"
let protocol_version = "2025-11-25"
let tool_prefix = "rt_"

type fingerprint = { mtime : float; ino : int; size : int }

let fingerprint path =
  match Unix.stat path with
  | stat -> Some { mtime = stat.Unix.st_mtime; ino = stat.st_ino; size = stat.st_size }
  | exception Unix.Unix_error _ -> None

type binary_status = Fresh | Stale | Missing

type conn = {
  client : Mcp.Client.t;
  transport : Mcp_eio.Stdio.t;
  child_stdin : [ Eio.Flow.sink_ty | Eio.Resource.close_ty ] Eio.Resource.t;
  child_stdout : [ Eio.Flow.source_ty | Eio.Resource.close_ty ] Eio.Resource.t;
  process : [ `Generic | `Unix ] Eio.Process.ty Eio.Resource.t;
  dead : unit Eio.Promise.t; (* resolved when the child's stdout reaches EOF *)
}

type session = {
  id : string;
  game : string;
  binary : string;
  opened : fingerprint option; (* stat of [binary] taken before the child was spawned *)
  config : string;
  rom_dir : string;
  conn : conn;
  tools : Mcp.Types.Tool.t list; (* the runtime's own tool list, from its tools/list *)
  mutable a5 : int option;
  mutable closed : bool;
}

type t = {
  root : string;
  env : Eio_unix.Stdenv.base;
  sw : Eio.Switch.t;
  api : F3recomp.Api.session;
  sessions : (string, session) Hashtbl.t;
  mutable order : string list; (* open session ids, most recently opened first *)
  mutable pinned : Mcp.Types.Tool.t list; (* runtime tools kept advertised by --game *)
}

let create ~root ~env ~sw ~api = { root; env; sw; api; sessions = Hashtbl.create 4; order = []; pinned = [] }

(* ---- paths ---- *)

let relative t path =
  let prefix = t.root ^ "/" in
  if String.starts_with ~prefix path then String.sub path (String.length prefix) (String.length path - String.length prefix)
  else path

let default_binary t game = Filename.concat t.root (Printf.sprintf "build/%s/f3rt-tool" game)

let resolve_binary t ~game ~tool_path =
  let binary = Option.value tool_path ~default:(default_binary t game) in
  if not (Sys.file_exists binary) then
    fail "%s does not exist. Build it first: nix develop -c cmake --build build/%s --target f3rt-tool (configure with f3-compile %s if build/%s is missing)."
      (relative t binary) game game game;
  binary

let binary_status session =
  match (session.opened, fingerprint session.binary) with
  | _, None -> Missing
  | Some before, Some now when before = now -> Fresh
  | None, Some _ -> Fresh
  | Some _, Some _ -> Stale

let status_name = function Fresh -> "fresh" | Stale -> "stale" | Missing -> "missing"

let stale_message t session status =
  Printf.sprintf
    "%s %s on disk since session %S was opened, so this session's runtime may no longer match the build. Runtime calls \
     are refused: call session_close, then session_open again."
    (relative t session.binary)
    (match status with Missing -> "is missing (it was removed or is being rebuilt)" | _ -> "changed")
    session.id

(* ---- the child connection ---- *)

let send conn outgoing = Mcp_eio.Stdio.send conn.transport (Mcp.Protocol.outgoing_to_message outgoing)

(* One request to the child: [make] builds the outgoing message and registers the callback. Fails fast
   when the child is gone, and when it exits while the call is pending. *)
let rpc (type a) conn (make : ((a, string) result -> unit) -> Mcp.Protocol.outgoing_message) :
    (a, string) result =
  if Eio.Promise.is_resolved conn.dead then Error "the runtime process has exited"
  else
    let promise, resolver = Eio.Promise.create () in
    let outgoing =
      make (fun result -> if not (Eio.Promise.is_resolved promise) then Eio.Promise.resolve resolver result)
    in
    match send conn outgoing with
    | exception exn -> Error ("cannot reach the runtime process: " ^ Printexc.to_string exn)
    | () ->
        Eio.Fiber.first
          (fun () -> Eio.Promise.await promise)
          (fun () ->
            Eio.Promise.await conn.dead;
            (* A response may have arrived in the same breath as the exit. *)
            if Eio.Promise.is_resolved promise then Eio.Promise.await promise
            else Error "the runtime process exited while handling the call")

let read_loop ~clock client transport =
  let failures = ref 0 in
  let rec loop () =
    match Mcp_eio.Stdio.recv transport ~clock () with
    | None -> ()
    | Some packet ->
        failures := 0;
        (match Mcp.Protocol.parse_message packet with
         | Ok message -> (
             match Mcp.Client.handle_message client message with
             | Some response -> Mcp_eio.Stdio.send transport (Mcp.Protocol.outgoing_to_message response)
             | None -> ())
         | Error message -> Logs.warn (fun m -> m "runtime sent an unreadable message: %s" message));
        loop ()
    | exception Failure message ->
        incr failures;
        Logs.warn (fun m -> m "runtime sent a malformed line: %s" message);
        if !failures < 3 then loop ()
  in
  loop ()

let close_pipe pipe = try Eio.Resource.close pipe with _ -> ()

let with_timeout t seconds body =
  match Eio.Time.with_timeout_exn (Eio.Stdenv.clock t.env) seconds body with
  | value -> Some value
  | exception Eio.Time.Timeout -> None

(* Ends the child: closing its stdin is EOF for the MCP transport, which tears down the machine and exits;
   a child that does not exit in time is killed. *)
let shutdown_child t (conn : conn) =
  let process = conn.process and dead = conn.dead in
  close_pipe conn.child_stdin;
  let exited = with_timeout t 10.0 (fun () -> Eio.Process.await process) in
  let status =
    match exited with
    | Some status -> status
    | None ->
        (try Eio.Process.signal process Sys.sigkill with _ -> ());
        Eio.Process.await process
  in
  ignore (with_timeout t 2.0 (fun () -> Eio.Promise.await dead));
  close_pipe conn.child_stdout;
  status

let exit_text = function
  | `Exited code -> Printf.sprintf "exited with status %d" code
  | `Signaled signal -> Printf.sprintf "killed by signal %d" signal

let spawn t ~binary ~args : conn =
  let mgr = Eio.Stdenv.process_mgr t.env in
  let stdin_r, stdin_w = Eio.Process.pipe ~sw:t.sw mgr in
  let stdout_r, stdout_w = Eio.Process.pipe ~sw:t.sw mgr in
  let process =
    try
      Eio.Process.spawn ~sw:t.sw mgr ~stdin:stdin_r ~stdout:stdout_w ~stderr:(Eio.Stdenv.stderr t.env) (binary :: args)
    with exn ->
      close_pipe stdin_r; close_pipe stdin_w; close_pipe stdout_r; close_pipe stdout_w;
      raise exn
  in
  close_pipe stdin_r;
  close_pipe stdout_w;
  let transport = Mcp_eio.Stdio.create ~stdin:stdout_r ~stdout:stdin_w in
  let client =
    Mcp.Client.create ~notification_handler:Mcp.Client.default_notification_handler
      ~client_info:{ name = "f3recomp"; version = client_version }
      ~client_capabilities:{ experimental = None; sampling = None; elicitation = None; roots = None }
      ()
  in
  let dead, dead_resolver = Eio.Promise.create () in
  let clock = Eio.Stdenv.clock t.env in
  Eio.Fiber.fork ~sw:t.sw (fun () ->
      Fun.protect
        ~finally:(fun () -> Eio.Promise.resolve dead_resolver ())
        (fun () -> try read_loop ~clock client transport with Eio.Cancel.Cancelled _ as exn -> raise exn | _ -> ()));
  { client; transport; child_stdin = stdin_w; child_stdout = stdout_r; process; dead }

let handshake (conn : conn) =
  match rpc conn (fun callback -> Mcp.Client.initialize conn.client ~protocol_version callback) with
  | Error message -> fail "the runtime did not initialise: %s" message
  | Ok _ -> (
      let initialized =
        Mcp.Client.send_notification conn.client (Mcp.Notification.Initialized { meta = None })
      in
      send conn initialized;
      match rpc conn (fun callback -> Mcp.Client.tools_list conn.client callback) with
      | Error message -> fail "the runtime did not list its tools: %s" message
      | Ok listing -> listing.tools)

(* ---- bridged tool descriptors ---- *)

let session_id_prop =
  string_prop
    "Runtime session to use (see session_list). Defaults to the most recently opened session."

let with_session_id (schema : Yojson.Safe.t) : Yojson.Safe.t =
  match schema with
  | `Assoc fields ->
      let props = match List.assoc_opt "properties" fields with Some (`Assoc props) -> props | _ -> [] in
      let fields = List.remove_assoc "properties" fields in
      `Assoc (fields @ [ ("properties", `Assoc (props @ [ ("session_id", session_id_prop) ])) ])
  | other -> other

let a5_note =
  " Addresses may also be written a5+0x... or a5-0x... : they are resolved with the game's inferred a5 base \
   (see the inspect tool) before the call is forwarded."

let bridged_descriptor (tool : Mcp.Types.Tool.t) : Mcp.Types.Tool.t =
  let schema = with_session_id tool.input_schema in
  let description =
    let base = Option.value tool.description ~default:tool.name in
    let base = if tool.name = "read" || tool.name = "write" then base ^ a5_note else base in
    Printf.sprintf "[runtime session] %s" base
  in
  { tool with name = tool_prefix ^ tool.name; input_schema = schema; description = Some description }

(* The runtime tools currently advertised: those of the most recently opened live session, else the
   --game pinned list. *)
let runtime_tools t =
  let raw =
    match t.order with
    | id :: _ -> (Hashtbl.find t.sessions id).tools
    | [] -> t.pinned
  in
  List.map bridged_descriptor raw

(* ---- sessions ---- *)

let find_session t = function
  | Some id -> (
      match Hashtbl.find_opt t.sessions id with
      | Some session -> session
      | None ->
          fail "no open session %S (open: %s)" id
            (if t.order = [] then "none" else String.concat ", " t.order))
  | None -> (
      match t.order with
      | id :: _ -> Hashtbl.find t.sessions id
      | [] -> fail "no runtime session is open: call session_open first")

let fresh_id t game =
  let rec pick n =
    let id = if n = 1 then game else Printf.sprintf "%s-%d" game n in
    if Hashtbl.mem t.sessions id then pick (n + 1) else id
  in
  pick 1

let close_session t session =
  if not session.closed then begin
    session.closed <- true;
    Hashtbl.remove t.sessions session.id;
    t.order <- List.filter (fun id -> id <> session.id) t.order
  end;
  shutdown_child t session.conn

let init_params args =
  List.filter_map
    (fun key -> match member key args with Some value -> Some (key, value) | None -> None)
    [ "set"; "game_video"; "sprite_writers"; "video_diff"; "eeprom" ]

(* Spawns the runtime, handshakes, lists its tools and runs its [init]. *)
let open_session t args =
  let game = require_string "game" args in
  Static_tools.safe_game game;
  let tool_path = string_arg "tool_path" args in
  let binary = resolve_binary t ~game ~tool_path in
  let rom_dir = Option.value (string_arg "rom_dir" args) ~default:(Filename.concat t.root ("roms/" ^ game)) in
  if not (Sys.file_exists rom_dir) then fail "ROM directory %s does not exist (pass rom_dir)" rom_dir;
  let id = match string_arg "session_id" args with Some id -> id | None -> fresh_id t game in
  if Hashtbl.mem t.sessions id then fail "session %S is already open" id;
  (* The fingerprint is taken before the spawn: a rebuild after this point is what "stale" means. *)
  let opened = fingerprint binary in
  let conn = spawn t ~binary ~args:[ "mcp"; "--no-init" ] in
  let abort () = ignore (shutdown_child t conn) in
  match
    let tools = handshake conn in
    let init_args = `Assoc (("rom_dir", `String rom_dir) :: init_params args) in
    let init_result =
      rpc conn (fun callback -> Mcp.Client.tools_call conn.client ~name:"init" ~arguments:init_args callback)
    in
    (tools, init_result)
  with
  | exception exn -> abort (); raise exn
  | _, Error message -> abort (); fail "the runtime's init failed: %s" message
  | _, Ok init when init.is_error = Some true ->
      abort ();
      let text =
        String.concat "\n"
          (List.filter_map (function Mcp.Types.Content.Text { text; _ } -> Some text | _ -> None) init.content)
      in
      fail "the runtime's init failed: %s" text
  | tools, Ok init ->
      let session =
        { id; game; binary; opened; config = Filename.concat t.root (Printf.sprintf "games/%s/config.toml" game);
          rom_dir; conn; tools; a5 = None; closed = false }
      in
      Hashtbl.replace t.sessions id session;
      t.order <- id :: t.order;
      (session, init.structured_content)

(* ---- addresses relative to a5 ---- *)

let session_a5 t session =
  match session.a5 with
  | Some a5 -> a5
  | None ->
      if not (Sys.file_exists session.config) then
        fail "cannot resolve an a5-relative address: %s does not exist" (relative t session.config);
      let result =
        F3recomp.Api.inspect t.api ~config:session.config ~rom_dir:session.rom_dir ~routines:false ()
      in
      let a5 =
        match F3recomp.Json.member "a5" result with
        | Some (F3recomp.Json.Int a5) -> a5
        | _ -> fail "cannot resolve an a5-relative address: no a5 base was found in the ROM (pass an absolute address)"
      in
      session.a5 <- Some a5;
      a5

(* "a5-0x60ac" and "a5+0x10" (case and spaces ignored) -> the absolute address; anything else is untouched. *)
let resolve_address t session = function
  | `String text -> (
      let compact = String.concat "" (String.split_on_char ' ' (String.lowercase_ascii text)) in
      if String.length compact > 3 && String.sub compact 0 2 = "a5" && (compact.[2] = '+' || compact.[2] = '-') then
        let offset = F3recomp.Inspect.parse_address (String.sub compact 3 (String.length compact - 3)) in
        let a5 = session_a5 t session in
        `Int ((if compact.[2] = '-' then a5 - offset else a5 + offset) land 0xffffffff)
      else `String text)
  | other -> other

(* ---- calls ---- *)

let forwarded_arguments t session ~tool args =
  let args = List.filter (fun (key, _) -> key <> "session_id") (fields args) in
  let args =
    if tool = "read" || tool = "write" then
      List.map (fun (key, value) -> if key = "addr" then (key, resolve_address t session value) else (key, value)) args
    else args
  in
  `Assoc args

(* Forwards [rt_<tool>] to the child verbatim (content blocks, structuredContent and isError pass through). *)
let call_runtime t ~tool args =
  protect (fun () ->
      let session = find_session t (string_arg "session_id" args) in
      match binary_status session with
      | (Stale | Missing) as status -> error_result (stale_message t session status)
      | Fresh -> (
          let arguments = forwarded_arguments t session ~tool args in
          match
            rpc session.conn (fun callback -> Mcp.Client.tools_call session.conn.client ~name:tool ~arguments callback)
          with
          | Ok result -> result
          | Error message -> error_result (Printf.sprintf "runtime session %S: %s" session.id message)))

(* ---- one-shot f3rt-tool commands ---- *)

let run_one_shot t ~command args =
  protect (fun () ->
      let game = require_string "game" args in
      Static_tools.safe_game game;
      let binary = resolve_binary t ~game ~tool_path:(string_arg "tool_path" args) in
      let rom_dir = Option.value (string_arg "rom_dir" args) ~default:(Filename.concat t.root ("roms/" ^ game)) in
      let flag name = function Some value -> [ name; value ] | None -> [] in
      let extra =
        match member "args" args with
        | None -> []
        | Some (`List items) ->
            List.map (function `String s -> s | _ -> fail "\"args\" must be an array of strings") items
        | Some _ -> fail "\"args\" must be an array of strings"
      in
      let argv =
        [ binary; command; "--json"; "--rom-dir"; rom_dir ]
        @ flag "--set" (string_arg "set" args)
        @ flag "--frames" (Option.map string_of_int (int_arg "frames" args))
        @ flag "--seed" (Option.map string_of_int (int_arg "seed" args))
        @ extra
      in
      let mgr = Eio.Stdenv.process_mgr t.env in
      Eio.Switch.run @@ fun sw ->
      let stdout_r, stdout_w = Eio.Process.pipe ~sw mgr in
      let process = Eio.Process.spawn ~sw mgr ~stdout:stdout_w ~stderr:(Eio.Stdenv.stderr t.env) argv in
      close_pipe stdout_w;
      let output = Eio.Buf_read.parse_exn Eio.Buf_read.take_all stdout_r ~max_size:max_int in
      close_pipe stdout_r;
      let status = Eio.Process.await process in
      let code = match status with `Exited code -> code | `Signaled signal -> 128 + signal in
      let parsed = try Some (Yojson.Safe.from_string output) with _ -> None in
      let result = `Assoc
          ([ ("command", `String command); ("exit_code", `Int code) ]
           @ (match parsed with Some json -> [ ("result", json) ] | None -> [ ("stdout", `String output) ]))
      in
      let tool_result = structured result in
      if code <> 0 then { tool_result with is_error = Some true } else tool_result)
