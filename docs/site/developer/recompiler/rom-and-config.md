# ROM loading and game config

The main compiler loads a mandatory `[rom]` table from the selected TOML, verifies its chip lanes and interleaves them into the configured program image. Images are not universally 2 MiB: 1 MiB images are supported, and the unpopulated part of the runtime's 2 MiB main map reads `0xff`.

## Selection and identity

`F3_GAME` selects `games/SET/config.toml`: `landmakrj` (default), `landmakr` (config-only, unverified), `rayforce`, `commandw` or `ridingf`. The two LM selections build `landmakr`; other title targets use the set name. Runtime metadata is generated from the manifests by `tools/compile_roms.py` using `recomp/roms.py`; Python 3.11 is required even without CPU generation. Capstone is required only for recompilation.

Chip size, CRC32 and SHA-1 protect against wrong revisions and corrupt files. Runtime main/sound image CRC guards also bind loaded images to generated programs. A matching directory name is not identity; missing files are never substituted from another set.

## Main compiler invocation

```sh
python3 -m recomp emit --config games/rayforce/config.toml \
  --rom-dir /path/to/roms/rayforce --output build/rayforce/generated/rayforce
```

Use `discover` instead of `emit` to write discovery coverage without C. `--config`, `--rom-dir` and `--output` are mandatory. `--max-block-instructions` defaults to 32. The command loads the image/config, discovers candidates, writes `coverage.json`, then emits C for `emit`; errors produce `f3-recomp:` and exit 1.

`[rom]` has positive `size`, `interleave` (default 4) and `[[rom.lanes]]`. Each lane supplies `file`, `offset`, `size`, `crc` and `sha1`. Main byte lane k supplies image bytes k, k+interleave, and so on. Every lane offset must be unique and all positions must be covered. Four 8-bit chips form a big-endian 32-bit main bus; lane 0 is its most significant byte. Wrong size/hash, duplicate/invalid offsets, missing files or incomplete image coverage reject loading.

Land Maker Japan's four 512 KiB main chips form a 2 MiB image; World has different program files/hashes and remains unverified. Historical Japan decisions and gameplay proof are not evidence for World or the newer titles.

## Additional manifest regions

Optional tables `sprites`, `sprites_hi`, `tiles`, `tiles_hi`, `sound`, `samples` describe runtime artwork and audio. Each region has explicit `size` and `fill` (0 or 255), with lanes containing `file`, `size`, `crc` (CRC32), `sha1`, `offset`, `stride`, `group`. Groups of `group` consecutive source bytes are placed every `stride` destination bytes, beginning at `offset`; placement must fit the region. These shared region lanes require sizes/hashes. Absent upper planes, as in Riding Fight, use empty zero-size regions, not fabricated files.

LM sound's accepted short dumps additionally specify `short_size`, `short_crc`, `short_sha1`. The short source is validated before FF padding, then the padded image is validated against the full lane identity. This is not permission to pad arbitrary corrupt chips.

Command War/Riding Fight specify physical `sound.size = 0x40000` and `mirror_size = 0x80000`: the physical bank repeats in the upper half of the mapped sound image. RayForce is physically and logically `0x80000`. The sound compiler consumes this same manifest; there is no `sound.bin` fallback.

## Discovery and exclusions

`[discovery]` controls recursive or `all_aligned` coverage, configured entries, jump tables and scanners. All-aligned candidates include data and overlapping starts; registration is not proof of reachability. `entry_points` lists even instruction addresses that become proven seeds. Code hooks (`[[hooks]]`) were removed from the recompiler and are no longer accepted.

`[[exclude]]` uses even half-open bounds, CPU (`main` by default or `sound`), reason and evidence. It removes instruction starts, not data bytes. Excluded execution fails even with diagnostic fallback; profiles must match the selected image and exclusions.

See [game config](/reference/game-config), [discovery](/developer/recompiler/discovery) and [porting](/developer/porting) for the complete contract and evidence limitations.

## CMake integration and tests

With `F3_ROM_DIR`, CMake generates main and sound programs from the selected config into `BUILD_DIR/generated/SET` and `BUILD_DIR/generated/sound-SET`. Configs, compiler sources and shared manifest tools are tracked inputs. See [build options](/reference/build-options).

ROM placement and corruption tests use synthetic fixtures; real ROMs are not distributed. The new placement/corruption tests and existing code-generation fixtures passed 14/14 during title bring-up. That does not establish full gameplay or audio parity.
