(* [f3recomp mcp]: one MCP server (stdio) for static analysis of a game's ROM and for a live runtime.

   Static tools run in this process. [session_open] spawns [build/<game>/f3rt-tool mcp] and bridges its tools
   as [rt_<name>], emitting [notifications/tools/list_changed]. See [Runtime_bridge]. *)

open Tool_support

let server_name = "f3recomp"
let server_version = "1.0.0"

let instructions =
  "Taito F3 reverse-engineering server for one repository checkout.\n\
   \n\
   Static analysis (works from games/<game>/config.toml and roms/<game>, or explicit config + rom_dir): rom_read, \
   disasm, inspect, xref, flow, coverage, check_irq, decisions, lower, profile_merge. Pass game=<id> for the \
   repository layout; results are JSON in structuredContent.\n\
   \n\
   Live runtime: session_open(game) spawns build/<game>/f3rt-tool mcp, boots the game, and dynamically registers \
   the runtime's tools as rt_<name> (rt_step, rt_regs, rt_read, rt_write, rt_set_input, rt_screenshot, rt_sprites, \
   rt_save_state, rt_load_state, rt_crc, ...), each with an optional session_id. This server then sends \
   notifications/tools/list_changed: refresh (re-run tools/list) after session_open and after session_close. Clients \
   that ignore list_changed can start the server with --game <game> so the rt_* tools are listed from the start. \
   rt_read and rt_write accept a5+0x... / a5-0x... addresses, resolved from the game's config.\n\
   \n\
   Stale runtimes: if build/<game>/f3rt-tool is rebuilt (or replaced or removed) while a session is open, every \
   rt_* call returns an error instead of running old code. Call session_close, then session_open again. \
   session_list shows whether each session's binary is fresh or stale. Close sessions when done: each owns a \
   live machine.\n\
   \n\
   One-shot runs without a session: gameplay, sprite_check and motion run `f3rt-tool <command> --json` fresh per \
   call (they can take minutes)."

let tool_descriptor ~name ~description ~schema : Mcp.Types.Tool.t =
  { name; title = None; description = Some description; input_schema = schema; output_schema = None;
    annotations = None; meta = None }

(* ---- session management tools ---- *)

let session_open_schema =
  object_schema ~required:[ "game" ]
    [
      ("game", string_prop "Game id; the runtime is build/<game>/f3rt-tool and the ROMs roms/<game>.");
      ("rom_dir", string_prop "ROM directory (default roms/<game>).");
      ("set", string_prop "ROM set name passed to the runtime's init (default: the build's game).");
      ("game_video", bool_prop "true: game renderer (default); false: FDP-oracle scanout.");
      ("sprite_writers", bool_prop "Record the PC that last wrote each sprite RAM slot.");
      ("video_diff", bool_prop "Also build the FDP oracle so rt_step/rt_video_diff can compare layers.");
      ("eeprom", string_prop "EEPROM image to load at init.");
      ("tool_path", string_prop "Use this f3rt-tool binary instead of build/<game>/f3rt-tool.");
      ("session_id", string_prop "Name for the session (default: the game id).");
    ]

let session_open_description =
  "Open a live runtime session: spawns build/<game>/f3rt-tool mcp, boots the game from its ROMs, and dynamically \
   registers the runtime's tools as rt_<name> (rt_step, rt_regs, rt_read, rt_write, rt_screenshot, rt_sprites, ...) \
   with an optional session_id. Emits notifications/tools/list_changed: refresh your tool list after this call (or \
   start f3recomp mcp with --game <game> to have the rt_* tools listed from the start). The binary's mtime, inode \
   and size are recorded now; if build/<game>/f3rt-tool is rebuilt on disk while the session is open, every rt_* \
   call returns an error until you session_close and session_open again."

let session_close_description =
  "Close a runtime session (default: the most recently opened): ends its f3rt-tool process, which frees the machine. \
   When no session remains the rt_* tools are unregistered (unless pinned by --game) and \
   notifications/tools/list_changed is emitted, so refresh your tool list afterwards. Use this to recover from a \
   stale-binary error, then call session_open again."

let session_list_description =
  "List the open runtime sessions: id, game, binary path, ROM directory, process state, and whether the binary on \
   disk is fresh (unchanged since the session opened), stale (rebuilt or replaced: rt_* calls are refused) or \
   missing."

let session_json (t : Runtime_bridge.t) (session : Runtime_bridge.session) =
  `Assoc
    [
      ("session_id", `String session.id);
      ("game", `String session.game);
      ("binary", `String (Runtime_bridge.relative t session.binary));
      ("rom_dir", `String session.rom_dir);
      ("alive", `Bool (not (Eio.Promise.is_resolved session.conn.dead)));
      ("binary_status", `String (Runtime_bridge.status_name (Runtime_bridge.binary_status session)));
      (* The session a call without session_id goes to: the newest one whose child is alive. *)
      ("active", `Bool (match Runtime_bridge.newest_live t with Some live -> live.id = session.id | None -> false));
    ]

let sessions_json (t : Runtime_bridge.t) =
  `List (List.filter_map (fun id -> Option.map (session_json t) (Hashtbl.find_opt t.sessions id)) t.order)

let tool_names (t : Runtime_bridge.t) =
  `List (List.map (fun (tool : Mcp.Types.Tool.t) -> `String tool.name) (Runtime_bridge.runtime_tools t))

(* ---- the tool table ---- *)

type entry = {
  descriptor : Mcp.Types.Tool.t;
  run : Yojson.Safe.t -> Mcp.Request.Tools.Call.result;
}

let one_shot_schema extra =
  object_schema ~required:[ "game" ]
    ([
       ("game", string_prop "Game id; runs build/<game>/f3rt-tool with the ROMs in roms/<game>.");
       ("rom_dir", string_prop "ROM directory (default roms/<game>).");
       ("set", string_prop "ROM set name.");
       ("frames", int_prop "Number of frames to advance.");
       ("seed", int_prop "PRNG seed for the input schedule.");
       ("args", `Assoc [ ("type", `String "array"); ("items", `Assoc [ ("type", `String "string") ]);
                         ("description", `String "Further f3rt-tool flags, passed verbatim (see f3rt-tool <command> --help).") ]);
       ("tool_path", string_prop "Use this f3rt-tool binary instead of build/<game>/f3rt-tool.");
     ]
    @ extra)

(* Entries are built once; the stateful ones close over [t] and [notify]. *)
let entries (t : Runtime_bridge.t) ~notify =
  let static =
    List.map
      (fun (tool : Static_tools.t) ->
        { descriptor = tool_descriptor ~name:tool.name ~description:tool.description ~schema:tool.schema;
          run = tool.run ~root:t.root t.api })
      Static_tools.all
  in
  let managed name description schema run =
    { descriptor = tool_descriptor ~name ~description ~schema;
      run = (fun args -> protect (fun () -> check_known ~schema args; run args)) }
  in
  let open_ =
    managed "session_open" session_open_description session_open_schema (fun args ->
        let session, init = Runtime_bridge.open_session t args in
        notify ();
        structured
          (`Assoc
             [
               ("session_id", `String session.id);
               ("game", `String session.game);
               ("binary", `String (Runtime_bridge.relative t session.binary));
               ("rom_dir", `String session.rom_dir);
               ("init", Option.value init ~default:`Null);
               ("runtime_tools", tool_names t);
               ("tools_list_changed", `Bool true);
               ("note", `String "notifications/tools/list_changed was sent: refresh the tool list to see the rt_* tools.");
             ]))
  in
  let close =
    managed "session_close" session_close_description
      (object_schema [ ("session_id", string_prop "Session to close (default: the most recently opened).") ])
      (fun args ->
        let session = Runtime_bridge.find_session_to_close t (string_arg "session_id" args) in
        let status = Runtime_bridge.close_session t session in
        notify ();
        structured
          (`Assoc
             [
               ("closed", `String session.id);
               ("process", `String (Runtime_bridge.exit_text status));
               ("remaining", sessions_json t);
               ("runtime_tools", tool_names t);
               ("tools_list_changed", `Bool true);
             ]))
  in
  let list =
    managed "session_list" session_list_description (object_schema []) (fun _ ->
        structured (`Assoc [ ("sessions", sessions_json t); ("pinned_tools", `Bool (t.pinned <> [])) ]))
  in
  let one_shot name description extra =
    let schema = one_shot_schema extra in
    let command = String.map (function '_' -> '-' | c -> c) name in
    managed name description schema (fun args -> Runtime_bridge.run_one_shot t ~command args)
  in
  static
  @ [ open_; close; list ]
  @ [
      one_shot "gameplay"
        "Run `f3rt-tool gameplay --json`: the deterministic strict-native seeded gameplay regression harness (default \
         40000 frames; can take minutes). Fresh process per call; no session needed. Returns the harness's JSON \
         result (frame/state CRCs, status) and the exit code."
        [];
      one_shot "sprite_check"
        "Run `f3rt-tool sprite-check --json`: sprite-group coverage, and a check that emulated state is identical with \
         sprite groups on and off. Fresh process per call."
        [];
      one_shot "motion"
        "Run `f3rt-tool motion --json`: the strict-native seeded temporal GPU motion proof. Fresh process per call."
        [];
    ]

(* MCP protocol versions this server speaks, newest first. *)
let supported_protocol_versions = [ Runtime_bridge.protocol_version; "2025-06-18"; "2025-03-26"; "2024-11-05" ]
let latest_protocol_version = List.hd supported_protocol_versions

(* A requested version we support is answered as is; otherwise the server answers with its latest. *)
let negotiate_protocol_version requested =
  if List.mem requested supported_protocol_versions then requested else latest_protocol_version

let runtime_tool_name (t : Runtime_bridge.t) name =
  let prefix = Runtime_bridge.tool_prefix in
  if String.starts_with ~prefix name then
    let tool = String.sub name (String.length prefix) (String.length name - String.length prefix) in
    let in_list tools = List.exists (fun (d : Mcp.Types.Tool.t) -> d.name = tool) tools in
    let in_any_session =
      Hashtbl.fold (fun _ (s : Runtime_bridge.session) acc -> acc || in_list s.tools) t.sessions false
    in
    if in_any_session || in_list t.pinned then Some tool else None
  else None

let unknown_tool_message name =
  if String.starts_with ~prefix:Runtime_bridge.tool_prefix name then
    Printf.sprintf
      "unknown tool %S: no runtime session is open. Call session_open (then refresh the tool list), or start \
       f3recomp mcp with --game."
      name
  else Printf.sprintf "unknown tool %S" name

(* [None] when [params.name] is not a registered tool: that is a protocol error, not a tool failure. *)
let run_tool table (t : Runtime_bridge.t) (params : Mcp.Request.Tools.Call.params) =
  let args = Option.value params.arguments ~default:(`Assoc []) in
  match Hashtbl.find_opt table params.name with
  | Some entry -> Some (entry.run args)
  | None -> Option.map (fun tool -> Runtime_bridge.call_runtime t ~tool args) (runtime_tool_name t params.name)

(* ---- the server ---- *)

let make_server (t : Runtime_bridge.t) ~notify =
  let table = Hashtbl.create 32 in
  let entries = entries t ~notify in
  List.iter (fun entry -> Hashtbl.replace table entry.descriptor.name entry) entries;
  let on_initialize (params : Mcp.Request.Initialize.params) : (Mcp.Request.Initialize.result, string) result =
    Ok
      {
        protocol_version = negotiate_protocol_version params.protocol_version;
        capabilities = { experimental = None; logging = None; completions = None; prompts = None; resources = None; tools = Some { list_changed = Some true } };
        server_info = { name = server_name; version = server_version };
        instructions = Some instructions;
        meta = None;
      }
  in
  let on_tools_list (params : Mcp.Request.Tools.List.params) =
    let tools =
      List.map (fun entry -> entry.descriptor) entries @ Runtime_bridge.runtime_tools t
    in
    Ok { Mcp.Request.Tools.List.tools; next_cursor = None; meta = params.meta }
  in
  let on_tools_call params =
    match run_tool table t params with
    | Some result -> Ok result
    | None -> Error (unknown_tool_message params.name) (* [dispatch] answers unknown tools before this runs *)
  in
  let handler = { Mcp.Server.default_handler with on_initialize; on_tools_list; on_tools_call } in
  let server =
    Mcp.Server.create ~handler ~notification_handler:Mcp.Server.default_notification_handler
      ~server_info:{ name = server_name; version = server_version }
      ~server_capabilities:{ experimental = None; logging = None; completions = None; prompts = None; resources = None; tools = Some { list_changed = Some true } }
  in
  (server, table)

let json_rpc_invalid_params = -32602
let json_rpc_method_not_found = -32601

(* Request-level validation the library's handler cannot express: a tools/call for a tool that does not exist, or
   whose arguments are not an object, is invalid params (-32602). Tool failures stay [isError] results. *)
let dispatch (t : Runtime_bridge.t) (server, table) (message : Mcp.Protocol.incoming_message) =
  let invalid id message = Some (Mcp.Protocol.error_to_outgoing ~id ~code:json_rpc_invalid_params ~message ()) in
  match message with
  | Request (id, ToolsCall params) when Mcp.Server.is_initialized server -> (
      match params.arguments with
      | Some (`Assoc _) | Some `Null | None ->
          let exists = Hashtbl.mem table params.name || runtime_tool_name t params.name <> None in
          if exists then Mcp.Server.handle_message server message else invalid id (unknown_tool_message params.name)
      | Some _ -> invalid id "tools/call: \"arguments\" must be an object")
  | _ -> Mcp.Server.handle_message server message

(* The methods [Mcp.Request.of_jsonrpc] knows: a request for one of them that fails to parse has bad params. *)
let known_methods =
  [ "initialize"; "resources/list"; "resources/read"; "resources/subscribe"; "resources/unsubscribe";
    "resources/templates/list"; "prompts/list"; "prompts/get"; "tools/list"; "tools/call";
    "sampling/createMessage"; "elicitation/create"; "logging/setLevel"; "completion/complete"; "roots/list"; "ping" ]

let parse_failure_code (request : Jsonrpc.Request.t) =
  if List.mem request.method_ known_methods then json_rpc_invalid_params else json_rpc_method_not_found

(* Reads packets until EOF. A bad line or an unsupported request must not end the server.

   Only [tools/call] can block (a bridged call waits on a child, a one-shot runs for minutes), so each runs in
   its own fiber while this loop keeps reading: ping, tools/list, [notifications/cancelled] and the end of input
   are never held up by a running call. Responses go out through [send], which serialises writers. On end of
   input every in-flight call is cancelled before this returns. *)
let serve ~clock ~transport ~server ~handle ~send ~take_notification ~background =
  let inflight : (Jsonrpc.Id.t, unit Eio.Promise.u) Hashtbl.t = Hashtbl.create 16 in
  let notify_list_changed () =
    send (Mcp.Server.send_notification server (Mcp.Notification.ToolsListChanged { meta = None }))
  in
  (* Handles [message] and sends its response and then, if the call changed the tool list, the notification
     (a client that refetches on it must see the new list). *)
  let respond message =
    let response = handle message in
    let notified = take_notification () in
    Option.iter send response;
    if notified then notify_list_changed ()
  in
  let call_in_fiber ~sw id message =
    let cancelled, cancel = Eio.Promise.create () in
    Hashtbl.replace inflight id cancel;
    Eio.Fiber.fork ~sw (fun () ->
        Fun.protect
          ~finally:(fun () -> Hashtbl.remove inflight id)
          (fun () ->
            try Eio.Fiber.first (fun () -> respond message) (fun () -> Eio.Promise.await cancelled)
            with
            | Eio.Cancel.Cancelled _ as exn -> raise exn
            | exn ->
                Logs.err (fun m -> m "tools/call failed: %s" (Printexc.to_string exn));
                (try
                   send (Mcp.Protocol.error_to_outgoing ~id ~code:(-32603) ~message:(Printexc.to_string exn) ())
                 with Eio.Cancel.Cancelled _ as exn -> raise exn | _ -> ())))
  in
  let cancel_request (params : Mcp.Notification.Cancelled.params) =
    let id : Jsonrpc.Id.t = match params.request_id with String s -> `String s | Int n -> `Int n in
    match Hashtbl.find_opt inflight id with
    | Some cancel -> Hashtbl.remove inflight id; Eio.Promise.resolve cancel ()
    | None -> ()
  in
  let handle_packet ~sw packet =
    match Mcp.Protocol.parse_message packet with
    | Ok (Mcp.Protocol.Request (id, Mcp.Request.ToolsCall _) as message) -> call_in_fiber ~sw id message
    | Ok (Mcp.Protocol.Notification (Mcp.Notification.Cancelled params)) -> cancel_request params
    | Ok message -> respond message
    | Error message -> (
        match packet with
        | Jsonrpc.Packet.Request request ->
            send (Mcp.Protocol.error_to_outgoing ~id:request.id ~code:(parse_failure_code request) ~message ())
        | _ -> Logs.warn (fun m -> m "ignoring a message: %s" message))
  in
  let rec loop ~sw failures =
    match Mcp_eio.Stdio.recv transport ~clock () with
    | None -> ()
    | exception Failure message ->
        Logs.warn (fun m -> m "ignoring a malformed line: %s" message);
        if failures < 3 then loop ~sw (failures + 1)
    | Some packet ->
        handle_packet ~sw packet;
        loop ~sw 0
  in
  try
    Eio.Switch.run @@ fun sw ->
    background sw;
    loop ~sw 0;
    (* End of input: nobody is left to answer, so stop whatever is still running. *)
    Eio.Switch.fail sw Exit
  with Exit -> ()

(* Stdout belongs to the MCP transport: it gets a private duplicate of fd 1, and fd 1 itself now points at
   stderr, so a stray print can never corrupt the framing. *)
let take_stdout ~sw =
  flush stdout;
  let mcp_fd = Unix.dup ~cloexec:true Unix.stdout in
  Unix.dup2 ~cloexec:false Unix.stderr Unix.stdout;
  Eio_unix.File.import_rw ~sw ~close_unix:true mcp_fd

(* Lists the runtime's tools from a throwaway child, so a client that ignores list_changed still sees them.
   Bounded by the handshake timeout; runs beside [serve], so a slow child never delays [initialize]. *)
let pin_game (t : Runtime_bridge.t) game ~changed =
  match
    Static_tools.safe_game game;
    let binary = Runtime_bridge.resolve_binary t ~game ~tool_path:None in
    let conn = Runtime_bridge.spawn t ~binary ~args:[ "mcp"; "--no-init" ] in
    let tools =
      try Runtime_bridge.handshake t conn with exn -> Runtime_bridge.abort_child t conn; raise exn
    in
    ignore (Runtime_bridge.shutdown_child t conn);
    tools
  with
  | tools -> if Runtime_bridge.set_pinned t tools then changed ()
  | exception Tool_error message -> prerr_endline ("f3recomp mcp: --game " ^ game ^ ": " ^ message)
  | exception (Eio.Cancel.Cancelled _ as exn) -> raise exn
  | exception exn -> prerr_endline ("f3recomp mcp: --game " ^ game ^ ": " ^ Printexc.to_string exn)

let run ~root ~game =
  Logs.set_reporter (Logs.format_reporter ~app:Format.err_formatter ~dst:Format.err_formatter ());
  Logs.set_level (Some Logs.Warning);
  Eio_main.run @@ fun env ->
  Eio.Switch.run @@ fun sw ->
  let out = take_stdout ~sw in
  let transport = Mcp_eio.Stdio.create ~stdin:(Eio.Stdenv.stdin env) ~stdout:out in
  let write_lock = Eio.Mutex.create () in
  (* Several fibers answer at once: one packet at a time on stdout. *)
  let send outgoing =
    Eio.Mutex.use_ro write_lock (fun () -> Mcp_eio.Stdio.send transport (Mcp.Protocol.outgoing_to_message outgoing))
  in
  let t = Runtime_bridge.create ~root ~env ~sw ~api:(F3recomp.Api.session ()) in
  let pending = ref false in
  let server, table = make_server t ~notify:(fun () -> pending := true) in
  let handle = dispatch t (server, table) in
  let take_notification () = let v = !pending in pending := false; v in
  (* Not before the handshake: a client has not asked for the list yet, and will see the current one. *)
  let announce_tools_changed () =
    if Mcp.Server.is_initialized server then
      try send (Mcp.Server.send_notification server (Mcp.Notification.ToolsListChanged { meta = None }))
      with Eio.Cancel.Cancelled _ as exn -> raise exn | _ -> ()
  in
  t.on_tools_changed <- announce_tools_changed;
  let background sw =
    Option.iter (fun game -> Eio.Fiber.fork ~sw (fun () -> pin_game t game ~changed:announce_tools_changed)) game
  in
  Fun.protect
    ~finally:(fun () ->
      (* Every open session owns a child process: end them cleanly before the switch closes. *)
      List.iter
        (fun id -> match Hashtbl.find_opt t.sessions id with
           | Some session -> ignore (Runtime_bridge.close_session t session)
           | None -> ())
        t.order)
    (fun () -> serve ~clock:(Eio.Stdenv.clock env) ~transport ~server ~handle ~send ~take_notification ~background)
