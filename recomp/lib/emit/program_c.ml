let is_rom = function Code_space.Rom -> true | _ -> false

let entry_name (layout : Layout.t) pc (entry, tier) =
  match entry with
  | Selection.Exception_entry vector -> Layout.exception_name tier vector
  | Native _ | Ram _ | Ram_exception _ -> Pc.Map.find pc layout.owner

let header =
  "#ifndef F3_GENERATED_PROGRAM_H\n#define F3_GENERATED_PROGRAM_H\n"
  ^ "#include <f3rt/cpu_abi.h>\n" ^ Blocks_c.abi_guard
  ^ {|#ifdef __cplusplus
extern "C" {
#endif
int f3_generated_register(f3_cpu *cpu);
#ifdef __cplusplus
}
#endif
#endif
|}

let exception_definition ~storage (tier, vector) =
  Printf.sprintf {|%svoid %s(f3_cpu *cpu) {
    F3_PROFILE_HIT_MAIN(cpu->pc);
    f3_exception(cpu, %d, cpu->pc);
}
|} storage (Layout.exception_name tier vector) vector

let program (layout : Layout.t) ~external_exceptions exceptions =
  let game = layout.selection.program.game in
  let buffer = Buffer.create 4096 in
  let emit fmt = Printf.bprintf buffer fmt in
  emit "%s#include \"program.h\"\n" Blocks_c.preamble;
  List.iter (fun (func : Layout.func) -> emit "%s" (Blocks_c.declaration func.name))
    (Layout.functions layout);
  List.iter (fun ((tier, vector) as entry) ->
    if external_exceptions then emit "%s" (Blocks_c.declaration (Layout.exception_name tier vector))
    else emit "%s" (exception_definition ~storage:"static " entry)) exceptions;
  emit "static const f3_block translated_blocks[] = {\n";
  Pc.Map.iter (fun pc entry -> emit "    { 0x%08xu, %s },\n" pc (entry_name layout pc entry))
    layout.selection.entries;
  emit "};\n";
  let exclusions = game.exclusions in
  if exclusions <> [] then begin
    emit "static const f3_excluded_range excluded_ranges[] = {\n";
    List.iter (fun (range : Exclusion.t) ->
      emit "    { 0x%08xu, 0x%08xu, %s, %s },\n" range.start range.stop
        (Quoted.c range.reason) (Quoted.c range.evidence)) exclusions;
    emit "};\n"
  end;
  let ram = Code_space.ram layout.selection.program.space in
  if ram <> [] then begin
    List.iteri (fun i (routine : Ram_code.routine) ->
      emit "static const uint8_t ram_payload_%d[] = {\n" i;
      String.iteri (fun offset byte ->
        if offset mod 16 = 0 then emit "\n    ";
        emit "0x%02x, " (Char.code byte)) routine.payload.bytes;
      emit "\n};\n") ram;
    emit "static const f3_ram_code_range ram_code[] = {\n";
    List.iteri (fun i (routine : Ram_code.routine) ->
      emit "    { 0x%08xu, sizeof(ram_payload_%d), ram_payload_%d },\n" routine.destination i i) ram;
    emit "};\n"
  end;
  emit "_Static_assert(2u * F3_ENTRY_WORDS == %#xu, \"ROM dispatch table size differs from layout\");\n"
    Layout.rom_dispatch_limit;
  emit "f3_block_fn f3_entry_by_word[F3_ENTRY_WORDS];\nint f3_generated_register(f3_cpu *cpu) {\n";
  emit "    if (!f3_validate_main_rom(cpu, %du, 0x%08lxu)) return 0;\n"
    (String.length game.rom.bytes) (Image.crc32 game.rom);
  emit {|    if (!f3_register_blocks(cpu, translated_blocks,
        sizeof(translated_blocks) / sizeof(translated_blocks[0]))) return 0;
    for (size_t i = 0; i < sizeof(translated_blocks) / sizeof(translated_blocks[0]); ++i)
        if (translated_blocks[i].address < 2u * F3_ENTRY_WORDS)
            f3_entry_by_word[translated_blocks[i].address >> 1] = translated_blocks[i].execute;
|};
  if ram <> [] then
    emit "    if (!f3_register_ram_code(cpu, ram_code, sizeof(ram_code) / sizeof(ram_code[0]))) return 0;\n";
  if exclusions = [] then emit "    return f3_register_exclusions(cpu, NULL, 0);\n"
  else emit "    return f3_register_exclusions(cpu, excluded_ranges, sizeof(excluded_ranges) / sizeof(excluded_ranges[0]));\n";
  emit "}\n";
  Buffer.contents buffer

let inventory (layout : Layout.t) sources =
  let program = layout.selection.program in
  let rom = program.game.rom in
  let original = Pc.Map.fold (fun pc _ acc ->
    if is_rom (Code_space.region program.space pc) then pc :: acc else acc)
    program.decoded [] |> List.rev in
  let retained, hot = Pc.Map.fold (fun pc (_, tier) (retained, hot) ->
    if Layout.is_ram layout pc then retained, hot
    else retained + 1, hot + Bool.to_int (tier = Selection.Hot && layout.selection.mode <> Baseline))
    layout.selection.entries (0, 0) in
  let bytes = List.fold_left (fun total (_, source) -> total + String.length source) 0 sources in
  Printf.sprintf {|{
  "version": 1,
  "region": "main",
  "rom_crc32": "%08lx",
  "base": 0,
  "size": %d,
  "entries": [%s],
  "retained_entries": %d,
  "hot_entries": %d,
  "cold_entries": %d,
  "generated_c_bytes": %d
}
|} (Image.crc32 rom) (String.length rom.bytes) (String.concat ", " (List.map string_of_int original))
    retained hot (List.length original - hot) bytes

let files (layout : Layout.t) blocks =
  let exceptions = Layout.exception_entries layout in
  let external_exceptions = layout.selection.mode = Selection.Tiers in
  let for_tier tier filename =
    let entries = List.filter (fun (t, _) -> t = tier) exceptions in
    if not external_exceptions || entries = [] then []
    else [filename, Blocks_c.preamble ^
      String.concat "" (List.map (exception_definition ~storage:"") entries)] in
  let hot, cold = List.combine layout.shards blocks
    |> List.partition (fun ((shard : Layout.shard), _) -> shard.tier = Selection.Hot) in
  let hot = List.map snd hot @ for_tier Hot "exceptions_hot.c"
    @ ["program.c", program layout ~external_exceptions exceptions] in
  let cold = List.map snd cold @ for_tier Cold "exceptions_cold.c" in
  let sources = hot @ cold in
  let cmake variable files =
    "set(" ^ variable ^ "\n" ^
    String.concat "" (List.map (fun (name, _) ->
      Printf.sprintf "    \"${CMAKE_CURRENT_LIST_DIR}/%s\"\n" name) files) ^ ")\n" in
  sources @ [
    "program.h", header;
    "program.bin", layout.selection.program.game.rom.bytes;
    "sources.cmake", cmake "F3_GENERATED_SOURCES" sources
      ^ cmake "F3_GENERATED_HOT_SOURCES" hot ^ cmake "F3_GENERATED_COLD_SOURCES" cold;
    "profile_inventory.json", inventory layout sources;
  ]

let write ~dir files =
  Output.write ~dir files;
  let is_shard name =
    let has_index prefix =
      let first = String.length prefix in
      let count = String.length name - first - String.length ".c" in
      count >= 4 && String.starts_with ~prefix name && String.ends_with ~suffix:".c" name &&
      String.for_all (function '0' .. '9' -> true | _ -> false) (String.sub name first count)
    in
    List.exists has_index ["blocks_"; "blocks_hot_"; "blocks_cold_"] in
  let obsolete name =
    is_shard name || List.mem name ["exceptions_hot.c"; "exceptions_cold.c"; "coverage.json"; "lowering.json"] in
  Sys.readdir dir |> Array.iter (fun name ->
    if obsolete name && not (List.mem_assoc name files) then
      let path = Filename.concat dir name in
      if (Unix.lstat path).st_kind = Unix.S_REG then Sys.remove path)
