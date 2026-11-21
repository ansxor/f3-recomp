(* End-to-end tests of the MCP servers over stdio:
   - [f3recomp mcp] on a tiny fixture game: protocol, static tools, errors, session lifecycle errors;
   - a real [f3rt-tool mcp] (standalone) and the bridge from [f3recomp mcp] to it: session_open, bridged runtime
     calls, a5 addresses, PNG passthrough, tools/list_changed, stale-binary refusal, session_close.
   The runtime cases need a built f3rt-tool (F3RT_TOOL, default build/<game>/f3rt-tool) and roms/<game>, and are
   skipped without them. F3RT_TEST_GAME picks the game (default commandw). *)

open F3recomp
open Json

let f3recomp = "../../bin/main.exe"

(* ---- files ---- *)

let write_file path contents = Out_channel.with_open_bin path (fun out -> output_string out contents)
let read_file path = In_channel.with_open_bin path In_channel.input_all

let scratch = lazy (
  let dir = Filename.temp_file ~temp_dir:(Sys.getcwd ()) "mcp" "" in
  Sys.remove dir;
  Sys.mkdir dir 0o700;
  at_exit (fun () ->
    Array.iter (fun file -> try Sys.remove (Filename.concat dir file) with Sys_error _ -> ())
      (Sys.readdir dir);
    try Sys.rmdir dir with Sys_error _ -> ());
  dir)

let scratch_file name = Filename.concat (Lazy.force scratch) name

(* ---- the repository and the runtime under test ---- *)

let rec find_root dir =
  if Sys.file_exists (Filename.concat dir "flake.nix") then Some dir
  else
    let parent = Filename.dirname dir in
    if parent = dir then None else find_root parent

let root = lazy (find_root (Sys.getcwd ()))
let game = Option.value (Sys.getenv_opt "F3RT_TEST_GAME") ~default:"commandw"

(* (root, f3rt-tool, ROM dir) when a runtime can be started. *)
let runtime () =
  match Lazy.force root with
  | None -> None
  | Some root ->
      let tool = match Sys.getenv_opt "F3RT_TOOL" with
        | Some path -> path
        | None -> Filename.concat root (Printf.sprintf "build/%s/f3rt-tool" game) in
      let rom_dir = match Sys.getenv_opt "F3RT_ROM_DIR" with
        | Some dir -> dir
        | None -> Filename.concat root ("roms/" ^ game) in
      if Sys.file_exists tool && Sys.file_exists rom_dir then
        Some (root, tool, rom_dir)
      else None

let need_runtime () = match runtime () with Some r -> r | None -> Alcotest.skip ()

(* ---- JSON access ---- *)

let field key json =
  match Json.member key json with
  | Some value -> value
  | None -> Alcotest.failf "missing %S in %s" key (Json.to_string json)

let items = function Array items -> items | json -> Alcotest.failf "not an array: %s" (Json.to_string json)
let int = function Int n -> n | json -> Alcotest.failf "not an int: %s" (Json.to_string json)
let str = function String s -> s | json -> Alcotest.failf "not a string: %s" (Json.to_string json)

let contains ~needle haystack =
  let n = String.length needle and h = String.length haystack in
  let rec scan i = i + n <= h && (String.sub haystack i n = needle || scan (i + 1)) in
  scan 0

let check_contains what ~needle haystack =
  if not (contains ~needle haystack) then Alcotest.failf "%s: expected %S in %S" what needle haystack

(* ---- a minimal MCP client over a child process ---- *)

type client = {
  pid : int;
  to_server : Unix.file_descr;
  from_server : Unix.file_descr;
  stderr_file : string;
  mutable buffer : string;
  mutable next_id : int;
  mutable notifications : Json.t list; (* messages without an id, oldest first *)
}

let spawn argv =
  let stderr_file = scratch_file (Printf.sprintf "stderr-%d" (Hashtbl.hash (Sys.time (), argv))) in
  let stdin_r, stdin_w = Unix.pipe ~cloexec:true () in
  let stdout_r, stdout_w = Unix.pipe ~cloexec:true () in
  let err = Unix.openfile stderr_file [ O_WRONLY; O_CREAT; O_TRUNC ] 0o600 in
  let pid = Unix.create_process (List.hd argv) (Array.of_list argv) stdin_r stdout_w err in
  List.iter Unix.close [ stdin_r; stdout_w; err ];
  { pid; to_server = stdin_w; from_server = stdout_r; stderr_file; buffer = ""; next_id = 0; notifications = [] }

let send_line client line =
  let data = Bytes.of_string (line ^ "\n") in
  let rec go offset =
    if offset < Bytes.length data then go (offset + Unix.write client.to_server data offset (Bytes.length data - offset))
  in
  go 0

(* One line from the server, waiting at most [timeout] seconds. *)
let read_line ?(timeout = 120.) client =
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

(* Sends a request and returns the whole response message; notifications met on the way are kept. *)
let request_raw client meth params =
  client.next_id <- client.next_id + 1;
  let id = client.next_id in
  send_line client
    (Json.to_string (Object [ "jsonrpc", String "2.0"; "id", Int id; "method", String meth; "params", params ]));
  let rec wait () =
    let message = parse (read_line client) in
    match Json.member "id" message with
    | Some (Int got) when got = id -> message
    | Some _ | None ->
        client.notifications <- client.notifications @ [ message ];
        wait ()
  in
  wait ()

let result_of response = match Json.member "result" response with
  | Some result -> result
  | None -> Alcotest.failf "request failed: %s" (Json.to_string response)

let request client meth params = result_of (request_raw client meth params)

let notify client meth =
  send_line client (Json.to_string (Object [ "jsonrpc", String "2.0"; "method", String meth ]))

let initialize client =
  let result =
    request client "initialize"
      (Object [ "protocolVersion", String "2025-11-25"; "capabilities", Object [];
                "clientInfo", Object [ "name", String "mcp_test"; "version", String "0" ] ])
  in
  notify client "notifications/initialized";
  result

(* Waits for a notification (already received or still in flight). *)
let expect_notification client meth =
  let is_meth message = Json.member "method" message = Some (String meth) in
  let rec wait () =
    match List.find_opt is_meth client.notifications with
    | Some _ -> client.notifications <- List.filter (fun m -> not (is_meth m)) client.notifications
    | None ->
        client.notifications <- client.notifications @ [ parse (read_line ~timeout:30. client) ];
        wait ()
  in
  wait ()

let tools_list client =
  List.map (fun tool -> str (field "name" tool)) (items (field "tools" (request client "tools/list" (Object []))))

let call client name arguments =
  request client "tools/call" (Object [ "name", String name; "arguments", Object arguments ])

let is_error result = Json.member "isError" result = Some (Bool true)
let structured result = field "structuredContent" result

let text_of result =
  String.concat "\n"
    (List.filter_map (fun block -> if Json.member "type" block = Some (String "text") then Some (str (field "text" block)) else None)
       (items (field "content" result)))

let ok_call client name arguments =
  let result = call client name arguments in
  if is_error result then Alcotest.failf "%s failed: %s" name (text_of result);
  result

let failed_call client name arguments =
  let result = call client name arguments in
  if not (is_error result) then Alcotest.failf "%s should have failed: %s" name (Json.to_string result);
  text_of result

(* Closes the server's stdin and waits for it to exit. *)
let finish client =
  Unix.close client.to_server;
  let deadline = Unix.gettimeofday () +. 30. in
  let rec wait () =
    match Unix.waitpid [ Unix.WNOHANG ] client.pid with
    | 0, _ when Unix.gettimeofday () < deadline -> Unix.sleepf 0.05; wait ()
    | 0, _ -> Unix.kill client.pid Sys.sigkill; ignore (Unix.waitpid [] client.pid); Alcotest.fail "server did not exit"
    | _, status -> status
  in
  let status = wait () in
  Unix.close client.from_server;
  match status with
  | Unix.WEXITED 0 -> ()
  | _ -> Alcotest.failf "server did not exit cleanly; stderr: %s" (read_file client.stderr_file)

let with_f3recomp ?(args = []) body =
  let client = spawn ([ f3recomp; "mcp" ] @ args) in
  Fun.protect ~finally:(fun () -> try Unix.kill client.pid Sys.sigkill with Unix.Unix_error _ -> ())
    (fun () -> ignore (initialize client); body client; finish client)

(* ---- the fixture game ---- *)

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
  (* reset: lea $410000,a5; bsr.w $420; move.w #1,$4a0000; move.w $410010,d0; jsr $430; rts *)
  put 0x400 (words [0x4bf9; 0x0041; 0x0000; 0x6100; 0x0018; 0x33fc; 0x0001; 0x004a; 0x0000;
                    0x3039; 0x0041; 0x0010; 0x4eb9; 0x0000; 0x0430; 0x4e75]);
  (* move.w #$ff,$10(a5); tst.w $10(a5); rts *)
  put 0x420 (words [0x3b7c; 0x00ff; 0x0010; 0x4a6d; 0x0010; 0x4e75]);
  (* movea.l $20(a5),a0; jsr (a0); rts *)
  put 0x430 (words [0x206d; 0x0020; 0x4e90; 0x4e75]);
  put 0x500 (words [0x4e73]);
  Bytes.to_string bytes

let fixture = lazy (
  let dir = Lazy.force scratch in
  write_file (Filename.concat dir "main") rom;
  let config = Filename.concat dir "config.toml" in
  let crc = Image.crc32 { Image.base = 0; bytes = rom } in
  write_file config (Printf.sprintf {|[game]
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
|} crc (Digestif.SHA1.(to_hex (digest_string rom))));
  [ "config", String config; "rom_dir", String dir ])

let fixture_args extra = fixture |> Lazy.force |> fun base -> base @ extra

(* ---- f3recomp mcp: protocol and static tools ---- *)

let protocol () =
  let client = spawn [ f3recomp; "mcp" ] in
  Fun.protect ~finally:(fun () -> try Unix.kill client.pid Sys.sigkill with Unix.Unix_error _ -> ())
    (fun () ->
       let init = initialize client in
       Alcotest.(check string) "server name" "f3recomp" (str (field "name" (field "serverInfo" init)));
       Alcotest.(check bool) "tools.listChanged" true
         (Json.member "listChanged" (field "tools" (field "capabilities" init)) = Some (Bool true));
       let instructions = str (field "instructions" init) in
       List.iter (fun needle -> check_contains "instructions" ~needle instructions)
         [ "session_open"; "session_close"; "tools/list_changed"; "rt_"; "--game"; "rebuilt"; "session_list" ];
       let tools = items (field "tools" (request client "tools/list" (Object []))) in
       let names = List.map (fun tool -> str (field "name" tool)) tools in
       List.iter (fun name -> Alcotest.(check bool) (name ^ " is listed") true (List.mem name names))
         [ "rom_read"; "disasm"; "inspect"; "xref"; "flow"; "coverage"; "check_irq"; "decisions"; "lower";
           "profile_merge"; "session_open"; "session_close"; "session_list"; "gameplay"; "sprite_check"; "motion" ];
       Alcotest.(check bool) "no runtime tools without a session" false (List.exists (fun n -> String.starts_with ~prefix:"rt_" n) names);
       List.iter (fun tool ->
         Alcotest.(check string) "input schema is an object" "object"
           (str (field "type" (field "inputSchema" tool)))) tools;
       let describe name = str (field "description" (List.find (fun t -> str (field "name" t) = name) tools)) in
       check_contains "session_open description" ~needle:"tools/list_changed" (describe "session_open");
       check_contains "session_open description" ~needle:"rebuilt" (describe "session_open");
       check_contains "session_close description" ~needle:"tools/list_changed" (describe "session_close");
       check_contains "session_list description" ~needle:"stale" (describe "session_list");
       (* A bad line and an unsupported request must not end the session. *)
       send_line client "this is not json";
       let unsupported = request_raw client "completion/nope" (Object []) in
       Alcotest.(check bool) "unsupported request is a JSON-RPC error" true (Json.member "error" unsupported <> None);
       let ping = request client "ping" (Object []) in
       Alcotest.(check bool) "still serving" true (ping = Object []);
       finish client)

let static_tools () =
  with_f3recomp (fun client ->
    let rom_read = structured (ok_call client "rom_read" (fixture_args [ "region", String "rom"; "addr", Int 0; "len", Int 8 ])) in
    Alcotest.(check string) "rom_read bytes" "0041000000000400" (str (field "bytes" rom_read));
    let rom_read_hex = structured (ok_call client "rom_read" (fixture_args [ "addr", String "0x400"; "len", String "0x4" ])) in
    Alcotest.(check string) "hex strings are accepted" "4bf90041" (str (field "bytes" rom_read_hex));
    let disasm = ok_call client "disasm" (fixture_args [ "pc", String "0x400"; "count", Int 2 ]) in
    Alcotest.(check (list string)) "disasm" [ "lea $410000,a5"; "bsr $000420" ]
      (List.map (fun l -> str (field "text" l)) (items (structured disasm)));
    check_contains "text content mirrors structuredContent" ~needle:"lea $410000,a5" (text_of disasm);
    let inspect = structured (ok_call client "inspect" (fixture_args [ "routines", Bool false ])) in
    Alcotest.(check int) "inferred a5" 0x410000 (int (field "a5" inspect));
    let xref = structured (ok_call client "xref" (fixture_args [ "target", String "a5+0x10"; "role", String "write" ])) in
    Alcotest.(check bool) "xref finds the write at 0x420" true
      (List.exists (fun r -> Json.member "pc" r = Some (Int 0x420)) (items (field "refs" xref)));
    let flow = structured (ok_call client "flow" (fixture_args [ "target", String "reset"; "depth", Int 1 ])) in
    Alcotest.(check int) "reset entry" 0x400 (int (field "entry" flow));
    ignore (ok_call client "coverage" (fixture_args []));
    let irq = structured (ok_call client "check_irq" (fixture_args [])) in
    Alcotest.(check string) "irq verdict" "proven" (str (field "verdict" (List.hd (items (field "handlers" irq)))));
    ignore (ok_call client "decisions" (fixture_args [ "sprite_only", Bool true ]));
    let lower = structured (ok_call client "lower" [ "pc", Int 0x1000; "hex", String "4e75"; "live_out", String "none" ]) in
    Alcotest.(check int) "lower one instruction" 1 (List.length (items (field "instructions" lower)));
    let profile = scratch_file "mcp.profile" and merged = scratch_file "mcp-merged.profile" in
    write_file profile "F3-BLOCK-PROFILE 1\n";
    ignore (ok_call client "profile_merge" [ "inputs", Array [ String profile ]; "output", String merged ]);
    Alcotest.(check bool) "merged profile written" true (Sys.file_exists merged))

let tool_errors () =
  with_f3recomp (fun client ->
    check_contains "missing argument" ~needle:"missing" (failed_call client "disasm" (fixture_args []));
    check_contains "unknown argument" ~needle:"unknown argument \"bogus\""
      (failed_call client "disasm" (fixture_args [ "pc", Int 0x400; "bogus", Int 1 ]));
    check_contains "odd pc" ~needle:"even" (failed_call client "disasm" (fixture_args [ "pc", Int 0x401 ]));
    check_contains "bad game id" ~needle:"invalid game id" (failed_call client "inspect" [ "game", String "../etc" ]);
    check_contains "unknown tool" ~needle:"unknown tool" (failed_call client "no_such_tool" []);
    check_contains "no session" ~needle:"session_open" (failed_call client "session_close" []);
    check_contains "runtime tool without a session" ~needle:"session_open" (failed_call client "rt_step" []);
    check_contains "missing binary" ~needle:"does not exist"
      (failed_call client "session_open" [ "game", String "fixture"; "tool_path", String "/nonexistent/f3rt-tool" ]);
    let listing = structured (ok_call client "session_list" []) in
    Alcotest.(check int) "no sessions" 0 (List.length (items (field "sessions" listing))))

(* ---- f3rt-tool mcp, standalone ---- *)

let png_signature = "\137PNG\r\n\026\n"

let decode_base64 text =
  let value c = match c with
    | 'A' .. 'Z' -> Char.code c - 65 | 'a' .. 'z' -> Char.code c - 71 | '0' .. '9' -> Char.code c + 4
    | '+' -> 62 | '/' -> 63 | _ -> -1 in
  let out = Buffer.create (String.length text) in
  let acc = ref 0 and bits = ref 0 in
  String.iter (fun c ->
    let v = value c in
    if v >= 0 then begin
      acc := (!acc lsl 6) lor v;
      bits := !bits + 6;
      if !bits >= 8 then begin
        bits := !bits - 8;
        Buffer.add_char out (Char.chr ((!acc lsr !bits) land 0xff))
      end
    end) text;
  Buffer.contents out

let image_block result =
  match List.find_opt (fun block -> Json.member "type" block = Some (String "image")) (items (field "content" result)) with
  | Some block -> block
  | None -> Alcotest.failf "no image block in %s" (Json.to_string result)

let check_png result =
  let block = image_block result in
  Alcotest.(check string) "image mime type" "image/png" (str (field "mimeType" block));
  let png = decode_base64 (str (field "data" block)) in
  Alcotest.(check string) "PNG signature" png_signature (String.sub png 0 8);
  Alcotest.(check string) "IHDR" "IHDR" (String.sub png 12 4);
  let be32 offset = (Char.code png.[offset] lsl 24) lor (Char.code png.[offset + 1] lsl 16)
                    lor (Char.code png.[offset + 2] lsl 8) lor Char.code png.[offset + 3] in
  Alcotest.(check int) "width" 320 (be32 16);
  Alcotest.(check bool) "height" true (be32 20 > 0)

let runtime_names = [ "init"; "shutdown"; "reset"; "step"; "set_input"; "regs"; "read"; "write"; "crc"; "audio_stats";
                      "save_state"; "load_state"; "screenshot"; "sprites"; "video_diff" ]

let standalone_runtime () =
  let _, tool, rom_dir = need_runtime () in
  let client = spawn [ tool; "mcp"; "--no-init" ] in
  Fun.protect ~finally:(fun () -> try Unix.kill client.pid Sys.sigkill with Unix.Unix_error _ -> ())
    (fun () ->
       let init = initialize client in
       Alcotest.(check bool) "instructions" true (String.length (str (field "instructions" init)) > 0);
       let tools = items (field "tools" (request client "tools/list" (Object []))) in
       let names = List.map (fun t -> str (field "name" t)) tools in
       List.iter (fun n -> Alcotest.(check bool) (n ^ " is listed") true (List.mem n names)) runtime_names;
       List.iter (fun t ->
         Alcotest.(check string) "input schema" "object" (str (field "type" (field "inputSchema" t)));
         Alcotest.(check bool) "description" true (Json.member "description" t <> None)) tools;
       check_contains "before init" ~needle:"not initialized" (failed_call client "regs" []);
       let init = structured (ok_call client "init" [ "rom_dir", String rom_dir ]) in
       Alcotest.(check int) "frame 0" 0 (int (field "frame" init));
       let step = structured (ok_call client "step" [ "frames", Int 20 ]) in
       Alcotest.(check int) "frames run" 20 (int (field "frames_run" step));
       Alcotest.(check int) "frame" 20 (int (field "frame" step));
       let state_crc = str (field "state_crc" step) in
       let regs = structured (ok_call client "regs" []) in
       Alcotest.(check int) "d registers" 8 (List.length (items (field "d" regs)));
       Alcotest.(check string) "regs does not change the state" state_crc
         (str (field "state_crc" (structured (ok_call client "crc" []))));
       (* writes and reads, with hex strings and integers, scalars and regions *)
       let write = structured (ok_call client "write" [ "addr", String "0x401000"; "size", String "w"; "value", String "0xbeef" ]) in
       Alcotest.(check int) "written value" 0xbeef (int (field "value" write));
       Alcotest.(check int) "read word" 0xbeef (int (field "value" (structured (ok_call client "read" [ "addr", String "$401000"; "size", Int 2 ]))));
       Alcotest.(check int) "read byte" 0xbe (int (field "value" (structured (ok_call client "read" [ "addr", Int 0x401000 ]))));
       ignore (ok_call client "write" [ "region", String "ram"; "offset", String "0x1100"; "hex", String "0123abcd" ]);
       Alcotest.(check string) "read region" "0123abcd"
         (str (field "hex" (structured (ok_call client "read" [ "region", String "ram"; "offset", Int 0x1100; "length", Int 4 ]))));
       check_contains "bad size" ~needle:"size" (failed_call client "read" [ "addr", Int 0; "size", Int 3 ]);
       check_contains "unknown argument" ~needle:"unknown argument" (failed_call client "step" [ "frame", Int 1 ]);
       (* checkpoint and replay *)
       let checkpoint_crc = str (field "state_crc" (structured (ok_call client "crc" []))) in
       let saved = structured (ok_call client "save_state" [ "slot", String "a"; "inline", Bool true ]) in
       Alcotest.(check bool) "inline state" true (String.length (str (field "base64" saved)) > 1000);
       let after = structured (ok_call client "step" [ "frames", Int 10 ]) in
       let loaded = structured (ok_call client "load_state" [ "slot", String "a" ]) in
       Alcotest.(check string) "load_state restores state_crc" checkpoint_crc (str (field "state_crc" loaded));
       let again = structured (ok_call client "step" [ "frames", Int 10 ]) in
       Alcotest.(check string) "replay is deterministic" (str (field "state_crc" after)) (str (field "state_crc" again));
       let from_base64 = structured (ok_call client "load_state" [ "base64", saved |> field "base64" ]) in
       Alcotest.(check string) "load_state from base64" checkpoint_crc (str (field "state_crc" from_base64));
       check_contains "empty slot" ~needle:"no saved state" (failed_call client "load_state" [ "slot", String "nope" ]);
       (* screenshot: a native image block and metadata *)
       let shot_path = scratch_file "shot.png" in
       let shot = ok_call client "screenshot" [ "path", String shot_path ] in
       check_png shot;
       Alcotest.(check int) "metadata width" 320 (int (field "width" (structured shot)));
       Alcotest.(check string) "file is a PNG" png_signature (String.sub (read_file shot_path) 0 8);
       (* sprites *)
       let sprites = structured (ok_call client "sprites" [ "drawn_only", Bool false; "limit", Int 8 ]) in
       Alcotest.(check int) "entries visited" 8 (List.length (items (field "entries" sprites)));
       Alcotest.(check bool) "summary" true (Json.member "drawn" (field "summary" sprites) <> None);
       Alcotest.(check bool) "writers off" true (field "writers" sprites = Null);
       let raw = structured (ok_call client "sprites" [ "raw", Bool true; "limit", Int 1 ]) in
       Alcotest.(check int) "raw sprite RAM" (0x10000 * 2) (String.length (str (field "hex" raw)));
       ignore (ok_call client "set_input" [ "port", Int 0; "mask", Int 1; "pressed", Bool true ]);
       ignore (ok_call client "audio_stats" []);
       ignore (ok_call client "reset" []);
       ignore (ok_call client "shutdown" []);
       check_contains "after shutdown" ~needle:"not initialized" (failed_call client "step" []);
       (* init again: the old machine's leases must be free *)
       ignore (ok_call client "init" [ "rom_dir", String rom_dir ]);
       ignore (ok_call client "init" [ "rom_dir", String rom_dir ]);
       finish client)

(* ---- f3recomp mcp bridging a runtime session ---- *)

let copy_file source destination =
  write_file destination (read_file source);
  Unix.chmod destination 0o755

let session_lifecycle () =
  let root, tool, rom_dir = need_runtime () in
  let session_args extra = [ "game", String game; "tool_path", String tool; "rom_dir", String rom_dir ] @ extra in
  let a5_args = [ "--root"; root ] in
  with_f3recomp ~args:a5_args (fun client ->
    let before = tools_list client in
    Alcotest.(check bool) "no runtime tools yet" false (List.mem "rt_step" before);
    let opened = structured (ok_call client "session_open" (session_args [])) in
    Alcotest.(check string) "session id defaults to the game" game (str (field "session_id" opened));
    expect_notification client "notifications/tools/list_changed";
    let after = tools_list client in
    List.iter (fun n -> Alcotest.(check bool) ("rt_" ^ n ^ " is listed") true (List.mem ("rt_" ^ n) after)) runtime_names;
    let listed = items (field "tools" (request client "tools/list" (Object []))) in
    let rt_step = List.find (fun t -> str (field "name" t) = "rt_step") listed in
    Alcotest.(check bool) "session_id in the bridged schema" true
      (Json.member "session_id" (field "properties" (field "inputSchema" rt_step)) <> None);
    (* forwarded calls *)
    let step = structured (ok_call client "rt_step" [ "frames", Int 20 ]) in
    Alcotest.(check int) "rt_step" 20 (int (field "frame" step));
    let state_crc = str (field "state_crc" step) in
    Alcotest.(check int) "rt_regs" 8 (List.length (items (field "d" (structured (ok_call client "rt_regs" [])))));
    Alcotest.(check string) "explicit session_id" state_crc
      (str (field "state_crc" (structured (ok_call client "rt_crc" [ "session_id", String game ]))));
    (* a5-relative addresses resolve through the game's inferred a5 *)
    let a5 = int (field "a5" (structured (ok_call client "inspect" [ "game", String game; "routines", Bool false ]))) in
    ignore (ok_call client "rt_write" [ "addr", Int (a5 - 0x60ac); "size", Int 2; "value", Int 0x1234 ]);
    let read = structured (ok_call client "rt_read" [ "addr", String "a5-0x60ac"; "size", Int 2 ]) in
    Alcotest.(check int) "a5-0x60ac resolved" (a5 - 0x60ac) (int (field "addr" read));
    Alcotest.(check int) "value written through the absolute address" 0x1234 (int (field "value" read));
    ignore (ok_call client "rt_write" [ "addr", String "a5+0x10"; "size", Int 1; "value", Int 7 ]);
    Alcotest.(check int) "a5+0x10" 7
      (int (field "value" (structured (ok_call client "rt_read" [ "addr", Int (a5 + 0x10) ]))));
    check_contains "unknown session" ~needle:"no open session" (failed_call client "rt_step" [ "session_id", String "nope" ]);
    (* image passthrough *)
    let shot = ok_call client "rt_screenshot" [] in
    check_png shot;
    Alcotest.(check int) "structured metadata passes through" 320 (int (field "width" (structured shot)));
    (* tool errors pass through as errors *)
    check_contains "runtime error" ~needle:"size" (failed_call client "rt_read" [ "addr", Int 0; "size", Int 3 ]);
    let listing = structured (ok_call client "session_list" []) in
    (match items (field "sessions" listing) with
     | [ session ] ->
         Alcotest.(check string) "binary fresh" "fresh" (str (field "binary_status" session));
         Alcotest.(check bool) "alive" true (field "alive" session = Bool true)
     | _ -> Alcotest.fail "expected one session");
    (* close *)
    let closed = structured (ok_call client "session_close" []) in
    Alcotest.(check string) "closed" game (str (field "closed" closed));
    check_contains "child exited" ~needle:"exited with status 0" (str (field "process" closed));
    expect_notification client "notifications/tools/list_changed";
    Alcotest.(check bool) "runtime tools removed" false (List.mem "rt_step" (tools_list client));
    check_contains "rt_ call after close" ~needle:"session_open" (failed_call client "rt_step" []);
    (* reopening works, and the old machine's audio/EEPROM leases were released *)
    ignore (ok_call client "session_open" (session_args []));
    expect_notification client "notifications/tools/list_changed";
    ignore (ok_call client "rt_step" [ "frames", Int 1 ])
    (* The session is left open: `finish` closes the server's stdin, and the server must end its child and
       exit 0. *))

let stale_binary () =
  let root, tool, rom_dir = need_runtime () in
  let copy_args copy = [ "game", String game; "tool_path", String copy; "session_id", String "copy"; "rom_dir", String rom_dir ] in
  with_f3recomp ~args:[ "--root"; root ] (fun client ->
    let copy = scratch_file "f3rt-tool-copy" in
    copy_file tool copy;
    ignore (ok_call client "session_open" (copy_args copy));
    expect_notification client "notifications/tools/list_changed";
    ignore (ok_call client "rt_step" [ "frames", Int 2 ]);
    (* touch: the mtime changes *)
    let later = Unix.gettimeofday () +. 100. in
    Unix.utimes copy later later;
    let message = failed_call client "rt_step" [ "frames", Int 1 ] in
    check_contains "stale message" ~needle:"changed on disk" message;
    check_contains "stale message" ~needle:"session_close" message;
    check_contains "stale message" ~needle:"session_open" message;
    let listing = structured (ok_call client "session_list" []) in
    Alcotest.(check string) "session_list reports stale" "stale"
      (str (field "binary_status" (List.hd (items (field "sessions" listing)))));
    ignore (failed_call client "rt_regs" []);
    let closed = structured (ok_call client "session_close" [ "session_id", String "copy" ]) in
    check_contains "closed cleanly" ~needle:"exited with status 0" (str (field "process" closed));
    (* replace by rename: new inode and size *)
    ignore (ok_call client "session_open" (copy_args copy));
    ignore (ok_call client "rt_step" [ "frames", Int 1 ]);
    let replacement = scratch_file "f3rt-tool-replacement" in
    write_file replacement (read_file copy ^ "\000");
    Unix.chmod replacement 0o755;
    Unix.rename replacement copy;
    check_contains "replaced binary" ~needle:"changed on disk" (failed_call client "rt_step" [ "frames", Int 1 ]);
    ignore (ok_call client "session_close" [ "session_id", String "copy" ]);
    (* missing: refused too, and the status says so *)
    copy_file tool copy;
    ignore (ok_call client "session_open" (copy_args copy));
    Sys.remove copy;
    check_contains "missing binary" ~needle:"missing" (failed_call client "rt_step" [ "frames", Int 1 ]);
    let listing = structured (ok_call client "session_list" []) in
    Alcotest.(check string) "session_list reports missing" "missing"
      (str (field "binary_status" (List.hd (items (field "sessions" listing)))));
    ignore (ok_call client "session_close" [ "session_id", String "copy" ]);
    Alcotest.(check bool) "tools unregistered" false (List.mem "rt_step" (tools_list client)))

let pinned_game () =
  let root, _, rom_dir = need_runtime () in
  let tool = Filename.concat root (Printf.sprintf "build/%s/f3rt-tool" game) in
  if not (Sys.file_exists tool) then Alcotest.skip ();
  with_f3recomp ~args:[ "--root"; root; "--game"; game ] (fun client ->
    Alcotest.(check bool) "rt_ tools listed from the start" true (List.mem "rt_step" (tools_list client));
    check_contains "no session yet" ~needle:"session_open" (failed_call client "rt_step" []);
    ignore (ok_call client "session_open" [ "game", String game; "rom_dir", String rom_dir ]);
    ignore (ok_call client "rt_step" [ "frames", Int 1 ]);
    ignore (ok_call client "session_close" []);
    Alcotest.(check bool) "pinned tools stay listed" true (List.mem "rt_step" (tools_list client)))

let () =
  let case name test = Alcotest.test_case name `Quick test in
  Alcotest.run "f3recomp mcp" [
    "f3recomp", [ case "protocol" protocol; case "static tools" static_tools; case "tool errors" tool_errors ];
    "f3rt-tool", [ case "standalone runtime" standalone_runtime ];
    "bridge", [ case "session lifecycle" session_lifecycle; case "stale binary" stale_binary;
                case "pinned game" pinned_game ];
  ]
