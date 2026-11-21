open F3recomp

let fixture ?(rom_extra = "") ?(lane_extra = "") ?(regions = "") ?(extra = "") () =
  {|[game]
id = "fixture"
[rom]
size = 0x1000
interleave = 1
|}
  ^ rom_extra
  ^ {|[[rom.lanes]]
file = "program"
size = 0x1000
crc = "12345678"
sha1 = "0123456789abcdef0123456789abcdef01234567"
|}
  ^ lane_extra
  ^ {|[sprites_hi]
size = 0
[tiles]
size = 0
[tiles_hi]
size = 0
[sound]
size = 0
[samples]
size = 0
|}
  ^ (if regions = "" then "[sprites]\nsize = 0\n" else regions)
  ^ extra

let parse source = Config.of_string ~path:"fixture/config.toml" source

let rejects (name, expected, attempt) =
  Alcotest.test_case name `Quick (fun () ->
    match attempt () with
    | () -> Alcotest.fail "expected Error.Invalid"
    | exception Error.Invalid { where; what } ->
        let message = where ^ ": " ^ what in
        let found =
          try ignore (Str.search_forward (Str.regexp_string expected) message 0); true
          with Not_found -> false
        in
        if not found then Alcotest.failf "expected %S in %S" expected message)

let rejects_config (name, expected, source) =
  rejects (name, expected, fun () -> ignore (parse source))

let rec find_games dir =
  let games = Filename.concat dir "games" in
  if Sys.file_exists games && Sys.is_directory games then games
  else
    let parent = Filename.dirname dir in
    if parent = dir then Alcotest.fail "could not find the repository games directory";
    find_games parent

let shipped_configs () =
  let games = find_games (Sys.getcwd ()) in
  let paths =
    Sys.readdir games |> Array.to_list |> List.sort String.compare
    |> List.filter_map (fun game ->
           let path = Filename.concat (Filename.concat games game) "config.toml" in
           if Sys.file_exists path then Some path else None)
  in
  Alcotest.(check bool) "found the shipped configs" true (List.length paths >= 22);
  Config.load_all paths
  |> List.iter (fun config ->
         let game = Filename.basename (Filename.dirname config.Config.path) in
         Config.require_id game config)

let lane = {|file = "chip"
size = 4
crc = "12345678"
sha1 = "0123456789abcdef0123456789abcdef01234567"
|}

let rom_cases =
  [ "chip in absent region", "exceeds region",
      fixture ~regions:("[sprites]\nsize = 0\n[[sprites.lanes]]\n" ^ lane) ();
    "group exceeds region", "exceeds region",
      fixture ~regions:("[sprites]\nsize = 10\n[[sprites.lanes]]\n" ^ lane
                        ^ "offset = 3\ngroup = 2\nstride = 6\n") ();
    "unknown rom key", "fixture/config.toml.rom: unknown keys: bogus",
      fixture ~rom_extra:"bogus = 1\n" ();
    "unknown lane key", "fixture/config.toml.rom.lanes[0]: unknown keys: typo",
      fixture ~lane_extra:"typo = 1\n" () ]

let exclusion ?(start = "0x800") ?(stop = "0x900") ?(evidence = "e") ?(fields = "") () =
  Printf.sprintf "[[exclude]]\nstart = %s\nend = %s\nreason = %S\nevidence = %S\n%s"
    start stop "r" evidence fields

let exclusion_cases =
  let config extra = fixture ~extra () in
  [ "odd bounds", "invalid exclusion interval", config (exclusion ~start:"0x801" ());
    "interval outside ROM", "invalid exclusion interval", config (exclusion ~stop:"0x1002" ());
    "empty evidence", "requires nonempty evidence", config (exclusion ~evidence:"" ());
    "overlap", "overlapping exclusions", config (exclusion () ^ exclusion ());
    "explicit excluded target", "excluded PC 0x800",
      config (exclusion () ^ "[[discovery.jump_tables]]\naddress = 0x500\ntargets = [0x800]\n") ]
  @ List.map
      (fun fill ->
        "invalid fill " ^ fill, "fill must be a byte value",
        config (exclusion ~fields:("fill = " ^ fill ^ "\n") ()))
      ["256"; "-1"; "true"; "1.5"]

let image_checks =
  let spec = { Exclusion.start = 0x800; stop = 0x1000; reason = "r"; evidence = "e"; fill = Some 255 } in
  let image = { Image.base = 0; bytes = String.make 0x2000 '\xff' } in
  let sound = { spec with start = 0xc00800; stop = 0xc01000; fill = Some 0 } in
  let sound_image = { Image.base = 0xc00000; bytes = String.make 0x2000 '\x00' } in
  let corrupt image offset byte =
    let bytes = Bytes.of_string image.Image.bytes in
    Bytes.set bytes offset byte;
    { image with bytes = Bytes.to_string bytes }
  in
  let check image spec = ignore (Exclusion.of_config ~image [spec]) in
  [ Alcotest.test_case "matching fill and 24-bit alias" `Quick (fun () ->
      check image spec;
      check sound_image sound;
      Alcotest.(check bool) "aliased start is included" true
        (Exclusion.find [spec] 0xff000800 = Some spec);
      Alcotest.(check bool) "stop is excluded from interval" true
        (Exclusion.find [spec] 0xff001000 = None));
    rejects ("contradicted fill", "fill 0xff contradicted by byte 0x00 at 0xa02",
      fun () -> check (corrupt image 0xa02 '\x00') spec);
    rejects ("contradicted sound fill", "contradicted by byte 0x01 at 0xc00900",
      fun () -> check (corrupt sound_image 0x900 '\x01') sound);
    rejects ("fill without image", "no program image",
      fun () -> Exclusion.check ~image:None sound) ]

let shipped_exclusion_fills () =
  let games = find_games (Sys.getcwd ()) in
  let root = Filename.dirname games in
  let rom_root =
    Option.value (Sys.getenv_opt "F3_RECOMP_ROM_ROOT") ~default:(Filename.concat root "roms") in
  let rom_dir = Filename.concat rom_root "landmakrj" in
  if not (Sys.file_exists rom_dir) then Alcotest.skip ();
  let path = Filename.concat games "landmakrj/config.toml" in
  let config = Config.load path in
  let manifest = Rom_manifest.of_configs [config] in
  let load region base specs =
    let image = Rom_manifest.load_region manifest ~game:config.game.id region ~rom_dir in
    Exclusion.of_config ~image:{ image with base } specs in
  let main = load Rom 0 config.exclusions.main in
  let sound = load Sound 0xc00000 config.exclusions.sound in
  Alcotest.(check int) "verified fills across both CPUs" 4
    (List.length (List.filter (fun (range : Exclusion.t) -> Option.is_some range.fill) (main @ sound)))

let unit_record ?(name = "\"objects\"") ?(start = "0x400") ?(stop = "0x40a")
    ?(register = "unit = \"a6\"\n") ?(size = "0x80") ?(extra = "") () =
  Printf.sprintf "[[video.emit_units]]\nname = %s\nstart = %s\nend = %s\n%s size = %s\n%s"
    name start stop register size extra

let sprite_cases =
  [ "odd start", "must be even", unit_record ~start:"0x401" ();
    "odd end", "must be even", unit_record ~stop:"0x40b" ();
    "unpaired hooks", "start has 2 entries, end has 1",
      unit_record ~start:"[0x400, 0x500]" ~stop:"[0x40a]" ();
    "duplicate unit name", "duplicate emit unit name", unit_record () ^ unit_record ();
    "a7 register", "unit register", unit_record ~register:"unit = \"a7\"\n" ();
    "sp register", "unit register", unit_record ~register:"unit = \"sp\"\n" ();
    "invalid identifier", "invalid unit name", unit_record ~name:"\"1bad\"" ();
    "reserved identifier", "invalid unit name", unit_record ~name:"\"all\"" ();
    "reversed hooks", "must be below", unit_record ~start:"0x40a" ~stop:"0x400" ();
    "negative unit size", "size must be a non-negative integer", unit_record ~size:"-1" ();
    "extra unit key", "unknown keys: extra", unit_record ~extra:"extra = 1\n" ();
    "retired owner key", "unknown keys: owner", unit_record ~extra:"owner = \"unit\"\n" ();
    "missing register", "missing 'unit'", unit_record ~register:"" ();
    "duplicate hook PC", "duplicate start or end PC", unit_record ~start:"[2, 2]" ~stop:"[4, 6]" () ]
  |> List.map (fun (name, expected, extra) -> name, expected, fixture ~extra ())

let sprite_spec extra = fixture ~extra () |> parse |> Emit_units.of_config

let sprite_queue = {|[[video.emit_units]]
name = "queue"
start = [2, 4]
end = [6, 8]
unit = "d0"
|}

let sprite_defaults_and_hooks () =
  let config = fixture ~extra:(unit_record () ^ sprite_queue) () |> parse in
  let units = config.Config.video.emit_units in
  Alcotest.(check (list int)) "IDs follow declaration order" [0; 1]
    (List.map (fun (unit : Config.emit_unit) -> unit.id) units);
  Alcotest.(check (list int)) "sizes include the default" [128; 0]
    (List.map (fun (unit : Config.emit_unit) -> unit.size) units);
  let check_hooks = Alcotest.(check (list (pair int (list int)))) in
  let enter, leave = config |> Emit_units.of_config |> Emit_units.hooks in
  check_hooks "enter map" [2, [1]; 4, [1]; 0x400, [0]] (Pc.Map.bindings enter);
  check_hooks "exit map" [6, [1]; 8, [1]; 0x40a, [0]] (Pc.Map.bindings leave);
  let shared =
    unit_record () ^ unit_record ~name:"\"second\"" ()
    |> sprite_spec |> Emit_units.hooks
  in
  check_hooks "shared starts retain declaration order" [0x400, [0; 1]]
    (Pc.Map.bindings (fst shared));
  check_hooks "shared ends retain declaration order" [0x40a, [0; 1]]
    (Pc.Map.bindings (snd shared))

let sprite_digest () =
  let digest rows = sprite_spec rows |> Emit_units.digest in
  let base = unit_record () in
  let expected = digest base in
  let check = Alcotest.(check int32) in
  check "empty canonical CRC32" 0xf2768d41l (digest "");
  check "populated canonical CRC32" 0xf1a6cfe9l expected;
  check "stable" expected (digest base);
  check "ignores discovery" expected (digest (base ^ "[discovery]\ncoverage = \"all_aligned\"\n"));
  let retired = "[video]\nframe_writers = { ranges = [[0x10, 0x20]] }\n" ^ base in
  check "ignores retired frame-writer table" expected (digest retired);
  List.iter
    (fun (field, changed) -> Alcotest.(check bool) field true (expected <> digest changed))
    [ "size", unit_record ~size:"0x40" ();
      "end", unit_record ~stop:"0x40c" ();
      "register", unit_record ~register:"unit = \"a5\"\n" ();
      "name", unit_record ~name:"\"renamed\"" ();
      "added unit", base ^ sprite_queue;
      "no units", "" ]

let compile_header ?(valid = true) headers source =
  let dir = Filename.temp_file "f3-config-header" "" in
  Sys.remove dir;
  Unix.mkdir dir 0o700;
  Fun.protect ~finally:(fun () ->
    Sys.readdir dir |> Array.iter (fun name -> Sys.remove (Filename.concat dir name));
    Unix.rmdir dir) (fun () ->
    Output.write ~dir (("check.cpp", source) :: headers);
    let include_path = Filename.concat (Filename.dirname (find_games (Sys.getcwd ()))) "include" in
    let compiler = "g++" in
    let args = [|compiler; "-std=c++20"; "-Werror"; "-fsyntax-only"; "-I"; include_path;
                 Filename.concat dir "check.cpp"|] in
    let pid = Unix.create_process compiler args Unix.stdin Unix.stdout Unix.stderr in
    Alcotest.(check bool) "C++ compilation" valid (snd (Unix.waitpid [] pid) = Unix.WEXITED 0))

let sprite_render_empty () =
  let config = fixture () |> parse in
  Alcotest.(check int) "no configured units" 0 (List.length config.video.emit_units);
  compile_header (Emit_units.render (Emit_units.of_config config)) {|
#include "sprite_units.hpp"
using namespace f3rt::sprite_units;
static_assert(all.empty() && F3_SPRITE_UNIT_COUNT == 0);
static_assert(digest == 0xf2768d41u);
|}

let sprite_render_units () =
  let rows = unit_record () ^ sprite_queue ^ "size = 18\n" in
  compile_header (rows |> sprite_spec |> Emit_units.render) {|
#include "sprite_units.hpp"
#include <string_view>
using namespace f3rt::sprite_units;
using f3rt::UnitRegister;
static_assert(F3_SPRITE_UNIT_COUNT == 2);
static_assert(all[0] == &objects && all[1] == &queue && valid_units());
static_assert(objects.id == 0 && std::string_view(objects.name) == "objects");
static_assert(objects.reg == UnitRegister::A6 && objects.size == 128);
static_assert(queue.id == 1 && std::string_view(queue.name) == "queue");
static_assert(queue.reg == UnitRegister::D0 && queue.size == 18);
static_assert(queue.starts[0] == 2 && queue.starts[1] == 4);
static_assert(queue.ends[0] == 6 && queue.ends[1] == 8);
|}

let sprite_retired_frame_writers () =
  let config = fixture ~extra:("[video]\nframe_writers = \"retired\"\n" ^ unit_record ()) () |> parse in
  let names = List.map (fun (unit : Config.emit_unit) -> unit.name) config.video.emit_units in
  Alcotest.(check (list string)) "unit names" ["objects"] names

let sprite_guard () =
  let units = unit_record () |> sprite_spec in
  let guard = Emit_units.guard_lines units in
  compile_header (Emit_units.render units) guard;
  compile_header ~valid:false (Emit_units.render (sprite_spec "")) guard

let sprite_shipped_configs () =
  let games = find_games (Sys.getcwd ()) in
  List.iter
    (fun (game, expected) ->
      let path = Filename.concat (Filename.concat games game) "config.toml" in
      let config = Config.load path in
      let names = List.map (fun (unit : Config.emit_unit) -> unit.name) config.video.emit_units in
      Alcotest.(check (list string)) game expected names)
    ["commandw", ["objects"]; "landmakrj", []; "landmakr", []]

let hold ?(name = "object-shadow") ?(period = 2) ?(follow = "self")
    ?(group_writer = 0x4010) ?(anchor_writer = 0x4020) () =
  Printf.sprintf
    "[[enhancements.flicker_hold]]\nname = %S\ngroup_writer = 0x%x\nanchor_writer = 0x%x\nperiod = %d\nfollow = %S\n"
    name group_writer anchor_writer period follow

let enhancements_fixture counter holds =
  fixture ~extra:("[enhancements]\ngame_frame_counter = " ^ counter ^ "\n" ^ holds) ()

let enhancement_cases =
  List.map
    (fun counter ->
      "invalid counter " ^ counter, "aligned game_frame_counter", enhancements_fixture counter (hold ()))
    ["true"; "0x3ffffe"; "0x420000"; "0x408329"]
  @ ([ "duplicate hold name", "invalid or repeated flicker hold name", hold () ^ hold ();
       "equals in name", "invalid or repeated flicker hold name", hold ~name:"object=shadow" ();
       "period below range", "period must be in", hold ~period:1 ();
       "period above range", "period must be in", hold ~period:256 ();
       "unknown follow mode", "follow must be self or anchor", hold ~follow:"other" () ]
     |> List.map (fun (name, expected, rows) -> name, expected, enhancements_fixture "0x408328" rows))

let enhancement_groups ?(anchor_base = 11) () =
  let writer pc base x_offset group_class : Sprite_groups.writer =
    { pc; base; x_offset; group_class; cursor_reg = base; lane = XY;
      y_offset = x_offset + 2; rigid = false; root_offsets = [] }
  in
  Sprite_groups.make ~game_id:"fixture" ~instructions:0 ~analysed:0 ~dispatch_targets:0
    ~writers:[writer 0x4010 11 4 2; writer 0x4020 anchor_base 0 1]
    ~unresolved:[] ~classifications:[]

let enhancement_header_prefix = {|#pragma once
#include <array>
#include <cstdint>

namespace f3rt::game_config {
struct FlickerHold {
    const char *name;
    uint8_t group_class, anchor_class, period;
    enum class Follow : uint8_t { Self, Anchor } follow;
};
|}

let check_enhancement_header expected spec =
  let header = Enhancements.render spec |> List.assoc "enhancements.hpp" in
  Alcotest.(check string) "header" (enhancement_header_prefix ^ expected) header

let enhancement_render () =
  let config = enhancements_fixture "0x408328" (hold ()) |> parse in
  let spec = Enhancements.of_config config (enhancement_groups ()) in
  let expected : Enhancements.t =
    { game_frame_counter = 0x408328;
      flicker_holds =
        [{ name = "object-shadow"; group_class = 2; anchor_class = 1; period = 2; follow = Self }] }
  in
  Alcotest.(check bool) "resolved spec" true (spec = expected);
  compile_header (Enhancements.render spec) {|
#include "enhancements.hpp"
#include <string_view>
using namespace f3rt::game_config;
static_assert(game_frame_counter == 0x408328);
static_assert(std::size(flicker_holds) == 1);
constexpr auto hold = flicker_holds[0];
static_assert(std::string_view(hold.name) == "object-shadow");
static_assert(hold.group_class == 2 && hold.anchor_class == 1);
static_assert(hold.period == 2 && hold.follow == FlickerHold::Follow::Self);
|}

let enhancement_default () =
  let config = enhancements_fixture "0x408328" (hold ()) |> parse in
  let spec = Enhancements.default config in
  Alcotest.(check bool) "default spec" true
    (spec = { game_frame_counter = 0; flicker_holds = [] });
  compile_header (Enhancements.render spec) {|
#include "enhancements.hpp"
using namespace f3rt::game_config;
static_assert(game_frame_counter == 0 && flicker_holds.empty());
|}

let enhancement_escaped_name () =
  let spec : Enhancements.t =
    { game_frame_counter = 0x408328;
      flicker_holds =
        [{ name = "é😀\t\"\\"; group_class = 2; anchor_class = 1; period = 255; follow = Anchor }] }
  in
  check_enhancement_header {|inline constexpr uint32_t game_frame_counter = 0x408328;
inline constexpr FlickerHold flicker_holds[] = {
    {"\u00e9\ud83d\ude00\t\"\\", 2, 1, 255, FlickerHold::Follow::Anchor},
};
}
|} spec

let enhancement_group_cases =
  let resolve holds groups () =
    let config = enhancements_fixture "0x408328" holds |> parse in
    ignore (Enhancements.of_config config groups)
  in
  [ "unknown group writer", "no sprite-group write at 0x9998",
    resolve (hold ~group_writer:0x9998 ()) (enhancement_groups ());
    "unknown anchor writer", "no sprite-group write at 0x9998",
    resolve (hold ~anchor_writer:0x9998 ()) (enhancement_groups ());
    "base mismatch", "base registers differ",
    resolve (hold ()) (enhancement_groups ~anchor_base:12 ()) ]

let base_values () =
  let image = { Image.base = 0x100; bytes = "\xff\x80\x00\x01" } in
  let check = Alcotest.(check int) in
  check "u8" 255 (Image.u8 image 0x100);
  check "s8" (-128) (Image.s8 image 0x101);
  check "u16" 0xff80 (Image.u16 image 0x100);
  check "s16" (-128) (Image.s16 image 0x100);
  check "u32" 0xff800001 (Image.u32 image 0x100);
  check "s32" (-8388607) (Image.s32 image 0x100);
  Alcotest.(check string) "slice" "\x00\x01" (Image.slice image 0x102 ~length:2).bytes;
  Alcotest.(check int32) "CRC32" 0xcbf43926l (Image.crc32 { base = 0; bytes = "123456789" })

let interrupt_observers () =
  let check = Alcotest.(check string) in
  let flags config = Ccr.to_string config.Config.cpu.interrupt_observes in
  check "empty default" "" (fixture () |> parse |> flags);
  check "selected flags" "XNVC"
    (fixture ~extra:"[cpu]\ninterrupt_observes = [\"C\", \"X\", \"V\", \"N\", \"C\"]\n" () |> parse |> flags)

let () =
  Alcotest.run "config"
    [ "load",
      [ Alcotest.test_case "all shipped configs" `Quick shipped_configs;
        Alcotest.test_case "interrupt observers" `Quick interrupt_observers;
        Alcotest.test_case "image reads and CRC32" `Quick base_values;
        rejects_config ("recursive discovery", "recursive discovery is unsupported",
          fixture ~extra:"[discovery]\ncoverage = \"recursive\"\n" ()) ];
      "rom", List.map rejects_config rom_cases;
      "exclusions", List.map rejects_config exclusion_cases @ image_checks
        @ [Alcotest.test_case "shipped main and sound fills" `Quick shipped_exclusion_fills];
      "sprite units",
      List.map rejects_config sprite_cases
      @ [ Alcotest.test_case "defaults, scalar and list PCs, hook maps" `Quick sprite_defaults_and_hooks;
          Alcotest.test_case "digest parity and sensitivity" `Quick sprite_digest;
          Alcotest.test_case "empty headers" `Quick sprite_render_empty;
          Alcotest.test_case "populated headers" `Quick sprite_render_units;
          Alcotest.test_case "retired frame writers ignored" `Quick sprite_retired_frame_writers;
          Alcotest.test_case "digest guard" `Quick sprite_guard;
          Alcotest.test_case "shipped unit names" `Quick sprite_shipped_configs ];
      "enhancements",
      List.map rejects_config enhancement_cases
      @ List.map rejects enhancement_group_cases
      @ [ Alcotest.test_case "resolve holds and render header" `Quick enhancement_render;
          Alcotest.test_case "default ignores holds" `Quick enhancement_default;
          Alcotest.test_case "reference name escaping" `Quick enhancement_escaped_name ] ]
