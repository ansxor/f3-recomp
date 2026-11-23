//! GPU boundary for builds without SDL support.

use super::api::{
    GpuVideoConfig, InterpolationFields, Postprocess, PresentRequest, RenderRequest,
    VideoInterpolation,
};
use super::motion::MotionInterpolationStats;
use crate::config::VideoScaleMode;
use crate::game::scene_snapshot::SceneSnapshot;

const UNSUPPORTED: &str = "built without F3RT_GPU";

/// Owned render texture placeholder.
#[derive(Default)]
pub struct GpuTexture;

impl GpuTexture {
    pub const fn empty() -> Self {
        Self
    }

    pub fn texture_handle(&self) -> usize {
        0
    }

    pub fn width(&self) -> u32 {
        0
    }

    pub fn height(&self) -> u32 {
        0
    }
}

pub struct GpuVideo {
    motion: MotionInterpolationStats,
}

impl GpuVideo {
    /// # Safety
    /// The host must honor the GPU backend contract when compiled with GPU support.
    pub unsafe fn new(
        _window: usize,
        _config: GpuVideoConfig,
        _playfield_assets: &[u8],
        _sprite_assets: &[u8],
    ) -> Result<Self, String> {
        Err(UNSUPPORTED.to_owned())
    }

    pub fn driver_name(&self) -> String {
        String::new()
    }

    pub fn set_scale(&mut self, _scale: u32) -> Result<(), String> {
        Err(UNSUPPORTED.to_owned())
    }

    pub fn set_scale_mode(&mut self, _mode: VideoScaleMode) {}

    pub fn presented_frames(&self) -> u64 {
        0
    }

    pub fn device(&self) -> usize {
        0
    }

    pub fn set_linear(&mut self, _linear: bool) {}

    pub fn set_interpolation(
        &mut self,
        _mode: VideoInterpolation,
        _fields: InterpolationFields,
    ) -> Result<(), String> {
        Err(UNSUPPORTED.to_owned())
    }

    /// # Safety
    /// The host must honor the GPU backend contract when compiled with GPU support.
    pub unsafe fn set_overlay(&mut self, _callback: usize, _userdata: usize) {}

    pub fn set_postprocess(
        &mut self,
        _preset: Postprocess,
        _user_shader: &str,
    ) -> Result<(), String> {
        Err(UNSUPPORTED.to_owned())
    }

    pub fn postprocess(&self) -> Postprocess {
        Postprocess::Off
    }

    pub fn render(
        &mut self,
        _scene: &SceneSnapshot,
        _output: &mut [u32],
        _request: &RenderRequest,
    ) -> Result<(), String> {
        Err(UNSUPPORTED.to_owned())
    }

    /// # Safety
    /// The host must honor the GPU backend contract when compiled with GPU support.
    pub unsafe fn present(
        &mut self,
        _scene: &SceneSnapshot,
        _request: &PresentRequest<'_>,
    ) -> Result<bool, String> {
        Err(UNSUPPORTED.to_owned())
    }

    pub fn capture_surface(
        &mut self,
        _scene: &SceneSnapshot,
        _destination: &mut GpuTexture,
    ) -> Result<(), String> {
        Err(UNSUPPORTED.to_owned())
    }

    pub fn capture_motion(&mut self, _scene: &SceneSnapshot, _frame: u64) -> Result<(), String> {
        Err(UNSUPPORTED.to_owned())
    }

    pub fn reset_motion(&mut self) {}

    pub fn last_motion(&self) -> &MotionInterpolationStats {
        &self.motion
    }

    pub fn pace_motion(&mut self) -> bool {
        false
    }

    pub fn last_presented(&self) -> bool {
        false
    }

    pub fn display_hz(&self) -> f64 {
        0.0
    }

    pub fn display_callback_hz(&self) -> f64 {
        0.0
    }

    pub fn requested_display_hz(&self) -> f64 {
        0.0
    }

    pub fn save_surface(&mut self, _path: &str) -> Result<(), String> {
        Err(UNSUPPORTED.to_owned())
    }
}
