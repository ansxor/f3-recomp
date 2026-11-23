//! SDL-free GPU façade stubs used when the `gpu` feature is off.
//!
//! The ABI crate bridges one `GpuVideo` type in every configuration; without SDL the constructor
//! fails, which is exactly what a headless build reports to the C++ frontend.

use super::api::{GpuSurface, Postprocess, PresentRequest, RenderRequest};
use super::interp::{InterpolationFields, InterpolationStats, VideoInterpolation};
use super::motion::MotionInterpolationStats;
use crate::config::{GameVideoOptions, Geometry, VideoScaleMode};
use crate::game::scene_snapshot::SceneSnapshot;

const UNSUPPORTED: &str = "built without F3RT_GPU";

/// Owned render texture placeholder.
#[derive(Default)]
pub struct GpuTexture;

impl GpuTexture {
    pub const fn empty() -> Self {
        Self
    }

    pub fn get(&self) -> usize {
        0
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

    pub fn is_valid(&self) -> bool {
        false
    }

    pub fn surface(&self) -> GpuSurface {
        GpuSurface::default()
    }
}

pub struct GpuVideo {
    interpolation: InterpolationStats,
    motion: MotionInterpolationStats,
}

impl GpuVideo {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        _window: usize,
        _options: GameVideoOptions,
        _geometry: Geometry,
        _playfield_assets: &[u8],
        _sprite_assets: &[u8],
        _linear: bool,
        _vsync: bool,
        _interpolation: VideoInterpolation,
        _fields: InterpolationFields,
        _rotation: u32,
    ) -> Result<Self, String> {
        Err(UNSUPPORTED.to_owned())
    }

    pub fn driver_name(&self) -> String {
        String::new()
    }

    pub fn last_interpolation(&self) -> &InterpolationStats {
        &self.interpolation
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

    pub fn set_overlay(&mut self, _callback: usize, _userdata: usize) {}

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

    pub fn present(
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
