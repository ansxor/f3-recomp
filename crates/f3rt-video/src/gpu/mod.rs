//! GPU renderer.
//!
//! The pure prep chain (`scene_layout`, `motion`, `interp`, `encode`, `user_spirv`) and the
//! SDL-free request/validation types build in every configuration. The SDL3 backend
//! (`sdl_gpu`, `scene_pass`, `postprocess`, `pacing`, `video`) is compiled only behind the `gpu`
//! Cargo feature, which CMake enables for `F3RT_SDL && F3RT_GPU` builds and links against
//! `SDL3::SDL3`. Without it, [`GpuVideo`] is an SDL-free stub that reports it was built without
//! `F3RT_GPU`.

mod api;
pub mod encode;
pub mod interp;
pub mod motion;
pub mod scene_layout;
pub mod user_spirv;

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
#[cfg(feature = "gpu")]
mod video;

#[cfg(not(feature = "gpu"))]
mod stub;

pub use api::{
    GpuSurface, Postprocess, PresentAcquire, PresentRequest, PresentTiming, RenderRequest,
    TILE_BYTES, validate_gpu_video_config, validate_rotation, validate_tile_assets,
};
pub use encode::{
    ALL_LAYERS, ENCODED_INTERPOLATION_WORDS, ENCODED_NATIVE_WORDS, ENCODED_SCENE_WORDS,
    EncodedScene, GpuPrepScratch, GpuUniforms, MotionScene, PreparedScene, ScenePlan,
    encoded_native_words,
};
pub use interp::{
    INTERPOLATION_PALETTE, INTERPOLATION_SOURCE, INTERPOLATION_VERTICAL, INTERPOLATION_ZOOM,
    InterpolationAnalysis, InterpolationCoefficients, InterpolationConfig, InterpolationFields,
    InterpolationLog, InterpolationReason, InterpolationStats, LayerInterpolationStats,
    PlayfieldCoefficients, VideoInterpolation, analyze_gpu_interpolation,
    analyze_gpu_interpolation_into, interpolation_fields_name, interpolation_reason_name,
    parse_interpolation_fields,
};
pub use motion::{
    GpuMotionHistory, MOTION_LAYER_MASK, MotionInterpolationStats, MotionResult, MotionRowState,
    MotionState,
};
pub use scene_layout::SceneUniforms;
pub use user_spirv::{
    LoadedUserShader, MAX_USER_SHADER_BYTES, load_user_shader, spirv, validate_user_spirv,
};

#[cfg(feature = "gpu")]
pub use pacing::{DisplayPacer, TickWait};
#[cfg(feature = "gpu")]
pub use postprocess::{Postprocessor, SurfaceRotator};
#[cfg(feature = "gpu")]
pub use scene_pass::{GpuTileAssets, ScenePass};
#[cfg(feature = "gpu")]
pub use sdl_gpu::{
    GpuBuffer, GpuCommand, GpuContext, GpuMapping, GpuPipeline, GpuSampler, GpuTexture,
    GpuTransfer, ReadbackBuffer, TextureFormat,
};
#[cfg(feature = "gpu")]
pub use video::{GpuVideo, Overlay};

#[cfg(not(feature = "gpu"))]
pub use stub::{GpuTexture, GpuVideo};
