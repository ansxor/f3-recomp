# Porting another Taito F3 game

The compiler, memory/device runtime and MAME-derived FDP renderer are reusable, but a manifest alone is not proof of compatibility. Supported revisions and finite evidence are:

| Set | Revision | Integration and evidence |
| --- | --- | --- |
| `landmakrj` | Land Maker Japan 2.01J | Native main/sound, Japan-specific game-data producers, enhanced presentation and netplay; sampled gameplay/reference evidence, not exhaustive campaigns. |
| `landmakr` | Land Maker World | Existing config only; native build/play unverified. No Japan producer or netplay compatibility claim. |
| `rayforce` | America Ver 2.3A (1994/01/20) | Strict-native executable ran 3600 frames with zero main fallback. FDP and accurate sound only. |
| `commandw` | Command War 0.0J prototype | Strict-native executable ran 1800 frames with zero main fallback. FDP and accurate sound only. Upstream marks imperfect graphics: a source limitation, not a physical-board accuracy claim. |
| `ridingf` | Riding Fight World 1.0O | Strict-native executable ran 1800 frames with zero main fallback. FDP and accurate sound only; absent upper graphics planes and sprite trails are part of its board behavior. |

Command War and Riding Fight native/oracle audio parity is under investigation. These runs do not establish full campaigns, physical-board accuracy or arbitrary F3 compatibility. Runtime CTest passed 4/4; ROM placement/corruption tests and existing code-generation fixtures passed 14/14. RayForce's actual Wayland window reported internal 224×320 and output 672×960; a rotated attract capture was recorded at `build/rayforce-surface.bmp` (local evidence, not a distributed asset).

## Build and ROM identity

`F3_GAME` selects `games/SET/config.toml`, main/sound generation, default runtime set and executable identity. Its default is `landmakrj`; accepted values are the five sets above. Both LM selections name the executable `landmakr`; the other executables use their set names. Use a separate build directory per game:

```sh
python3 -m pip install --target build/rayforce/python -r recomp/requirements.txt
cmake -S . -B build/rayforce -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DF3_GAME=rayforce -DF3_ROM_DIR=/path/to/roms/rayforce -DF3RT_GPU=OFF
cmake --build build/rayforce --target rayforce -j 4
./build/rayforce/rayforce --headless --frames 3600
./build/rayforce/rayforce --frames 3600 --surface build/rayforce-surface.bmp
```

Substitute `commandw` or `ridingf` throughout to build those titles. Title executables default to strict native main execution and generated native sound with accurate devices. Python 3.11 is always required for manifest metadata; Capstone is needed only for CPU recompilation. Generated directories default to `BUILD_DIR/generated/SET` and `BUILD_DIR/generated/sound-SET`.

[`recomp/roms.py`](https://github.com/ansxor/f3-recomp/blob/main/recomp/roms.py) and [`tools/compile_roms.py`](https://github.com/ansxor/f3-recomp/blob/main/tools/compile_roms.py) share region geometry/validation between runtime metadata and sound compilation. Main and sound generated images bind to their loaded CRC identities; a mismatched set is rejected, not silently executed. Main images may be 1 MiB; unpopulated bytes in the 2 MiB main map return `0xff`. See [game config](/reference/game-config) for placement, hashes, padding and sound mirroring.

The new manifests are pinned to [MAME cfc4760a](https://github.com/mamedev/mame/blob/cfc4760a3be9c5a79846b19b6a573cb38459fa7e/src/mame/taito/taito_f3.cpp). Do not copy Japan's hooks, exclusions or `profiles/landmakrj.profile` to another image. Full-coverage tiers preserve cold entries; experimental slim profiles remove them and abort on misses. Exclusions remove instruction starts, not data bytes, and fail even with diagnostic fallback enabled.

## Video boundary

| Set | Board crop | Rotation/display | Sprite lag |
| --- | --- | --- | --- |
| `rayforce` | 320×224, visible y 31 | ROT90, 224×320 | 2 |
| `commandw` | 320×224, visible y 32 | Unrotated | 1 |
| `ridingf` | 320×224, visible y 32 | Unrotated | 1 |

Video metadata uses `rotation`, `sprite_lag`, `visible_y`, `visible_height` and extended sprite addressing (`extend = true`). Palette precision is not a per-game constant. FDA selects 15/24-bit color from latched line RAM `$6400` bit 14, active low. The 15-bit word is `RRRRGGGGBBBBRGBx`: fifth bits are 3/2/1, and each channel is `nibble * 16 + fifthbit * 8`, not full-range 5-bit expansion. Bit 13 controls blur, also active low. Dynamic pen masks belong to tile attributes and sprite commands, not hardcoded title masks. Sources: [fdp-collapse 28e411d4, tc0630fdp.cpp](https://github.com/y-ack/mame/blob/28e411d4f760df3d55fae070a2f6424f89966a2f/src/mame/taito/tc0630fdp.cpp) and [taito_f3_v.cpp](https://github.com/y-ack/mame/blob/28e411d4f760df3d55fae070a2f6424f89966a2f/src/mame/taito/taito_f3_v.cpp).

The conservative FDP path consumes ordinary device-memory writes and needs no per-game decoder. A game opts into the VRAM scene decoder by providing `games/<game>/video/`; CMake compiles it and defines `F3RT_GAME_VIDEO`. Japan's `games/landmakrj/video/video.cpp` decodes every component and holds the Japan-specific store-PC ranges; the generic decode lives in `runtime/video_decode.cpp`. Moving or renaming the per-game file does not make it generic. Its unsupported-feature fallback to FDP (`flipped-screen`, `sprite-trails`, `bitmap-pivot`) is separate from CPU interpreter fallback. Game-data video, HLE audio, GPU enhanced/motion semantics and versus netplay remain Japan-only.

## Bring-up requirements for another title

1. Identify exact revision and legal chip sources; record sizes, CRC32/SHA-1, grouping, fill and bank placement in its manifest. Absent planes need empty regions, not fake chip files.
2. Select config, generated image and executable together; retain CRC guards. Establish full instruction coverage and exercise strict native runs with zero main fallback.
3. Check memory map, inputs, EEPROM, IRQ/frame timing, renderer geometry, latch ordering, trails and palette effects against the pinned reference. MAME-derived output is not physical-chip verification.
4. Bring sound up independently with accurate devices and native/oracle instruction, bus, reset/IRQ and WAV comparisons. LM semantic trace probes are not generic driver metadata. Successful boot is not audio parity.
5. Exercise attract, service, coin/start, representative gameplay, transitions and snapshots under the exact revision; report durations and limitations rather than universal support.
6. Only add game-data/enhanced presentation or netplay after implementing title-specific producers, input/versus predicates and deterministic handoff/rollback evidence. Shared transport is not a generic gameplay adapter.

Existing gameplay/netplay/GPU regression tools retain LM-specific schedules and proof contracts. Inspect them before reuse; the new titles' frontend finite runs are not those campaigns. See [validation evidence](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/VALIDATION.md), [native sound](/developer/runtime/audio/native-driver) and [build options](/reference/build-options).

## Credits and licensing

Preserve [runtime/LICENSES.txt](https://github.com/ansxor/f3-recomp/blob/main/runtime/LICENSES.txt): individually BSD-3-Clause MAME adaptations are not a license to copy the MAME framework/GPL-only sources. Musashi and its SoftFloat dependency have separate notices; SDL3 uses zlib terms. No repository-wide license grant is supplied. ROMs, decoded artwork and captures remain separately protected; do not publish ROMs or generated game artifacts.
