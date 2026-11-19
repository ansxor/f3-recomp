let vector_seeds space rom =
  let seeds = ref Pc.Set.empty in
  for slot = 1 to 255 do
    let pc = Pc.physical (Image.u32 rom (4 * slot)) in
    if pc >= 0x400 && pc land 1 = 0 then
      match Code_space.region space pc with
      | Rom -> seeds := Pc.Set.add pc !seeds
      | Excluded _ -> Error.fail ~where:"discovery" "vector %d points to excluded PC %#x" slot pc
      | _ -> ()
  done;
  !seeds

let decode_rom space rom =
  let rec decode pc entries =
    if not (Image.contains rom pc) then entries
    else
      let entries =
        match Code_space.region space pc with
        | Rom -> Pc.Map.add pc (Decode.at rom pc) entries
        | _ -> entries
      in
      decode (pc + 2) entries
  in
  decode rom.Image.base Pc.Map.empty

let all_aligned ~game ~entry_points ~jump_tables space =
  let rom = game.Game_image.rom in
  if Image.length rom < 1024 || Image.length rom land 1 <> 0 then
    Error.fail ~where:"discovery" "need an even ROM size and a complete vector table";
  let ram = Code_space.ram space in
  let seeds : Program.seeds = {
    vectors = vector_seeds space rom;
    configured = Pc.Set.of_list entry_points;
    ram_entries = Pc.Set.of_list (List.map (fun routine -> routine.Ram_code.entry) ram);
    ram_exits = Ram_code.exits ram;
  } in
  let configured =
    List.fold_left (fun tables (spec : Config.jump_table) ->
      match Jump_table.configured space rom spec with
      | None -> tables
      | Some table ->
          Jump_table.check table space;
          Pc.Map.add spec.address table tables)
      Pc.Map.empty jump_tables
  in
  let decoded =
    List.fold_left (fun entries routine ->
      Pc.Map.union (fun pc _ _ -> Error.fail ~where:"discovery" "overlapping code at %#x" pc)
        entries routine.Ram_code.decoded)
      (decode_rom space rom) ram
  in
  let functions, tables =
    Pc.Map.fold (fun pc entry (functions, tables) ->
      match entry with
      | Decode.Illegal _ -> functions, tables
      | Decode.Insn insn ->
          let functions =
            match insn.op with
            | Bsr _ | Jsr _ ->
                List.fold_left (fun functions target ->
                  if Pc.Map.mem target decoded then Pc.Set.add target functions else functions)
                  functions (Flow.of_insn insn).targets
            | _ -> functions
          in
          let tables =
            if Pc.Map.mem pc tables || Code_space.region space pc <> Rom then tables
            else match Jump_table.scan space insn with
              | None -> tables
              | Some table ->
                  Jump_table.check table space;
                  Pc.Map.add pc table tables
          in
          functions, tables)
      decoded (Pc.Set.union seeds.vectors seeds.configured, configured)
  in
  { Program.game; space; decoded; functions; tables; seeds }
