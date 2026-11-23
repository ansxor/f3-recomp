//! Embedded GPU shader code.
//!
//! CMake generates `video_shaders.rs` and passes its path through `F3_VIDEO_SHADERS_RS`, which
//! the crate build script turns into the `f3_video_shaders` cfg. Without it (bare
//! `cargo check --features gpu`) the empty table below keeps the backend type-checking; such a
//! build cannot create pipelines.

#[cfg(f3_video_shaders)]
include!(env!("F3_VIDEO_SHADERS_RS"));

#[cfg(not(f3_video_shaders))]
mod fallback {
    use std::ffi::CStr;

    #[derive(Clone, Copy, Debug)]
    pub struct Shader {
        pub spirv: &'static [u8],
        pub msl: &'static [u8],
        pub msl_entry: &'static CStr,
        pub samplers: u32,
        pub buffers: u32,
        pub uniforms: u32,
    }

    const EMPTY: Shader = Shader {
        spirv: &[],
        msl: &[],
        msl_entry: c"main",
        samplers: 0,
        buffers: 0,
        uniforms: 0,
    };

    pub const FULLSCREEN_VERT: Shader = EMPTY;
    pub const SPRITE_VERT: Shader = EMPTY;
    pub const SPRITE_FRAG: Shader = EMPTY;
    pub const SCENE_FRAG: Shader = EMPTY;
    pub const SCENE_INTERP_FRAG: Shader = EMPTY;
    pub const ROTATE_FRAG: Shader = EMPTY;
    pub const POSTPROCESS_FRAG: Shader = EMPTY;
}

#[cfg(not(f3_video_shaders))]
pub use fallback::*;
