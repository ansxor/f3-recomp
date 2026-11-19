type t = (string * string) list

let rec make_directory path =
  try Unix.mkdir path 0o777 with
  | Unix.Unix_error (Unix.ENOENT, _, _) when Filename.dirname path <> path ->
      make_directory (Filename.dirname path);
      make_directory path
  | Unix.Unix_error (Unix.EEXIST, _, _) when Sys.is_directory path -> ()

let write_if_changed path contents =
  let unchanged =
    Sys.file_exists path
    && In_channel.with_open_bin path In_channel.input_all = contents
  in
  if not unchanged then Out_channel.with_open_bin path (fun out -> output_string out contents)

let write ~dir files =
  make_directory dir;
  List.iter (fun (name, contents) -> write_if_changed (Filename.concat dir name) contents) files
