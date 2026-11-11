{
  description = "f3-recomp: static recompilation of Taito F3 arcade games";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    devkitNix = {
      url = "github:bandithedoge/devkitNix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, devkitNix }:
    let
      lib = nixpkgs.lib;
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      # devkitNix packages the devkitPro images for Linux hosts only.
      n3dsSystems = [ "x86_64-linux" ];

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
        tools = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.glslang pkgs.spirv-cross pkgs.git pkgs.cargo pkgs.rustc ];
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

      desktop = system:
        let pkgs = nixpkgs.legacyPackages.${system}; in
        {
          packages = { default = compile pkgs; f3-compile = compile pkgs; };

          apps = let app = { type = "app"; program = "${compile pkgs}/bin/f3-compile"; };
            in { default = app; compile = app; };

          devShells =
            let tc = toolchain pkgs; in
            {
              default = pkgs.mkShell {
                packages = [ tc.uv tc.python ] ++ tc.tools ++ tc.libs;
                shellHook = ''
                  export CMAKE_PREFIX_PATH=${pkgs.lib.makeSearchPath "" (map pkgs.lib.getDev tc.libs)}
                  ${pkgs.lib.concatStringsSep "\n" (pkgs.lib.mapAttrsToList (n: s: "export CPM_${n}_SOURCE=${s}") (cpmSources pkgs))}
                '';
              };
            };
        };


      devkitEnv = devkit: {
        DEVKITPRO = "${devkit}/opt/devkitpro";
        DEVKITARM = "${devkit}/opt/devkitpro/devkitARM";
      };

      # makerom/ctrtool live under Project_CTR; mirror nixpkgs' ctrtool derivation
      # (3DSGuy/Project_CTR#145 forces -O0 on the C/C++ compile flags).
      makerom = pkgs: pkgs.stdenv.mkDerivation (finalAttrs: {
        pname = "makerom";
        version = "0.19.0";
        src = pkgs.fetchFromGitHub {
          owner = "3DSGuy";
          repo = "Project_CTR";
          rev = "makerom-v${finalAttrs.version}";
          hash = "sha256-GvEzv97DqCsaDWVqDpajQRWYe+WM8xCYmGE0D3UcSrM=";
        };
        sourceRoot = "${finalAttrs.src.name}/makerom";
        enableParallelBuilding = true;
        preBuild = "make -j $NIX_BUILD_CORES deps";
        env.NIX_CFLAGS_COMPILE = "-O0";
        installPhase = ''
          mkdir -p $out/bin
          cp bin/makerom $out/bin/
        '';
        meta = {
          description = "CIA/CCI/CFA builder for the Nintendo 3DS";
          license = lib.licenses.mit;
          platforms = lib.platforms.linux;
          mainProgram = "makerom";
        };
      });

      bannertool = pkgs: pkgs.stdenv.mkDerivation (finalAttrs: {
        pname = "bannertool";
        version = "1.2.0";
        src = pkgs.fetchFromGitHub {
          owner = "diasurgical";
          repo = "bannertool";
          rev = finalAttrs.version;
          hash = "sha256-jdve9Ftki7H1C9k6gO8EQR5CtF3PUlRVqLOySegcejU=";
          fetchSubmodules = true;
        };
        postPatch = ''
          sed -i -e 's/^VERSION_PARTS.*$/VERSION_PARTS := ${lib.concatStringsSep " " (lib.splitVersion finalAttrs.version)}/' Makefile
        '';
        enableParallelBuilding = true;
        buildPhase = ''
          runHook preBuild
          make -j$NIX_BUILD_CORES output/linux-x86_64/bannertool
          runHook postBuild
        '';
        installPhase = ''
          runHook preInstall
          install -Dm755 output/linux-x86_64/bannertool $out/bin/bannertool
          runHook postInstall
        '';
        meta = {
          description = "Banner and SMDH builder for the Nintendo 3DS";
          license = lib.licenses.mit;
          platforms = [ "x86_64-linux" ];
          mainProgram = "bannertool";
        };
      });

      m68kmake = pkgs: pkgs.runCommandCC "f3-m68kmake-1" {
        meta = {
          description = "Musashi 68k opcode table generator built for the host";
          mainProgram = "m68kmake";
        };
      } ''
        mkdir -p $out/bin
        $CC -O2 -o $out/bin/m68kmake ${./runtime/third_party/musashi/m68kmake.c}
      '';

      n3dsConfigureFlags = pkgs: makeromDrv: bannertoolDrv: m68kmakeDrv:
        let
          tc = toolchain pkgs;
          sources = lib.getAttrs [ "imgui" "yyjson" "cpp_yyjson" ] (cpmSources pkgs);
        in
        [
          "-DCMAKE_BUILD_TYPE=Release"
          "-DF3RT_SDL=OFF"
          "-DF3RT_GPU=OFF"
          "-DF3RT_NETPLAY=OFF"
          "-DF3RT_HOST_TOOLS=OFF"
          "-DBUILD_TESTING=OFF"
          "-DUV_EXECUTABLE=${tc.uv}/bin/uv"
          "-DF3_M68KMAKE_EXECUTABLE=${m68kmakeDrv}/bin/m68kmake"
          "-DF3_MAKEROM_EXECUTABLE=${makeromDrv}/bin/makerom"
          "-DF3_BANNERTOOL_EXECUTABLE=${bannertoolDrv}/bin/bannertool"
        ] ++ lib.mapAttrsToList (name: src: "-DCPM_${name}_SOURCE=${src}") sources;

      n3ds = system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          tc = toolchain pkgs;
          devkit = devkitNix.packages.${system}.devkitARM;
          env = devkitEnv devkit;
          makeromDrv = makerom pkgs;
          bannertoolDrv = bannertool pkgs;
          m68kmakeDrv = m68kmake pkgs;
          configureFlags = n3dsConfigureFlags pkgs makeromDrv bannertoolDrv m68kmakeDrv;
          flagArray = lib.concatMapStrings (f: "  ${lib.escapeShellArg f}\n") configureFlags;

          f3-3ds = pkgs.writeShellApplication {
            name = "f3-3ds";
            runtimeInputs = [
              tc.uv tc.python pkgs.cmake pkgs.ninja
              devkit makeromDrv bannertoolDrv m68kmakeDrv
            ];
            text = ''
              usage() {
                printf '%s\n' \
                  'usage: f3-3ds {3dsx|cia|bench} [GAME=landmakrj] [ROM_DIR=roms/GAME] [-- cmake configure args]' \
                  '  Run from the repository root. GAME defaults to landmakrj, ROM_DIR to roms/GAME.' \
                  '  Output: build/GAME-3ds/GAME.3dsx, or GAME.cia for the cia target.' \
                  '  bench: dev-only 3dsx with the scripted bench input (F3_N3DS_BENCH), in' \
                  '  build/GAME-3ds-bench/GAME.3dsx so the shipped build directory stays untouched.' >&2
                exit 2
              }

              [ "$#" -ge 1 ] || usage
              target=$1
              shift
              case $target in
                3dsx|cia|bench) ;;
                *)
                  printf 'error: unknown target: %s\n' "$target" >&2
                  usage
                  ;;
              esac

              game=landmakrj
              rom_dir=
              if [ "$#" -gt 0 ] && [ "$1" != "--" ]; then game=$1; shift; fi
              if [ "$#" -gt 0 ] && [ "$1" != "--" ]; then rom_dir=$1; shift; fi
              if [ "''${1:-}" = "--" ]; then shift; fi
              if [ -z "$rom_dir" ]; then rom_dir=roms/$game; fi

              if [ ! -f CMakeLists.txt ] || [ ! -d "games/$game" ]; then
                printf 'error: run from the repo root with a valid GAME (%s)\n' "$game" >&2
                exit 1
              fi
              if [ ! -d "$rom_dir" ]; then
                printf 'error: ROM directory %s does not exist\n' "$rom_dir" >&2
                exit 1
              fi
              rom_dir=$(realpath "$rom_dir")

              export DEVKITPRO=${env.DEVKITPRO}
              export DEVKITARM=${env.DEVKITARM}
              export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"

              if [ ! -f "$DEVKITPRO/cmake/3DS.cmake" ]; then
                printf 'error: %s/cmake/3DS.cmake is missing from the devkitARM package\n' "$DEVKITPRO" >&2
                exit 1
              fi

              build_dir="build/$game-3ds"
              bench_flags=()
              if [ "$target" = bench ]; then
                build_dir="build/$game-3ds-bench"
                bench_flags=(-DF3_N3DS_BENCH=ON)
              fi

              flags=(
              ${flagArray})

              cmake -S . -B "$build_dir" -G Ninja \
                -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
                -DF3_GAME="$game" \
                -DF3_ROM_DIR="$rom_dir" \
                "''${flags[@]}" \
                "''${bench_flags[@]}" \
                "$@"

              case $target in
                3dsx|bench) cmake --build "$build_dir" ;;
                cia) cmake --build "$build_dir" --target "$game-cia" ;;
              esac
              printf 'build ok: %s/%s.%s\n' "$build_dir" "$game" "''${target/bench/3dsx}"
            '';
          };

          app = drv: name: { type = "app"; program = "${drv}/bin/${name}"; };

          f3-3dsx = pkgs.writeShellApplication {
            name = "f3-3dsx";
            runtimeInputs = [ f3-3ds ];
            text = ''
              exec f3-3ds 3dsx "$@"
            '';
          };

          f3-3ds-bench = pkgs.writeShellApplication {
            name = "f3-3ds-bench";
            runtimeInputs = [ f3-3ds ];
            text = ''
              exec f3-3ds bench "$@"
            '';
          };

          f3-3ds-cia = pkgs.writeShellApplication {
            name = "f3-3ds-cia";
            runtimeInputs = [ f3-3ds ];
            text = ''
              exec f3-3ds cia "$@"
            '';
          };

          f3-3ds-azahar = pkgs.writeShellApplication {
            name = "f3-3ds-azahar";
            runtimeInputs = [ f3-3ds pkgs.azahar ];
            text = ''
              game=landmakrj
              if [ "$#" -gt 0 ] && [ "$1" != "--" ]; then
                game=$1
                shift
              fi
              f3-3ds 3dsx "$game" "$@"
              exec ${pkgs.azahar}/bin/azahar "build/$game-3ds/$game.3dsx"
            '';
          };
        in
        {
          packages = {
            devkitARM = devkit;
            f3-3ds = f3-3ds;
            makerom = makeromDrv;
            bannertool = bannertoolDrv;
            f3-m68kmake = m68kmakeDrv;
          };

          apps = {
            "3ds-3dsx" = app f3-3dsx "f3-3dsx";
            "3ds-cia" = app f3-3ds-cia "f3-3ds-cia";
            "3ds-bench" = app f3-3ds-bench "f3-3ds-bench";
            "3ds-azahar" = app f3-3ds-azahar "f3-3ds-azahar";
          };

          devShells = {
            "3ds" = pkgs.mkShell {
              packages = [
                devkit makeromDrv bannertoolDrv m68kmakeDrv
                pkgs.azahar pkgs.ctrtool
                pkgs.linuxPackages.perf # host-side profile of the emulator process
                tc.uv tc.python
                pkgs.cmake pkgs.ninja pkgs.git
              ];
              shellHook = ''
                export DEVKITPRO=${env.DEVKITPRO}
                export DEVKITARM=${env.DEVKITARM}
                export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
                export CPATH="${devkit.CPATH}"
                ${lib.concatStringsSep "\n" (lib.mapAttrsToList (n: s: "export CPM_${n}_SOURCE=${s}") (cpmSources pkgs))}
              '';
            };
          };
        };

      n3dsOutputs = lib.genAttrs n3dsSystems n3ds;

      merged = attr: lib.genAttrs systems (system:
        let base = (desktop system).${attr}; in
        base // lib.optionalAttrs (lib.hasAttr system n3dsOutputs) n3dsOutputs.${system}.${attr});
    in
    {
      packages = merged "packages";
      apps = merged "apps";
      devShells = merged "devShells";
    };
}
