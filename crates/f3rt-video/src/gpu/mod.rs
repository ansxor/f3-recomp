//! GPU renderer, mechanically ported from `runtime/renderer/gpu/`.
//!
//! Wave 0 carries the user-shader validation/loading path. Wave 2B adds the scene buffer layout,
//! the render-only motion history, interpolation analysis and the scene encoder. Wave 4 adds the
//! complete SDL3 GPU backend behind the [`driver::GpuDriver`] vtable: [`sdl_gpu`] object
//! lifetimes and presentation, [`scene_pass`], [`postprocess`], [`pacing`] and [`video`]. The
//! C++ runtime keeps thin façades plus the SDL3 driver shim (`sdl_driver_shim.cpp`).

pub mod driver;
pub mod encode;
pub mod interp;
#[cfg(test)]
pub mod mock_driver;
pub mod motion;
pub mod pacing;
pub mod postprocess;
pub mod scene_layout;
pub mod scene_pass;
pub mod sdl_gpu;
pub mod user_spirv;
pub mod video;

pub use driver::{
    Blit, DriverRef, Filter, GpuDriver, GpuDriverVtable, LoadOp, RawBuffer, RawCommand,
    RawCopyPass, RawDevice, RawPacing, RawPipeline, RawRenderPass, RawSampler, RawTexture,
    RawTransfer, RawWindow, SdlDriver, ShaderKind, SwapchainOut, TextureFormat,
    TextureSamplerBinding, TransferUsage, driver_ref,
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
pub use pacing::{DisplayPacer, TickWait};
pub use postprocess::{Postprocess, Postprocessor, SurfaceRotator};
pub use scene_layout::SceneUniforms;
pub use scene_pass::{GpuTileAssets, ScenePass, TILE_BYTES};
pub use sdl_gpu::{
    GpuBuffer, GpuCommand, GpuContext, GpuMapping, GpuPipeline, GpuSampler, GpuSurface, GpuTexture,
    GpuTransfer, PresentAcquire, PresentRequest, PresentTiming, ReadbackBuffer,
};
pub use user_spirv::{
    LoadedUserShader, MAX_USER_SHADER_BYTES, load_user_shader, spirv, validate_user_spirv,
};
pub use video::{GpuVideo, RenderRequest};
