open F3recomp

let () =
  match Array.to_list Sys.argv with
  | [_; config_path; rom_dir] ->
      let config = Config.load config_path in
      let manifest = Rom_manifest.of_configs [config] in
      let image = Rom_manifest.load_region manifest ~game:config.game.id Rom ~rom_dir in
      Out_channel.output_string stdout image.bytes
  | _ ->
      prerr_endline "usage: dump_main_image CONFIG ROM_DIR";
      exit 2
