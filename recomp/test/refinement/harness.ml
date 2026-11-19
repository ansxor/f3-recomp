open F3recomp

type entry = { pc : int; sem : Sem.t }
type liveness = entry list -> (int -> Ccr.t)
type result = Passed of int | Mismatch of string

module Pc_map = Map.Make (Int)

let rec successors (stmt : Sem.stmt) =
  match stmt with
  | Goto (Const pc) -> [Some pc]
  | Goto _ | Raise _ | Stop _ -> [None]
  | If (_, yes, no) -> List.concat_map successors (yes @ no)
  | _ -> []

let local_liveness entries =
  let analyse { pc; sem } (inputs, outputs) =
    let successor_live = function
      | None -> Ccr.all
      | Some target ->
          if target <= pc && List.exists (fun entry -> entry.pc = target) entries then
            invalid_arg "local liveness requires forward branches";
          Pc_map.find_opt target inputs |> Option.value ~default:Ccr.all
    in
    let stmts = match sem with
      | Sem.Body stmts -> stmts
      | Unsupported reason -> invalid_arg reason
    in
    let outgoing = List.concat_map successors stmts in
    let live_out =
      match outgoing with
      | [] -> Ccr.all
      | edges ->
          List.fold_left (fun live edge -> Ccr.union live (successor_live edge)) Ccr.none edges
    in
    let exit = Effects.make ~reads:live_out () in
    let live_in = (List.fold_right (Effects.before ~exit) stmts exit).reads in
    Pc_map.add pc live_in inputs, Pc_map.add pc live_out outputs
  in
  let _, outputs = List.fold_right analyse entries (Pc_map.empty, Pc_map.empty) in
  fun pc -> Pc_map.find pc outputs

let decode (image : Image.t) =
  let limit = image.base + String.length image.bytes in
  let rec loop pc =
    if pc = limit then []
    else
      let decoded = Decode.at image pc in
      let length = match decoded with Insn insn -> insn.length | Illegal illegal -> illegal.length in
      if length <= 0 || pc + length > limit then invalid_arg "incomplete refinement snippet";
      { pc; sem = Semantics.of_decoded ~pc decoded } :: loop (pc + length)
  in
  loop image.base

let emit out name live entries =
  List.iteri (fun index { pc; sem } ->
      let body = match Lower.entry sem ~live_out:(live pc) with
        | Ok entry -> entry.stmts
        | Error reason -> failwith (Printf.sprintf "cannot lower %#x: %s" pc reason)
      in
      output_string out (C.print_func { name = Printf.sprintf "%s_%d" name index; body }))
    entries;
  Printf.fprintf out "static void %s(f3_cpu *cpu) {\n" name;
  output_string out "    for (;;) {\n        uint32_t pc = cpu->pc;\n        switch (pc) {\n";
  List.iteri (fun index { pc; _ } ->
      Printf.fprintf out "        case %#xu: %s_%d(cpu); break;\n" pc name index)
    entries;
  output_string out
    "        default: return;\n        }\n        if (cpu->pc <= pc || cpu->stopped || ((Test_bus *)cpu->runtime)->raised) return;\n    }\n}\n"

let rec repo_root dir =
  if Sys.file_exists (Filename.concat dir "include/f3rt/cpu_abi.h") then dir
  else
    let parent = Filename.dirname dir in
    if parent = dir then failwith "cannot locate runtime headers";
    repo_root parent

let process command args =
  let channel = Unix.open_process_args_in command (Array.of_list (command :: args)) in
  let output = In_channel.input_all channel in
  Unix.close_process_in channel, output

let run ?(liveness = local_liveness) ~seed ~samples ~base program =
  let entries = F3recomp_test.Asm.assemble ~base program |> decode in
  if entries = [] || samples < 1 then invalid_arg "refinement needs instructions and samples";
  let live = liveness entries in
  let source, out = Filename.open_temp_file ~temp_dir:"." "refinement-" ".c" in
  let binary = source ^ ".exe" in
  Fun.protect ~finally:(fun () ->
      close_out_noerr out;
      Sys.remove source;
      if Sys.file_exists binary then Sys.remove binary)
    (fun () ->
      output_string out Fixture.source;
      emit out "eager" (Fun.const Ccr.all) entries;
      emit out "lazy" live entries;
      Printf.fprintf out "int main(void) {\n    uint32_t rng = %#xu;\n" (seed land 0xffffffff);
      output_string out "    const uint32_t entries[] = {";
      List.iter (fun { pc; _ } -> Printf.fprintf out " %#xu," pc) entries;
      Printf.fprintf out " };\n    return compare(eager, lazy, &rng, entries, %d, %d);\n}\n"
        (List.length entries) samples;
      close_out out;
      let root = repo_root (Sys.getcwd ()) in
      let cc = Sys.getenv_opt "CC" |> Option.value ~default:"cc" in
      (match process cc ["-std=gnu11"; "-O1"; "-I"; Filename.concat root "include";
                         "-I"; Filename.concat root "recomp"; source; "-o"; binary] with
       | Unix.WEXITED 0, _ -> ()
       | _ -> failwith "refinement runner failed to compile");
      match process binary [] with
      | Unix.WEXITED 0, _ -> Passed (List.length entries * samples)
      | Unix.WEXITED 1, output when output <> "" -> Mismatch (String.trim output)
      | _ -> failwith "refinement runner failed to execute")
