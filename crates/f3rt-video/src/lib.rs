//! Taito F3 scanout decoding, composition, and render-only presentation.

pub mod config;
pub mod decode;
pub mod game;
pub mod gpu;
pub mod graphics_assets;
pub(crate) mod memory_map;
pub mod scale;
mod state_io;

pub use config::{
    GameVideoOptions, Geometry, PixelFormat, SpriteList, VideoConfig, VideoScaleMode,
};
pub use decode::{DecodedSpriteEntry, SpritePresentation, SpriteRamState, decode_sprite_list};
pub use graphics_assets::GraphicsAssets;
pub use scale::{VideoBlit, video_blit_for_window, video_scale_for_window};
pub use state_io::{CanonicalRecord, StateReader, StateWriter};
