//! Taito F3 renderer, mechanically ported from `runtime/renderer/`.
//!
//! Wave 0 covers the pure leaf modules: configuration/geometry, window scaling, the sprite
//! display-list decoder, ROM graphics unpacking, user SPIR-V validation, and the game-side
//! `ram_shadow` / `clip` / `mix` / `row_bits` / `layer_visits` / `flicker_hold` helpers.
//! Wave 1 adds the stateful VRAM decoders (`palette`, `tiles`, `text`, `sprites`, `lines`) and
//! the snapshot serialization contract (`state_io`). Wave 2A adds the chunk scheduler
//! (`row_workers`), chunked sprite raster (`SpriteCoverage`/`SpriteBins`), the indexed compositor,
//! `SceneSnapshot` and the `GameVideo` frame driver and save-state root.
//! Wave 2B adds the GPU prep chain (`gpu::scene_layout`, `gpu::motion`, `gpu::interp`,
//! `gpu::encode`). Wave 3 cuts the C++ runtime over through `crates/f3rt-abi/src/video.rs`; the
//! surviving C++ files are thin façades plus the independent FDP/oracle renderer.
//! These are `rlib` modules of `f3rt-abi`, the single staticlib the C++ runtime links.

pub mod config;
pub mod decode;
pub mod game;
pub mod gpu;
pub mod graphics_assets;
pub mod memory_map;
pub mod scale;
pub mod state_io;

pub use config::{
    GameVideoOptions, Geometry, PixelFormat, SpriteList, VideoConfig, VideoScaleMode,
};
pub use decode::{DecodedSpriteEntry, SpritePresentation, SpriteRamState, decode_sprite_list};
pub use graphics_assets::GraphicsAssets;
pub use scale::{VideoBlit, video_blit_for_window, video_scale_for_window};
pub use state_io::{CanonicalRecord, StateReader, StateWriter};
