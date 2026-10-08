# Video system overview

This section explains both video renderers, their source data, and their native output contract. It also covers diagnostics and optional presentation.

## Two renderers

The runtime has two independent renderers for the Taito F3 picture. Both produce native 320x232 output at vertical blank. `GameVideo` can also produce expanded presentation.

| Renderer | Class | Input | Role |
| --- | --- | --- | --- |
| FDP renderer | `f3rt::Video` | The emulated video RAM that the game wrote: sprite RAM, playfield RAM, text RAM, character RAM, line RAM, pivot RAM, palette RAM and control registers | The internal **oracle**: a MAME-derived TC0630FDP model used as the game-data renderer's reference, not physical-chip verification. |
| Game-data renderer | `f3rt::GameVideo` | The same FDP video RAM (`graphics` at 0x600000, `control` at 0x660000), decoded into scene structures at VBSTART. | The **default renderer** of a game that provides `games/<game>/video/`. It rebuilds the scene and draws it. It can also draw at a higher resolution with extra border columns. |

The abbreviation **FDP** means the TC0630FDP video chip of the Taito F3 board. The oracle name comes from its job: `GameVideo` must give the same pixels as `Video` for every frame that `GameVideo` supports.

Internal pixel parity and matching sampled MAME output are separate evidence.
Neither proves unexercised chip behavior or support for every F3 game.

::: info
Why two renderers? The game-data renderer decodes the same video RAM into scene structures, so it can rerasterize supported scenes at higher resolutions and add border columns. The FDP renderer remains the internal reference for comparison and draws fallback frames.
:::

## Where the renderers run

`Machine::advance_to` renders one frame each time the hardware clock reaches `next_vblank`. The render runs before the runtime raises IRQ2. If `Machine::game_video` is not null, the runtime calls `GameVideo::render_frame`. If it is null, the runtime calls `Video::render_frame` directly.

```mermaid
flowchart TD
    A["Machine::advance_to"] --> B{"hardware_cycles equals next_vblank?"}
    B -- "no" --> Z["continue CPU and audio"]
    B -- "yes" --> C{"game_video set?"}
    C -- "no" --> D["Video::render_frame"]
    C -- "yes" --> E["GameVideo::render_frame"]
    E --> D
    D --> F["Machine::native_pixels(), 320x232"]
    E --> G["GameVideo presentation buffer"]
    F --> H["raise IRQ2, increment frame"]
    G --> H
```

In game mode (`enhanced` / `game-cpu`), `GameVideo::render_frame` calls `Video::render_frame` only when the scene is not supported. Read [GameVideo and frame selection](/developer/runtime/video/game-hle) for the exact rules.

## Mode selection

The frontend selects the renderer with `--renderer` (or the `renderer=` setting). `accurate` and `enhanced` are user-facing; `game-cpu`, `compare-cpu` and `compare-gpu` are developer-only command-line values. Each name maps to a `GameVideoMode` and a presentation backend (CPU or GPU). The backend never changes `Machine::native_pixels()`. The table shows the modes and one internal mode.

| Renderer | `GameVideoMode` (backend) | Who sets it | Output in `Machine::native_pixels()` |
| --- | --- | --- | --- |
| `accurate` | none (`game_video` is null; CPU) | `--renderer accurate` | Oracle picture |
| `enhanced`, `game-cpu` | `Game` (GPU, CPU) | `--renderer enhanced`, `--renderer game-cpu` | Game-data picture for supported frames. Oracle picture for other frames. |
| `compare-cpu`, `compare-gpu` | `Compare` (CPU, GPU) | `--renderer compare-cpu`, `--renderer compare-gpu` | Always the oracle picture. The runtime also checks that the game-data picture is equal on each supported frame. |
| none | `Diagnostic` | The gameplay regression tool | Always the oracle picture. The tool calls `compare_layers` at chosen frames. |

Game-data video is compiled only for games with a `games/<game>/video/` folder (`F3RT_GAME_VIDEO`; today `landmakrj`). The strict-native `landmakr` program then uses `enhanced` by default (`accurate` when the build has no `F3RT_GPU`); with `--allow-fallback` it uses `accurate`, because game-data video requires strict native execution. A build without the folder supports only `accurate` and rejects every other renderer. Read [Presentation](/developer/runtime/video/presentation) for scale, border and filter options and [Compare mode](/developer/runtime/video/compare-mode) for the checking tools. The user view of the same options is in the [video guide](/guide/video).

## Source file map

The table lists every file of the video system and the page that explains it.

| File | Contents | Page |
| --- | --- | --- |
| `include/f3rt/video.hpp`, `runtime/renderer/fdp/video.cpp` | `Video`: the FDP renderer and its inspection functions | [FDP renderer](/developer/runtime/video/fdp) |
| `include/f3rt/game_video.hpp`, `runtime/renderer/game/video.cpp` | `GameVideo`, `GameVideoOptions`, `GameVideoMode`; the VBSTART decode and frame selection | [GameVideo](/developer/runtime/video/game-hle) |
| `runtime/renderer/game/scene.hpp` | `VideoRam`, `ScenePixel`, `SceneSprite`, `SceneLayer`, `ScenePlayfield`, `SceneClip`, `SceneRow` | [Scene types](/developer/runtime/video/scene) |
| `runtime/renderer/decode.hpp`, `runtime/renderer/decode.cpp` | Generic char-RAM tile unpack and sprite display-list walk | [Sprites](/developer/runtime/video/sprites) |
| `games/landmakrj/video/video.cpp` | Per-game `video_writer_known` store-PC lists for `--discovery-log` | [Video write logging](/developer/runtime/video/producers) |
| `runtime/renderer/game/tiles.hpp`, `runtime/renderer/game/tiles.cpp` | `GameTiles`: four raw playfield cell maps | [Playfield tiles](/developer/runtime/video/tiles) |
| `runtime/renderer/game/text.hpp`, `runtime/renderer/game/text.cpp` | `GameText`: text map and glyphs | [Text layer](/developer/runtime/video/text) |
| `runtime/renderer/game/sprites.hpp`, `runtime/renderer/game/sprites.cpp` | `GameSprites`: sprite list, latch and raster | [Sprites](/developer/runtime/video/sprites) |
| `runtime/renderer/game/lines.hpp`, `runtime/renderer/game/lines.cpp` | `GameLines`: per-line effects and `SceneRow` generation | [Line effects](/developer/runtime/video/lines) |
| `runtime/renderer/game/compositor.hpp`, `runtime/renderer/game/compositor.cpp` | `compose_game_scene` | [Compositor](/developer/runtime/video/compositor) |
| `runtime/renderer/game/video_log.hpp`, `runtime/renderer/game/video_log.cpp` | `log_unsupported_video` (fallback kinds); the opt-in store-PC log is `runtime/discovery_log.cpp` | [Video write logging](/developer/runtime/video/producers) |
| `runtime/state_io.hpp` | `Canonical*` structs that save the video state | [GameVideo](/developer/runtime/video/game-hle) |
| `runtime/check.cpp` | Unit checks for tile, sprite and edge rules | [Extending the renderer](/developer/runtime/video/extending) |
| `tools/gameplay_regression.cpp` | The `--video-diff` harness | [Compare mode](/developer/runtime/video/compare-mode) |

## Key numbers

These constants appear in many places. All come from `include/f3rt/video.hpp` and `runtime/renderer/fdp/video.cpp`.

| Name | Value | Meaning |
| --- | --- | --- |
| `SCREEN_WIDTH`, `SCREEN_HEIGHT` | 320, 232 | Size of the visible picture |
| `H_TOTAL` | 432 | Width of the scanout space and of the sprite plane |
| `H_START` | 46 | First visible column in scanout space |
| `V_START`, `V_VIS` | 24, 232 | First visible line and number of visible lines |
| Scanout lines | 256 | Lines 0 to 255. The renderer draws lines 24 to 255. |
| `Machine::frame_pixels` | 432 x 262 | Pixel clocks per frame. The pixel clock is 6,671,500 Hz. |

The visible window is columns 46 to 365 and lines 24 to 255 of the scanout space. Most code in this system converts between scanout coordinates and picture coordinates by subtracting 46 and 24.

## Reading order

Read the pages in this order if you are new to the code.

1. [F3 video hardware](/developer/runtime/video/hardware). It explains the modeled memory layout and effects, with the limits of physical-hardware evidence.
2. [FDP renderer](/developer/runtime/video/fdp), then [FDP sprites](/developer/runtime/video/fdp-sprites) and [FDP mixing](/developer/runtime/video/fdp-mixing).
3. [GameVideo](/developer/runtime/video/game-hle), [Video write logging](/developer/runtime/video/producers) and the per-layer pages.
4. [Presentation](/developer/runtime/video/presentation), [Compare mode](/developer/runtime/video/compare-mode) and [Parity evidence and limits](/developer/runtime/video/parity).
5. [Extending the renderer](/developer/runtime/video/extending) when you are ready to change code.

The long evidence log for the game-data renderer is in [docs/developer/VIDEO-HLE.md](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/VIDEO-HLE.md). The ABI note is in [docs/developer/ABI-CHANGES.md](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/ABI-CHANGES.md). For the surrounding runtime, read [Machine](/developer/runtime/machine) and [Frontend](/developer/runtime/frontend).

Primary sources: [video.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/renderer/fdp/video.cpp), [video.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/renderer/game/video.cpp), and [frontend.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/frontend.cpp).
