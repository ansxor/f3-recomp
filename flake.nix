{
  description = "f3-recomp: static recompilation of Taito F3 arcade games";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      lib = nixpkgs.lib;
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];

      cpmSources = pkgs:
        let gh = owner: repo: rev: hash: extra:
          pkgs.fetchFromGitHub ({ inherit owner repo rev hash; } // extra);
        in {
          yyjson = gh "ibireme" "yyjson" "0.12.0" "sha256-1CYnEgUMUc7eqdkv6M/KyL/MdVQBMov9HgLCycF6++w=" { };
          cpp_yyjson = gh "yosh-matsuda" "cpp-yyjson" "v1.1.0" "sha256-5mssdkHHYsSd4Wz6bjKDGLhEUD2bRYf5bw/jHK+yXuA=" { };
          httplib = gh "yhirose" "cpp-httplib" "v0.51.0" "sha256-7vifeaLAlNioumol+fUkIUO93MFxVNOY6AGZkUJVYy8=" { };
          Corrosion = gh "corrosion-rs" "corrosion" "v0.6.1" "sha256-ppuDNObfKhneD9AlnPAvyCRHKW3BidXKglD1j/LE9CM=" { };
          imgui = gh "ocornut" "imgui" "v1.93.0" "sha256-SSrZe05l78C2nea80JmK6Xc8VPYdnRkrOVXG1sYo2VE=" { };
          rapidcheck = gh "emil-e" "rapidcheck" "ff6af6fc683159deb51c543b065eba14dfcf329b"
            "sha256-Ixz5RpY0n8Un/Pv4XoTfbs40+70iyMbkQUjDqoLaWOg=" { };
          Yojimbo = gh "mas-bandwidth" "yojimbo" "272153a10f32135bb44bb60e7467072baf48f762"
            "sha256-C7KepsbRlBMIIV2w7O3wZpzjgtdzgIo/c+dYGpucOi0=" { fetchSubmodules = true; };
        };

      toolchain = pkgs: {
        python = pkgs.python3.withPackages (ps: [ ps.capstone ]); # pyproject pins capstone==5.0.9

        # The build helper apps run shader Python scripts via CMake. This shim
        # runs them with Nix Python and avoids installing the Python project.
        uv = pkgs.writeShellScriptBin "uv" ''
          [ "$1" = run ] || { echo "uv shim: only 'uv run' is supported" >&2; exit 1; }
          shift
          while [ $# -gt 0 ]; do
            case "$1" in
              --project) shift 2 ;;
              -*) shift ;;
              *) break ;;
            esac
          done
          exec "$@"
        '';

        libs = [ pkgs.sdl3 pkgs.zlib pkgs.gtest ];

        # The OCaml recompiler (recomp/), which CMake builds with dune at configure time.
        ocaml = [ pkgs.ocaml pkgs.dune_3 ] ++ (with pkgs.ocamlPackages; [
          findlib cmdliner alcotest otoml checkseum digestif qcheck qcheck-alcotest
        ]);
        # findlib's setup hook only runs in mkShell, so helpers export this themselves.
        ocamlpath =
          let
            roots = with pkgs.ocamlPackages; [ findlib cmdliner alcotest otoml checkseum digestif qcheck qcheck-alcotest ];
            closure = builtins.genericClosure {
              startSet = map (p: { key = p.outPath; pkg = p; }) roots;
              operator = item: map (p: { key = p.outPath; pkg = p; })
                (builtins.filter (p: p ? outPath) (item.pkg.propagatedBuildInputs or [ ]));
            };
          in pkgs.lib.makeSearchPath "lib/ocaml/${pkgs.ocaml.version}/site-lib" (map (i: i.pkg) closure);
        tools = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.glslang pkgs.spirv-cross pkgs.git pkgs.cargo pkgs.rustc ];
      };

      cmakeFlags = pkgs:
        let tc = toolchain pkgs; in
        [
          "-DCMAKE_PREFIX_PATH=${pkgs.lib.concatMapStringsSep ";" (p: "${pkgs.lib.getDev p}") tc.libs}"
        ] ++ pkgs.lib.mapAttrsToList (name: src: "-DCPM_${name}_SOURCE=${src}") (cpmSources pkgs);

      # Shell fragment: parses `[ROM_DIR] [-- args]` after $game is set and shifted, validates,
      # sets rom_dir and build_dir, and defines configure() running the desktop cmake configure.
      desktopConfigure = pkgs: ''
        rom_dir=roms/$game
        if [ $# -gt 0 ] && [ "$1" != -- ]; then rom_dir=$1; shift; fi
        [ "''${1:-}" = -- ] && shift

        [ -f CMakeLists.txt ] && [ -d games/"$game" ] || { echo "error: run from the repo root with a valid GAME ($game)" >&2; exit 1; }
        [ -d "$rom_dir" ] || { echo "error: ROM directory $rom_dir does not exist" >&2; exit 1; }
        rom_dir=$(realpath "$rom_dir")
        build_dir=''${BUILD_DIR:-build/$game}
        export OCAMLPATH=${(toolchain pkgs).ocamlpath}''${OCAMLPATH:+:$OCAMLPATH}

        configure() {
          cmake -S . -B "$build_dir" -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DF3_GAME="$game" \
            -DF3_ROM_DIR="$rom_dir" \
            ${pkgs.lib.escapeShellArgs (cmakeFlags pkgs)} \
            "$@"
        }
      '';

      compile = pkgs:
        let tc = toolchain pkgs; in
        pkgs.writeShellApplication {
          name = "f3-compile";
          runtimeInputs = [ tc.uv tc.python pkgs.stdenv.cc ] ++ tc.tools ++ tc.ocaml;
          text = ''
            usage() {
              echo "usage: f3-compile GAME [ROM_DIR] [-- extra cmake configure args]" >&2
              echo "  Run from the repository root. ROM_DIR defaults to roms/GAME." >&2
              echo "  Output: \$BUILD_DIR, default build/GAME/ (game executable inside)." >&2
              exit 2
            }
            [ $# -ge 1 ] || usage
            game=$1; shift
            ${desktopConfigure pkgs}
            configure "$@"
            cmake --build "$build_dir"
            echo "build ok: $build_dir"
          '';
        };

      compdb = pkgs:
        let tc = toolchain pkgs; in
        pkgs.writeShellApplication {
          name = "f3-compdb";
          runtimeInputs = [ tc.uv tc.python pkgs.stdenv.cc ] ++ tc.tools ++ tc.ocaml;
          text = ''
            # usage: f3-compdb [GAME=landmakrj] [ROM_DIR=roms/GAME] [-- extra cmake configure args]
            # Configures $BUILD_DIR (default build/GAME) when it has no compile_commands.json or
            # when configure args are given, then links it to ./compile_commands.json for clangd.
            game=landmakrj
            if [ $# -gt 0 ] && [ "$1" != -- ]; then game=$1; shift; fi
            ${desktopConfigure pkgs}
            if [ $# -gt 0 ] || [ ! -f "$build_dir/compile_commands.json" ]; then
              configure "$@"
            fi
            ln -sfn "$build_dir/compile_commands.json" compile_commands.json
            echo "compile_commands.json -> $build_dir/compile_commands.json"
          '';
        };

      desktop = system:
        let pkgs = nixpkgs.legacyPackages.${system}; in
        {
          packages = { default = compile pkgs; f3-compile = compile pkgs; f3-compdb = compdb pkgs; };

          apps = let app = { type = "app"; program = "${compile pkgs}/bin/f3-compile"; };
            in {
              default = app;
              compile = app;
              compdb = { type = "app"; program = "${compdb pkgs}/bin/f3-compdb"; };
            };

          devShells =
            let tc = toolchain pkgs; in
            {
              default = pkgs.mkShell {
                packages = [ pkgs.uv tc.python pkgs.clippy pkgs.rustfmt pkgs.clang-tools ] ++ tc.tools ++ tc.libs
                  # The OCaml recompiler (recomp/, built with dune).
                  ++ tc.ocaml;
                env.RUST_SRC_PATH = "${pkgs.rustPlatform.rustLibSrc}";
                shellHook = ''
                  export CMAKE_PREFIX_PATH=${pkgs.lib.makeSearchPath "" (map pkgs.lib.getDev tc.libs)}
                  ${pkgs.lib.concatStringsSep "\n" (pkgs.lib.mapAttrsToList (n: s: "export CPM_${n}_SOURCE=${s}") (cpmSources pkgs))}
                '';
              };
            };
        };

      merged = attr: lib.genAttrs systems (system: (desktop system).${attr});
    in
    {
      packages = merged "packages";
      apps = merged "apps";
      devShells = merged "devShells";
    };
}
