{
  description = "f3-recomp: static recompilation of Taito F3 arcade games";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # Sources CPM.cmake would otherwise fetch at configure time (cmake/dependencies.cmake).
      # Pins mirror the GIT_TAGs there; each is handed to CMake as -DCPM_<name>_SOURCE so the
      # build needs no network.
      cpmSources = pkgs:
        let gh = owner: repo: rev: hash: extra:
          pkgs.fetchFromGitHub ({ inherit owner repo rev hash; } // extra);
        in {
          yyjson = gh "ibireme" "yyjson" "0.12.0" "sha256-1CYnEgUMUc7eqdkv6M/KyL/MdVQBMov9HgLCycF6++w=" { };
          cpp_yyjson = gh "yosh-matsuda" "cpp-yyjson" "v1.1.0" "sha256-VCCat/gLlRwEWCJ0AP8FH8PI1MqgtU7s7ORy3nHZFSc=" { };
          httplib = gh "yhirose" "cpp-httplib" "v0.51.0" "sha256-7vifeaLAlNioumol+fUkIUO93MFxVNOY6AGZkUJVYy8=" { };
          imgui = gh "ocornut" "imgui" "v1.92.8" "sha256-NaSgDE5QEEMrsgYOCAPx5d0XvCIQ9T+ciAKfLFhlmzw=" { };
          rapidcheck = gh "emil-e" "rapidcheck" "ff6af6fc683159deb51c543b065eba14dfcf329b"
            "sha256-23BY/V+qhenfllrH8jiNj4AsKGtEqLC18ev7yCAf9mA=" { };
          Yojimbo = gh "mas-bandwidth" "yojimbo" "272153a10f32135bb44bb60e7467072baf48f762"
            "sha256-C7KepsbRlBMIIV2w7O3wZpzjgtdzgIo/c+dYGpucOi0=" { fetchSubmodules = true; };
        };

      toolchain = pkgs: {
        python = pkgs.python3.withPackages (ps: [ ps.capstone ]); # pyproject pins capstone==5.0.9

        # CMake runs `uv run --project . --locked --no-default-groups python ...`. uv would
        # fetch capstone from PyPI; the sandbox-free Nix environment already has it, so this
        # stand-in drops uv's options and runs the command with the Nix Python.
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
        tools = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.glslang pkgs.spirv-cross pkgs.git ];
      };

      cmakeFlags = pkgs:
        let tc = toolchain pkgs; in
        [
          "-DCMAKE_PREFIX_PATH=${pkgs.lib.concatMapStringsSep ";" (p: "${pkgs.lib.getDev p}") tc.libs}"
        ] ++ pkgs.lib.mapAttrsToList (name: src: "-DCPM_${name}_SOURCE=${src}") (cpmSources pkgs);

      compile = pkgs:
        let tc = toolchain pkgs; in
        pkgs.writeShellApplication {
          name = "f3-compile";
          runtimeInputs = [ tc.uv tc.python pkgs.stdenv.cc ] ++ tc.tools;
          text = ''
            usage() {
              echo "usage: f3-compile GAME [ROM_DIR] [-- extra cmake configure args]" >&2
              echo "  Run from the repository root. ROM_DIR defaults to roms/GAME." >&2
              echo "  Output: \$BUILD_DIR, default build/GAME/ (game executable inside)." >&2
              exit 2
            }
            [ $# -ge 1 ] || usage
            game=$1; shift
            rom_dir=roms/$game
            if [ $# -gt 0 ] && [ "$1" != -- ]; then rom_dir=$1; shift; fi
            [ "''${1:-}" = -- ] && shift

            [ -f CMakeLists.txt ] && [ -d games/"$game" ] || { echo "error: run from the repo root with a valid GAME ($game)" >&2; exit 1; }
            [ -d "$rom_dir" ] || { echo "error: ROM directory $rom_dir does not exist" >&2; exit 1; }
            rom_dir=$(realpath "$rom_dir")
            build_dir=''${BUILD_DIR:-build/$game}

            cmake -S . -B "$build_dir" -G Ninja \
              -DCMAKE_BUILD_TYPE=Release \
              -DF3_GAME="$game" \
              -DF3_ROM_DIR="$rom_dir" \
              ${pkgs.lib.escapeShellArgs (cmakeFlags pkgs)} \
              "$@"
            cmake --build "$build_dir"
            echo "build ok: $build_dir"
          '';
        };
    in
    {
      packages = forAllSystems (pkgs: { default = compile pkgs; f3-compile = compile pkgs; });

      # nix run .#compile -- landmakrj roms/landmakrj
      apps = forAllSystems (pkgs:
        let app = { type = "app"; program = "${compile pkgs}/bin/f3-compile"; };
        in { default = app; compile = app; });

      devShells = forAllSystems (pkgs:
        let tc = toolchain pkgs; in
        {
          default = pkgs.mkShell {
            packages = [ tc.uv tc.python ] ++ tc.tools ++ tc.libs;
            # `make build GAME=<id>` works unchanged: CMake and CPM read these from the environment.
            shellHook = ''
              export CMAKE_PREFIX_PATH=${pkgs.lib.makeSearchPath "" (map pkgs.lib.getDev tc.libs)}
              ${pkgs.lib.concatStringsSep "\n" (pkgs.lib.mapAttrsToList (n: s: "export CPM_${n}_SOURCE=${s}") (cpmSources pkgs))}
            '';
          };
        });
    };
}
