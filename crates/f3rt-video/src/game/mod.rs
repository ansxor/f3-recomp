//! Decoded scanout scenes and the frame pipeline.

pub(crate) mod clip;
pub mod flicker_hold;
pub mod full_detail;
pub mod indexed_compositor;
pub(crate) mod layer_visits;
pub mod lines;
pub(crate) mod mix;
pub mod palette;
pub(crate) mod ram_shadow;
pub(crate) mod row_bits;
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
    SceneRow, SceneSprite, VideoRam, kind, layer_bit, layer_info, playfield, presented_playfield,
    sprite, sprite_layer, sub_index, wrap_tile_index,
};
pub use scene_snapshot::SceneSnapshot;
pub use video::{GameVideo, SpriteEnhancements, VideoStage};
