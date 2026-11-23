//! Game-side scene model, decoders and frame composition, mechanically ported from
//! `runtime/renderer/game/`.
//!
//! Wave 0 carries the plain data types and pure helpers (`clip`, `mix`, `row_bits`,
//! `layer_visits`, `flicker_hold`, `ram_shadow`). Wave 1 adds the stateful VRAM decoders
//! (`scene`, `palette`, `tiles`, `text`, `sprites`, `lines`). Wave 2A adds the chunk scheduler,
//! sprite coverage/binning, the indexed compositor, the scene snapshot and the frame driver.

pub mod clip;
pub mod flicker_hold;
pub mod indexed_compositor;
pub mod layer_visits;
pub mod lines;
pub mod mix;
pub mod palette;
pub mod ram_shadow;
pub mod row_bits;
pub mod row_workers;
pub mod scene;
pub mod scene_snapshot;
pub mod sprites;
pub mod text;
pub mod tiles;
pub mod video;

pub use scene::{
    ALL_LAYERS, LAYER_COUNT, LayerId, LayerInfo, LayerKind, MAX_HARDWARE_SPRITES,
    MAX_PRESENTED_SPRITES, SCROLLED_LAYERS, SceneClip, SceneLayer, ScenePixel, ScenePlayfield,
    SceneRow, SceneSource, SceneSprite, VideoRam, kind, layer_bit, layer_info, playfield,
    presented_playfield, sprite, sprite_layer, sub_index, wrap_tile_index,
};
pub use scene_snapshot::SceneSnapshot;
pub use video::{GameVideo, SpriteEnhancements, VideoStage};
