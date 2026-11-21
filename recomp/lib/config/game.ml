type t = Game_image.t = {
  config : Config.t;
  rom : Image.t;
  exclusions : Exclusion.t list;
}

let load path ~rom_dir =
  let config = Config.load path in
  let manifest = Rom_manifest.of_configs [config] in
  let rom = Rom_manifest.load_region manifest ~game:config.game.id Rom ~rom_dir in
  let exclusions = Exclusion.of_config ~image:rom config.exclusions.main in
  { config; rom; exclusions }

let discover game =
  let { Config.entry_points; jump_tables } = game.config.discovery in
  let ram = Ram_code.decode game.config.ram_code game.rom in
  Code_space.of_game game ~ram
  |> Discovery.all_aligned ~game ~entry_points ~jump_tables
