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

(* Protocol versions the runtime child may answer [initialize] with. *)
let supported_protocol_versions = [ "2025-11-25"; "2025-06-18"; "2025-03-26"; "2024-11-05" ]

(* Seconds a child may take before the bridge gives up on it: the handshake ([initialize], [tools/list],
   [init]) and ordinary calls share [call]; [step] runs frames and gets longer. [F3RECOMP_MCP_TIMEOUT]
   overrides [call] (and scales [step] to four times it). *)
type timeouts = { handshake : float; call : float; step : float }

let default_timeouts = { handshake = 30.0; call = 30.0; step = 120.0 }

let timeouts_from_env () =
  match Option.bind (Sys.getenv_opt "F3RECOMP_MCP_TIMEOUT") float_of_string_opt with
  | Some seconds when seconds > 0.0 -> { handshake = seconds; call = seconds; step = 4.0 *. seconds }
  | _ -> default_timeouts

(* Everything [stat] can say about a file that changes when it is rebuilt or replaced. *)
type fingerprint = { dev : int; ino : int; size : int; mtime : float; ctime : float }

let fingerprint path =
  match Unix.stat path with
  | stat ->
      Some
        { dev = stat.Unix.st_dev; ino = stat.st_ino; size = stat.st_size; mtime = stat.st_mtime;
          ctime = stat.st_ctime }
  | exception Unix.Unix_error _ -> None

type binary_status = Fresh | Stale | Missing

type conn = {
  client : Mcp.Client.t;
  transport : Mcp_eio.Stdio.t;
  clock : float Eio.Time.clock_ty Eio.Resource.t;
  child_stdin : [ Eio.Flow.sink_ty | Eio.Resource.close_ty ] Eio.Resource.t;
  child_stdout : [ Eio.Flow.source_ty | Eio.Resource.close_ty ] Eio.Resource.t;
  process : [ `Generic | `Unix ] Eio.Process.ty Eio.Resource.t;
  dead : unit Eio.Promise.t; (* resolved when the child's stdout reaches EOF *)
}

type session = {
  id : string;
  game : string;
  binary : string; (* absolute: the path that was stat'ed and spawned *)
  opened : fingerprint; (* stat of [binary] taken before the child was spawned *)
  config : string;
  opened_set : string; (* ROM set the session was opened with *)
  mutable rom_dir : string;
  mutable current_set : string; (* ROM set of the latest successful init *)
  conn : conn;
  tools : Mcp.Types.Tool.t list; (* the runtime's own tool list, from its tools/list *)
  lock : Eio.Mutex.t; (* one call at a time per child *)
  mutable a5 : int option;
  mutable closed : bool;
}

type t = {
  root : string;
  env : Eio_unix.Stdenv.base;
  sw : Eio.Switch.t;
  api : F3recomp.Api.session;
  timeouts : timeouts;
  sessions : (string, session) Hashtbl.t;
  reserved : (string, unit) Hashtbl.t; (* ids of sessions still opening *)
  mutable order : string list; (* open session ids, most recently opened first *)
  mutable pinned : Mcp.Types.Tool.t list; (* runtime tools kept advertised by --game *)
  mutable advertised : Mcp.Types.Tool.t list; (* raw tools behind the advertised rt_* list *)
  mutable on_tools_changed : unit -> unit; (* a session died on its own: the rt_* list may have changed *)
}

let create ~root ~env ~sw ~api =
  { root; env; sw; api; timeouts = timeouts_from_env (); sessions = Hashtbl.create 4; reserved = Hashtbl.create 4;
    order = []; pinned = []; advertised = []; on_tools_changed = ignore }

(* ---- paths ---- *)

let relative t path =
  let prefix = t.root ^ "/" in
  if String.starts_with ~prefix path then String.sub path (String.length prefix) (String.length path - String.length prefix)
  else path

let default_binary t game = Filename.concat t.root (Printf.sprintf "build/%s/f3rt-tool" game)

let absolute path = if Filename.is_relative path then Filename.concat (Sys.getcwd ()) path else path

let is_executable_file path =
  match Unix.stat path with
  | { Unix.st_kind = Unix.S_REG; _ } -> (try Unix.access path [ Unix.X_OK ]; true with Unix.Unix_error _ -> false)
  | _ -> false
  | exception Unix.Unix_error _ -> false

(* The first executable named [name] on PATH, like the shell would find it. *)
let search_path name =
  let dirs = String.split_on_char ':' (Option.value (Sys.getenv_opt "PATH") ~default:"") in
  List.find_map
    (fun dir ->
      let candidate = absolute (Filename.concat (if dir = "" then "." else dir) name) in
      if is_executable_file candidate then Some candidate else None)
    dirs

(* The one absolute path that is both stat'ed (fingerprint, freshness) and spawned: a slashless [tool_path]
   is looked up on PATH, a relative one against the working directory. *)
let resolve_binary t ~game ~tool_path =
  let binary =
    match tool_path with
    | None -> default_binary t game
    | Some "" -> fail "tool_path is empty"
    | Some path when String.contains path '/' -> absolute path
    | Some name -> (
        match search_path name with
        | Some path -> path
        | None -> fail "%s was not found on PATH (use a path containing a slash for a file outside PATH)" name)
  in
  if not (Sys.file_exists binary) then
    fail "%s does not exist.%s" (relative t binary)
      (if tool_path = None then
         Printf.sprintf
           " Build it first: nix develop -c cmake --build build/%s --target f3rt-tool (configure with f3-compile %s if build/%s is missing)."
           game game game
       else "");
  if not (is_executable_file binary) then fail "%s is not an executable file" (relative t binary);
  binary

let binary_status session =
  match fingerprint session.binary with
  | None -> Missing
  | Some now -> if now = session.opened then Fresh else Stale

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
   when the child is gone, when it exits while the call is pending, and (with [timeout]) when it does not
   answer in time. *)
let rpc (type a) ?timeout conn (make : ((a, string) result -> unit) -> Mcp.Protocol.outgoing_message) :
    (a, string) result =
  if Eio.Promise.is_resolved conn.dead then Error "the runtime process has exited"
  else
    let promise, resolver = Eio.Promise.create () in
    let sent = ref false in
    let call () =
      let outgoing =
        make (fun result -> if not (Eio.Promise.is_resolved promise) then Eio.Promise.resolve resolver result)
      in
      match send conn outgoing with
      | exception (Eio.Cancel.Cancelled _ as exn) -> raise exn
      | exception exn -> Error ("cannot reach the runtime process: " ^ Printexc.to_string exn)
      | () ->
          sent := true;
          Eio.Fiber.first
            (fun () -> Eio.Promise.await promise)
            (fun () ->
              Eio.Promise.await conn.dead;
              (* A response may have arrived in the same breath as the exit. *)
              if Eio.Promise.is_resolved promise then Eio.Promise.await promise
              else Error "the runtime process exited while handling the call")
    in
    match timeout with
    | None -> call ()
    | Some seconds ->
        Eio.Fiber.first call (fun () ->
            Eio.Time.sleep conn.clock seconds;
            (* A request cut off half-written leaves the child's input unframed: it cannot be reused. *)
            if not !sent then (try Eio.Process.signal conn.process Sys.sigkill with _ -> ());
            Error (Printf.sprintf "timed out after %gs waiting for the runtime" seconds))

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
   a child that does not exit within [grace] seconds is killed. *)
let shutdown_child ?(grace = 10.0) t (conn : conn) =
  let process = conn.process and dead = conn.dead in
  close_pipe conn.child_stdin;
  let exited = with_timeout t grace (fun () -> Eio.Process.await process) in
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

(* Tears a half-opened child down even when the calling fiber is being cancelled (parent EOF). *)
let abort_child t conn = Eio.Cancel.protect (fun () -> ignore (shutdown_child ~grace:1.0 t conn))

(* OCaml reports a child's fatal signal with its own negative constants ([Sys.sigkill] = -7), never the
   system number: this maps them to the POSIX numbers and names (Linux numbering). *)
let signal_table =
  [ (Sys.sighup, 1, "SIGHUP"); (Sys.sigint, 2, "SIGINT"); (Sys.sigquit, 3, "SIGQUIT"); (Sys.sigill, 4, "SIGILL");
    (Sys.sigtrap, 5, "SIGTRAP"); (Sys.sigabrt, 6, "SIGABRT"); (Sys.sigbus, 7, "SIGBUS"); (Sys.sigfpe, 8, "SIGFPE");
    (Sys.sigkill, 9, "SIGKILL"); (Sys.sigusr1, 10, "SIGUSR1"); (Sys.sigsegv, 11, "SIGSEGV");
    (Sys.sigusr2, 12, "SIGUSR2"); (Sys.sigpipe, 13, "SIGPIPE"); (Sys.sigalrm, 14, "SIGALRM");
    (Sys.sigterm, 15, "SIGTERM"); (Sys.sigchld, 17, "SIGCHLD"); (Sys.sigcont, 18, "SIGCONT");
    (Sys.sigstop, 19, "SIGSTOP"); (Sys.sigtstp, 20, "SIGTSTP"); (Sys.sigttin, 21, "SIGTTIN");
    (Sys.sigttou, 22, "SIGTTOU"); (Sys.sigurg, 23, "SIGURG"); (Sys.sigxcpu, 24, "SIGXCPU");
    (Sys.sigxfsz, 25, "SIGXFSZ"); (Sys.sigvtalrm, 26, "SIGVTALRM"); (Sys.sigprof, 27, "SIGPROF");
    (Sys.sigpoll, 29, "SIGPOLL"); (Sys.sigsys, 31, "SIGSYS") ]

(* (POSIX number, name) of an [Eio.Process] [`Signaled] value; a positive value is already a system number. *)
let posix_signal ocaml_signal =
  match List.find_opt (fun (ocaml, _, _) -> ocaml = ocaml_signal) signal_table with
  | Some (_, number, name) -> (Some number, name)
  | None -> (
      match List.find_opt (fun (_, number, _) -> number = ocaml_signal) signal_table with
      | Some (_, number, name) when ocaml_signal > 0 -> (Some number, name)
      | _ when ocaml_signal > 0 -> (Some ocaml_signal, Printf.sprintf "signal %d" ocaml_signal)
      | _ -> (None, "an unknown signal"))

let exit_text = function
  | `Exited code -> Printf.sprintf "exited with status %d" code
  | `Signaled signal -> (
      match posix_signal signal with
      | Some number, name -> Printf.sprintf "killed by %s (signal %d)" name number
      | None, name -> Printf.sprintf "killed by %s" name)

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
  { client; transport; clock; child_stdin = stdin_w; child_stdout = stdout_r; process; dead }

(* [initialize] then [tools/list], each bounded by the handshake timeout. The child's protocol version must
   be one this bridge speaks. *)
let handshake t (conn : conn) =
  let timeout = t.timeouts.handshake in
  match rpc ~timeout conn (fun callback -> Mcp.Client.initialize conn.client ~protocol_version callback) with
  | Error message -> fail "the runtime did not initialise: %s" message
  | Ok init when not (List.mem init.protocol_version supported_protocol_versions) ->
      fail "the runtime answered initialize with unsupported protocolVersion %S (supported: %s)" init.protocol_version
        (String.concat ", " supported_protocol_versions)
  | Ok _ -> (
      let initialized =
        Mcp.Client.send_notification conn.client (Mcp.Notification.Initialized { meta = None })
      in
      send conn initialized;
      match rpc ~timeout conn (fun callback -> Mcp.Client.tools_list conn.client callback) with
      | Error message -> fail "the runtime did not list its tools: %s" message
      | Ok listing -> listing.tools)

(* ---- bridged tool descriptors ---- *)

let session_id_prop =
  string_prop
    "Runtime session to use (see session_list). Defaults to the most recently opened live session."

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

(* ---- sessions ---- *)

let is_live session = (not session.closed) && not (Eio.Promise.is_resolved session.conn.dead)

(* The session calls go to when no session_id is given: the most recently opened one whose child is alive. *)
let newest_live t =
  List.find_map
    (fun id -> match Hashtbl.find_opt t.sessions id with Some session when is_live session -> Some session | _ -> None)
    t.order

(* Recomputes which runtime tools are advertised (the newest live session's, else the --game pinned list);
   true when that changed. *)
let refresh_advertised t =
  let raw = match newest_live t with Some session -> session.tools | None -> t.pinned in
  let changed = raw <> t.advertised in
  t.advertised <- raw;
  changed

let runtime_tools t = List.map bridged_descriptor t.advertised

let set_pinned t tools =
  t.pinned <- tools;
  refresh_advertised t

let no_session_hint = "call session_open first (then refresh the tool list), or start f3recomp mcp with --game"

let find_session t = function
  | Some id -> (
      match Hashtbl.find_opt t.sessions id with
      | Some session -> session
      | None ->
          fail "no open session %S (open: %s)" id
            (if t.order = [] then "none" else String.concat ", " t.order))
  | None -> (
      match newest_live t with
      | Some session -> session
      | None ->
          if t.order = [] then fail "no runtime session is open: %s" no_session_hint
          else
            fail "no live runtime session: the child process of every open session (%s) has exited. Call session_close \
                  for each, then session_open again."
              (String.concat ", " t.order))

(* session_close without an id: the newest live session, else the newest one at all (a dead one to clear). *)
let find_session_to_close t = function
  | Some _ as id -> find_session t id
  | None -> (
      match newest_live t with
      | Some session -> session
      | None -> (
          match List.find_map (Hashtbl.find_opt t.sessions) t.order with
          | Some session -> session
          | None -> fail "no runtime session is open: %s" no_session_hint))

let id_taken t id = Hashtbl.mem t.sessions id || Hashtbl.mem t.reserved id

let fresh_id t game =
  let rec pick n =
    let id = if n = 1 then game else Printf.sprintf "%s-%d" game n in
    if id_taken t id then pick (n + 1) else id
  in
  pick 1

let close_session t session =
  if not session.closed then begin
    session.closed <- true;
    Hashtbl.remove t.sessions session.id;
    t.order <- List.filter (fun id -> id <> session.id) t.order;
    ignore (refresh_advertised t)
  end;
  shutdown_child t session.conn

let init_params args =
  List.filter_map
    (fun key -> match member key args with Some value -> Some (key, value) | None -> None)
    [ "set"; "game_video"; "sprite_writers"; "video_diff"; "eeprom" ]

let init_text (init : Mcp.Request.Tools.Call.result) =
  String.concat "\n"
    (List.filter_map (function Mcp.Types.Content.Text { text; _ } -> Some text | _ -> None) init.content)

(* Spawns the runtime, handshakes, lists its tools and runs its [init]. *)
let open_session t args =
  let game = require_string "game" args in
  Static_tools.safe_game game;
  let tool_path = string_arg "tool_path" args in
  let binary = resolve_binary t ~game ~tool_path in
  let rom_dir = Option.value (string_arg "rom_dir" args) ~default:(Filename.concat t.root ("roms/" ^ game)) in
  if not (Sys.file_exists rom_dir) then fail "ROM directory %s does not exist (pass rom_dir)" rom_dir;
  let id =
    match string_arg "session_id" args with
    | Some id ->
        if id_taken t id then fail "session %S is already open" id;
        id
    | None -> fresh_id t game
  in
  Hashtbl.replace t.reserved id ();
  Fun.protect ~finally:(fun () -> Hashtbl.remove t.reserved id) @@ fun () ->
  (* The fingerprint is taken before the spawn: a rebuild after this point is what "stale" means. *)
  let opened =
    match fingerprint binary with
    | Some stat -> stat
    | None -> fail "cannot stat %s: the runtime binary is not accessible" (relative t binary)
  in
  let conn = spawn t ~binary ~args:[ "mcp"; "--no-init" ] in
  match
    let tools = handshake t conn in
    let init_args = `Assoc (("rom_dir", `String rom_dir) :: init_params args) in
    let init_result =
      rpc ~timeout:t.timeouts.handshake conn (fun callback ->
          Mcp.Client.tools_call conn.client ~name:"init" ~arguments:init_args callback)
    in
    (tools, init_result)
  with
  | exception exn -> abort_child t conn; raise exn
  | _, Error message -> abort_child t conn; fail "the runtime's init failed: %s" message
  | _, Ok init when init.is_error = Some true ->
      abort_child t conn;
      fail "the runtime's init failed: %s" (init_text init)
  | tools, Ok init ->
      let set = Option.value (string_arg "set" args) ~default:game in
      let session =
        { id; game; binary; opened; config = Filename.concat t.root (Printf.sprintf "games/%s/config.toml" game);
          opened_set = set; rom_dir; current_set = set; conn; tools; lock = Eio.Mutex.create (); a5 = None;
          closed = false }
      in
      Hashtbl.replace t.sessions id session;
      t.order <- id :: t.order;
      ignore (refresh_advertised t);
      (* A child that dies on its own changes which session is the default and may change the tool list. *)
      Eio.Fiber.fork_daemon ~sw:t.sw (fun () ->
          Eio.Promise.await conn.dead;
          if (not session.closed) && refresh_advertised t then t.on_tools_changed ();
          `Stop_daemon);
      (session, init.structured_content)

(* ---- addresses relative to a5 ---- *)

let u32_max = 0xffffffff

let session_a5 t session =
  match session.a5 with
  | Some a5 -> a5
  | None ->
      if session.current_set <> session.opened_set then
        fail
          "cannot resolve an a5-relative address: rt_init switched the ROM set to %S (the session was opened with \
           %S), so the a5 base inferred from %s no longer applies. Pass an absolute address."
          session.current_set session.opened_set (relative t session.config);
      if not (Sys.file_exists session.config) then
        fail "cannot resolve an a5-relative address: %s does not exist" (relative t session.config);
      let result =
        let unresolved why =
          fail "cannot resolve an a5-relative address from the ROMs in %s: %s (pass an absolute address)"
            session.rom_dir why
        in
        try F3recomp.Api.inspect t.api ~config:session.config ~rom_dir:session.rom_dir ~routines:false ()
        with
        | Sys_error why -> unresolved why
        | F3recomp.Error.Invalid { what; _ } -> unresolved what
      in
      let a5 =
        match F3recomp.Json.member "a5" result with
        | Some (F3recomp.Json.Int a5) when a5 >= 0 && a5 <= u32_max -> a5
        | Some (F3recomp.Json.Int a5) -> fail "cannot resolve an a5-relative address: the inferred a5 base %d is not a 32-bit address" a5
        | _ -> fail "cannot resolve an a5-relative address: no a5 base was found in the ROM (pass an absolute address)"
      in
      session.a5 <- Some a5;
      a5

(* "a5-0x60ac" and "a5+0x10" (case and spaces ignored) -> the absolute address, which must stay inside the
   68020's 32-bit space; anything else is untouched. *)
let resolve_address t session = function
  | `String text -> (
      let compact = String.concat "" (String.split_on_char ' ' (String.lowercase_ascii text)) in
      if String.length compact > 3 && String.sub compact 0 2 = "a5" && (compact.[2] = '+' || compact.[2] = '-') then begin
        let offset = F3recomp.Inspect.parse_address (String.sub compact 3 (String.length compact - 3)) in
        if offset < 0 || offset > u32_max then fail "the offset in %S is outside 0..0xffffffff" text;
        let a5 = session_a5 t session in
        let address = if compact.[2] = '-' then a5 - offset else a5 + offset in
        if address < 0 || address > u32_max then
          fail "%S resolves to %s, outside the 32-bit address space (a5 = 0x%x)" text
            (if address < 0 then Printf.sprintf "-0x%x" (-address) else Printf.sprintf "0x%x" address)
            a5;
        `Int address
      end
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
  (* init without a rom_dir would load whatever the child defaults to, not what the cached a5 was inferred from. *)
  let args =
    if tool = "init" && not (List.mem_assoc "rom_dir" args) then ("rom_dir", `String session.rom_dir) :: args
    else args
  in
  `Assoc args

(* A successful [rt_init] reloads the machine from [rom_dir]/[set]: the cached a5 and ROM source follow it. *)
let note_init session arguments =
  (match string_arg "rom_dir" arguments with Some dir -> session.rom_dir <- dir | None -> ());
  session.current_set <- Option.value (string_arg "set" arguments) ~default:session.game;
  session.a5 <- None

let call_timeout t tool = if tool = "step" then t.timeouts.step else t.timeouts.call

(* Forwards [rt_<tool>] to the child verbatim (content blocks, structuredContent and isError pass through).
   Calls to one session are serialised; the tool must exist in that session's own tool list. *)
let call_runtime t ~tool args =
  protect (fun () ->
      let session = find_session t (string_arg "session_id" args) in
      if not (List.exists (fun (d : Mcp.Types.Tool.t) -> d.name = tool) session.tools) then
        error_result
          (Printf.sprintf "%s%s is not available in runtime session %S (its tools: %s)" tool_prefix tool session.id
             (String.concat ", " (List.map (fun (d : Mcp.Types.Tool.t) -> tool_prefix ^ d.name) session.tools)))
      else
        Eio.Mutex.use_ro session.lock (fun () ->
            if session.closed then error_result (Printf.sprintf "runtime session %S was closed" session.id)
            else
              match binary_status session with
              | (Stale | Missing) as status -> error_result (stale_message t session status)
              | Fresh -> (
                  let arguments = forwarded_arguments t session ~tool args in
                  match
                    rpc ~timeout:(call_timeout t tool) session.conn (fun callback ->
                        Mcp.Client.tools_call session.conn.client ~name:tool ~arguments callback)
                  with
                  | Ok result ->
                      if tool = "init" && result.is_error <> Some true then note_init session arguments;
                      result
                  | Error message ->
                      error_result
                        (Printf.sprintf "runtime session %S: %s%s" session.id message
                           (if String.starts_with ~prefix:"timed out" message then
                              ". The runtime may be stalled: session_close it, then session_open again."
                            else "")))))

(* ---- one-shot f3rt-tool commands ---- *)

let max_output = 8 * 1024 * 1024
let max_count = 0x7fffffff

(* A count-like argument (frames, seed) must be a non-negative integer the child can parse. *)
let count_arg name args =
  match int_arg name args with
  | Some n when n < 0 || n > max_count -> fail "%S must be between 0 and %d (got %d)" name max_count n
  | other -> other

let rec check_passthrough_counts = function
  | ("--frames" | "--seed" as flag) :: value :: rest ->
      if String.length value > 0 && value.[0] = '-' then fail "args: %s must not be negative (got %s)" flag value;
      check_passthrough_counts (value :: rest)
  | _ :: rest -> check_passthrough_counts rest
  | [] -> ()

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
      let frames = count_arg "frames" args and seed = count_arg "seed" args in
      check_passthrough_counts extra;
      let argv =
        [ binary; command; "--json"; "--rom-dir"; rom_dir ]
        @ flag "--set" (string_arg "set" args)
        @ flag "--frames" (Option.map string_of_int frames)
        @ flag "--seed" (Option.map string_of_int seed)
        @ extra
      in
      let mgr = Eio.Stdenv.process_mgr t.env in
      Eio.Switch.run @@ fun sw ->
      (* The child's stdin is a pipe that is already at EOF: it must never read the MCP stream. *)
      let stdin_r, stdin_w = Eio.Process.pipe ~sw mgr in
      close_pipe stdin_w;
      let stdout_r, stdout_w = Eio.Process.pipe ~sw mgr in
      let process =
        try Eio.Process.spawn ~sw mgr ~stdin:stdin_r ~stdout:stdout_w ~stderr:(Eio.Stdenv.stderr t.env) argv
        with exn -> close_pipe stdin_r; close_pipe stdout_r; close_pipe stdout_w; raise exn
      in
      close_pipe stdin_r;
      close_pipe stdout_w;
      let kill () = try Eio.Process.signal process Sys.sigkill with _ -> () in
      let output =
        match Eio.Buf_read.parse Eio.Buf_read.take_all stdout_r ~max_size:max_output with
        | result -> result
        | exception exn -> kill (); close_pipe stdout_r; raise exn
      in
      close_pipe stdout_r;
      match output with
      | Error _ ->
          kill ();
          ignore (Eio.Process.await process);
          error_result
            (Printf.sprintf "f3rt-tool %s wrote more than %d MiB to stdout; the process was killed" command
               (max_output / 1024 / 1024))
      | Ok output ->
          let status = Eio.Process.await process in
          let exit_code, signal =
            match status with
            | `Exited code -> (`Int code, `Null)
            | `Signaled s ->
                let number, name = posix_signal s in
                ( `Null,
                  `Assoc [ ("number", match number with Some n -> `Int n | None -> `Null); ("name", `String name) ] )
          in
          let parsed = try Some (Yojson.Safe.from_string output) with _ -> None in
          let failed = status <> `Exited 0 || parsed = None in
          let result =
            `Assoc
              ([ ("command", `String command); ("exit_code", exit_code) ]
               @ (match signal with `Null -> [] | signal -> [ ("signal", signal) ])
               @ (match parsed with
                  | Some json -> [ ("result", json) ]
                  | None ->
                      [ ("stdout", `String output);
                        ("error", `String "stdout is not JSON") ]))
          in
          let tool_result = structured result in
          if failed then { tool_result with is_error = Some true } else tool_result)
