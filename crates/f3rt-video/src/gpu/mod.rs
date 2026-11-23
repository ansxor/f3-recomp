//! Scene preparation and optional SDL GPU presentation.

mod api;
mod motion;

#[cfg(any(feature = "gpu", test))]
mod encode;
#[cfg(any(feature = "gpu", test))]
mod interp;
#[cfg(any(feature = "gpu", test))]
mod scene_layout;
#[cfg(any(feature = "gpu", test))]
mod user_spirv;

#[cfg(feature = "gpu")]
mod pacing;
#[cfg(all(feature = "gpu", target_os = "macos"))]
mod pacing_macos;
#[cfg(feature = "gpu")]
mod postprocess;
#[cfg(feature = "gpu")]
mod scene_pass;
#[cfg(feature = "gpu")]
mod sdl_gpu;
#[cfg(feature = "gpu")]
mod shaders;
#[cfg(not(feature = "gpu"))]
mod stub;
#[cfg(feature = "gpu")]
mod video;

pub use api::{
    GpuSurface, GpuVideoConfig, InterpolationFields, Postprocess, PresentRequest, PresentTiming,
    RenderRequest, VideoInterpolation,
};
pub use motion::{GpuMotionHistory, MotionInterpolationStats, MotionResult, MotionState};
#[cfg(not(feature = "gpu"))]
pub use stub::{GpuTexture, GpuVideo};
#[cfg(feature = "gpu")]
pub use {sdl_gpu::GpuTexture, video::GpuVideo};
