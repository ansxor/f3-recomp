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
          nlohmann_json = gh "nlohmann" "json" "v3.12.0" "sha256-cECvDOLxgX7Q9R3IE86Hj9JJUxraDQvhoyPDF03B2CY=" { };
          mcp_cpp = gh "Neumann-Labs" "mcp-cpp" "57be64093a2241b58d01292d096b266fc6a52ec8"
            "sha256-1iBxSFWQ0xvXekztS0omgd1bljv524gHAcva25NDIyU=" { };
          Yojimbo = gh "mas-bandwidth" "yojimbo" "272153a10f32135bb44bb60e7467072baf48f762"
            "sha256-C7KepsbRlBMIIV2w7O3wZpzjgtdzgIo/c+dYGpucOi0=" { fetchSubmodules = true; };
        };

      # tmattio/ocaml-mcp (MCP client/server SDK) and its jsonschema dependency are not in nixpkgs.
      ocamlMcp = pkgs:
        let
          o = pkgs.ocamlPackages;
          jsonschema = o.buildDunePackage {
            pname = "jsonschema";
            version = "0.1.0+git7cc8b36";
            src = pkgs.fetchFromGitHub {
              owner = "tmattio"; repo = "ocaml-jsonschema";
              rev = "7cc8b365596295d9ac9eb68d78bb274bf3074c7c";
              hash = "sha256-yeOAxN+dC4jFNyFPut/xjfLM8d4Eh3CknrbkpJUnWL4=";
            };
            propagatedBuildInputs = with o; [ re yojson ppx_deriving_yojson base64 uri ];
            doCheck = false;
          };
          src = pkgs.fetchFromGitHub {
            owner = "tmattio"; repo = "ocaml-mcp";
            rev = "49ee348859d8167efb23dfad714250e92177d564";
            hash = "sha256-tStg0hme5yLcg1IPVBvNZ+qUI86REg3OvqEdy2GZlRQ=";
          };
          # jsonrpc >= 1.25 and yojson 3 dropped the `Tuple/`Variant JSON cases the upstream converters match on;
          # optional capability and request fields get [@default None] (mcp-cpp omits listChanged, isError and
          # arguments); and the 1 MB stdio line limit is lifted (screenshots and sprite RAM are one JSON line).
          patches = [ ./nix/ocaml-mcp-compat.patch ];
          # Libraries: mcp, mcp.sdk (package mcp) and mcp-eio, mcp-eio.sdk (package mcp-eio).
          mcp = o.buildDunePackage {
            pname = "mcp";
            version = "0.1.0+git49ee348";
            inherit src patches;
            propagatedBuildInputs = [ jsonschema ] ++ (with o; [ logs jsonrpc yojson ppx_deriving_yojson re base64 ]);
            doCheck = false;
          };
          mcp-eio = o.buildDunePackage {
            pname = "mcp-eio";
            version = "0.1.0+git49ee348";
            inherit src patches;
            propagatedBuildInputs = [ mcp jsonschema ]
              ++ (with o; [ eio eio_main logs jsonrpc yojson ppx_deriving_yojson http cohttp cohttp-eio uri cstruct ]);
            doCheck = false;
          };
        in [ jsonschema mcp mcp-eio ];

      toolchain = pkgs: rec {
        python = pkgs.python3.withPackages (ps: [ ps.capstone ps.pytest ]); # pyproject pins capstone==5.0.9

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

        libs = [ pkgs.sdl3 pkgs.zlib pkgs.gtest pkgs.nlohmann_json ];

        # The OCaml recompiler (recomp/), which CMake builds with dune at configure time.
        ocamlLibs = (with pkgs.ocamlPackages; [
          findlib cmdliner alcotest otoml checkseum digestif qcheck qcheck-alcotest eio_main yojson
        ]) ++ ocamlMcp pkgs;
        ocaml = [ pkgs.ocaml pkgs.dune_3 ] ++ ocamlLibs;
        # findlib's setup hook only runs in mkShell, so helpers export this themselves.
        ocamlpath =
          let
            roots = ocamlLibs;
            closure = builtins.genericClosure {
              startSet = map (p: { key = p.outPath; pkg = p; }) roots;
              operator = item: map (p: { key = p.outPath; pkg = p; })
                (builtins.filter (p: p ? outPath) (item.pkg.propagatedBuildInputs or [ ]));
            };
          in pkgs.lib.makeSearchPath "lib/ocaml/${pkgs.ocaml.version}/site-lib" (map (i: i.pkg) closure);
        tools = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.glslang pkgs.spirv-cross pkgs.git pkgs.cargo pkgs.rustc pkgs.cxxbridge ];
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

      # The OCaml recompiler as a standalone executable, for the MCP server (CMake builds its own copy with dune).
      f3recomp = pkgs:
        pkgs.ocamlPackages.buildDunePackage {
          pname = "f3recomp";
          version = "0.1.0";
          src = lib.cleanSourceWith {
            src = ./recomp;
            filter = path: _type: builtins.baseNameOf path != "_build";
          };
          buildInputs = with pkgs.ocamlPackages; [ cmdliner otoml checkseum digestif eio_main yojson ] ++ ocamlMcp pkgs;
          doCheck = false;
        };

      # `f3recomp mcp`: static analysis plus live f3rt-tool sessions over MCP (stdio). Run from the repository
      # root (or pass --root DIR): it resolves games/, roms/, profiles/ and build/<game>/f3rt-tool there.
      mcpServer = pkgs:
        pkgs.writeShellApplication {
          name = "f3-mcp";
          runtimeInputs = [ (f3recomp pkgs) ];
          text = ''
            exec f3recomp mcp "$@"
          '';
        };

      desktop = system:
        let pkgs = nixpkgs.legacyPackages.${system}; in
        {
          packages = {
            default = compile pkgs;
            f3-compile = compile pkgs;
            f3-compdb = compdb pkgs;
            f3recomp = f3recomp pkgs;
            f3-mcp = mcpServer pkgs;
          };

          apps = let app = { type = "app"; program = "${compile pkgs}/bin/f3-compile"; };
            in {
              default = app;
              compile = app;
              compdb = { type = "app"; program = "${compdb pkgs}/bin/f3-compdb"; };
              f3-mcp = { type = "app"; program = "${mcpServer pkgs}/bin/f3-mcp"; };
              mcp = { type = "app"; program = "${mcpServer pkgs}/bin/f3-mcp"; };
            };

          devShells =
            let tc = toolchain pkgs; in
            {
              default = pkgs.mkShell {
                packages = [ tc.uv tc.python pkgs.clippy pkgs.rustfmt pkgs.clang-tools ] ++ tc.tools ++ tc.libs
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
