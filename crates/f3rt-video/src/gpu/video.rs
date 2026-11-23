//! The GPU video façade, mechanically ported from `runtime/renderer/gpu/video.{hpp,cpp}`.
//!
//! `prepare_scene` runs the whole `ScenePlan -> MotionScene -> PreparedScene -> encode_into`
//! chain inside Rust, encoding straight into the mapped upload slice: no intermediate copy and no
//! per-frame FFI crossing.

// Raw GPU handles are only ever passed back to SDL3 or overlaid on C++-owned hosts.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use super::api::{
    GpuSurface, Postprocess, PresentAcquire, PresentRequest, PresentTiming, RenderRequest,
    validate_gpu_video_config,
};
use super::encode::{ALL_LAYERS, EncodedScene, GpuPrepScratch, ScenePlan};
use super::interp::{
    InterpolationConfig, InterpolationFields, InterpolationLog, InterpolationStats,
    VideoInterpolation,
};
use super::motion::{GpuMotionHistory, MotionInterpolationStats};
use super::pacing::DisplayPacer;
use super::postprocess::{Postprocessor, SurfaceRotator};
use super::scene_pass::{GpuTileAssets, ScenePass};
use super::sdl_gpu::{GpuContext, GpuTexture, ReadbackBuffer, TextureFormat, save_surface};
use crate::config::{GameVideoOptions, Geometry, SpriteList, VideoScaleMode};
use crate::game::scene_snapshot::SceneSnapshot;
use crate::gpu::scene_layout::F3_SCENE_SPRITE_COUNT;
use sdl3_sys::gpu::{SDL_GPUCommandBuffer, SDL_GPUTexture};
use std::ffi::c_void;

/// Host overlay invoked on the window composition target after the game blit.
#[derive(Clone, Copy, Debug, Default)]
pub struct Overlay {
    callback: usize,
    userdata: usize,
}

type OverlayFn =
    unsafe extern "C" fn(*mut c_void, *mut SDL_GPUCommandBuffer, *mut SDL_GPUTexture, u32, u32);

impl Overlay {
    pub fn new(callback: usize, userdata: usize) -> Option<Self> {
        (callback != 0).then_some(Self { callback, userdata })
    }

    pub fn invoke(
        self,
        command: *mut SDL_GPUCommandBuffer,
        target: usize,
        width: u32,
        height: u32,
    ) {
        // SAFETY: the host keeps the callback live for the video's whole lifetime and it accepts
        // the SDL3 GPU command buffer and composition target.
        let callback: OverlayFn = unsafe { std::mem::transmute(self.callback) };
        unsafe {
            callback(
                self.userdata as *mut c_void,
                command,
                target as *mut SDL_GPUTexture,
                width,
                height,
            )
        };
    }
}

pub struct GpuVideo {
    assets: GpuTileAssets,
    scene_pass: ScenePass,
    postprocess: Postprocessor,
    rotator: SurfaceRotator,
    readback: ReadbackBuffer,
    pacer: DisplayPacer,
    motion: Option<GpuMotionHistory>,
    prep_scratch: GpuPrepScratch,
    interp_config: InterpolationConfig,
    interpolation_stats: InterpolationStats,
    interp_log: InterpolationLog,
    motion_stats: MotionInterpolationStats,
    options: GameVideoOptions,
    geometry: Geometry,
    scale_mode: VideoScaleMode,
    linear: bool,
    overlay: Option<Overlay>,
    rendered: bool,
    last_presented: bool,
    presented_frames: u64,
    unrotated_surface: GpuSurface,
    saved_pixels: Vec<u32>,
    ctx: GpuContext,
}

impl GpuVideo {
    /// SDL video must be initialized. Window may be absent for offscreen parity.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        window: usize,
        options: GameVideoOptions,
        geometry: Geometry,
        playfield_assets: &[u8],
        sprite_assets: &[u8],
        linear: bool,
        vsync: bool,
        interpolation: VideoInterpolation,
        fields: InterpolationFields,
        rotation: u32,
    ) -> Result<Self, String> {
        validate_gpu_video_config(options, rotation, playfield_assets, sprite_assets)?;
        let ctx = GpuContext::new(window, vsync)?;
        let assets = GpuTileAssets::new(&ctx, playfield_assets, sprite_assets)?;
        let scene_pass = ScenePass::new(
            &ctx,
            &geometry,
            options,
            interpolation != VideoInterpolation::Off,
        )?;
        let rotator = SurfaceRotator::new(&ctx, &geometry, options, rotation)?;
        let pacer = DisplayPacer::new(window as *mut sdl3_sys::video::SDL_Window);
        Ok(Self {
            assets,
            scene_pass,
            postprocess: Postprocessor::default(),
            rotator,
            readback: ReadbackBuffer::default(),
            pacer,
            motion: None,
            prep_scratch: GpuPrepScratch::default(),
            interp_config: InterpolationConfig {
                mode: interpolation,
                fields,
            },
            interpolation_stats: InterpolationStats::default(),
            interp_log: InterpolationLog::default(),
            motion_stats: MotionInterpolationStats::default(),
            options,
            geometry,
            scale_mode: VideoScaleMode::Fixed,
            linear,
            overlay: None,
            rendered: false,
            last_presented: false,
            presented_frames: 0,
            unrotated_surface: GpuSurface::default(),
            saved_pixels: Vec::new(),
            ctx,
        })
    }

    pub fn driver_name(&self) -> String {
        self.ctx.driver_name()
    }

    pub fn last_interpolation(&self) -> &InterpolationStats {
        &self.interpolation_stats
    }

    /// Host-only geometry. Assets, pipelines and canonical game state survive. Strong guarantee:
    /// on failure the previous scale and every live resource are untouched.
    pub fn set_scale(&mut self, scale: u32) -> Result<(), String> {
        if scale == 0 || scale > GameVideoOptions::MAX_GPU_SCALE {
            return Err("GPU scale must be 1..8".to_owned());
        }
        if scale == self.options.scale {
            return Ok(());
        }
        let next = GameVideoOptions {
            scale,
            ..self.options
        };
        let next_scene = ScenePass::reallocate_targets(&self.ctx, &self.geometry, next)?;
        let next_post = self
            .postprocess
            .reallocate_target(&self.ctx, &self.geometry, next)?;
        let next_rotated = self
            .rotator
            .reallocate_target(&self.ctx, &self.geometry, next)?;
        let mut next_pixels = Vec::new();
        if !self.saved_pixels.is_empty() {
            next_pixels.resize(
                (next.width(&self.geometry) * next.height(&self.geometry)) as usize,
                0,
            );
        }
        // The driver defers destruction of replaced targets until queued users finish: no
        // GPU-idle stall.
        self.scene_pass.commit_targets(next_scene);
        self.postprocess.commit_target(next_post);
        self.rotator.commit_target(next_rotated);
        self.readback.reset();
        self.saved_pixels = next_pixels;
        self.options = next;
        self.rendered = false;
        self.unrotated_surface = GpuSurface::default();
        self.interpolation_stats = InterpolationStats::default();
        Ok(())
    }

    pub fn set_scale_mode(&mut self, mode: VideoScaleMode) {
        self.scale_mode = mode;
    }

    /// Accepted swapchain submissions.
    pub fn presented_frames(&self) -> u64 {
        self.presented_frames
    }

    pub fn device(&self) -> usize {
        self.ctx.device() as usize
    }

    pub fn set_linear(&mut self, linear: bool) {
        self.linear = linear;
    }

    /// Live scanline interpolation mode and fields; takes effect on the next draw. Off/on
    /// changes only switch the pipeline and coefficient upload, so assets and buffers persist.
    /// Strong guarantee: if the pipeline cannot be built the previous mode stays.
    pub fn set_interpolation(
        &mut self,
        mode: VideoInterpolation,
        fields: InterpolationFields,
    ) -> Result<(), String> {
        let next = InterpolationConfig { mode, fields };
        if next == self.interp_config {
            return Ok(());
        }
        if mode != VideoInterpolation::Off {
            self.scene_pass.ensure_interpolation_pipeline(&self.ctx)?;
        }
        self.interp_config = next;
        self.interpolation_stats = InterpolationStats::default();
        self.interp_log.reset();
        Ok(())
    }

    pub fn set_overlay(&mut self, callback: usize, userdata: usize) {
        self.overlay = Overlay::new(callback, userdata);
    }

    /// See `Postprocessor::configure` for the user shader ABI. Errors retain the last valid
    /// preset.
    pub fn set_postprocess(
        &mut self,
        preset: Postprocess,
        user_shader: &str,
    ) -> Result<(), String> {
        self.postprocess
            .configure(&self.ctx, &self.geometry, self.options, preset, user_shader)?;
        self.rendered = false;
        self.unrotated_surface = GpuSurface::default();
        Ok(())
    }

    pub fn postprocess(&self) -> Postprocess {
        self.postprocess.preset()
    }

    /// Renders `scene` offscreen and reads back width*height ARGB8888 pixels (unrotated), waiting
    /// on the GPU fence before returning.
    pub fn render(
        &mut self,
        scene: &SceneSnapshot,
        output: &mut [u32],
        request: &RenderRequest,
    ) -> Result<(), String> {
        let pixels =
            (self.options.width(&self.geometry) * self.options.height(&self.geometry)) as usize;
        if output.len() < pixels {
            return Err("Incomplete GPU output buffer".to_owned());
        }
        self.last_presented = false;
        let encoded =
            self.prepare_scene(scene, request.sprites, request.layer_mask, request.alpha)?;
        let mut cmd = self.ctx.begin_command()?;
        let composed = self.scene_pass.draw(
            &mut cmd,
            &encoded,
            &self.ctx,
            &self.assets,
            self.options,
            &self.geometry,
        )?;
        let final_surface = self.postprocess.apply(
            &mut cmd,
            composed,
            &self.ctx,
            &self.geometry,
            self.options,
            self.postprocess.active() && request.postprocess,
        )?;
        self.readback
            .finish(&mut cmd, final_surface, &self.ctx, output)?;
        self.unrotated_surface = final_surface;
        self.rendered = true;
        Ok(())
    }

    /// Presents `scene` to the window. Returns whether a swapchain texture was submitted.
    pub fn present(
        &mut self,
        scene: &SceneSnapshot,
        request: &PresentRequest<'_>,
    ) -> Result<bool, String> {
        self.last_presented = false;
        if !matches!(request.timing, PresentTiming::Canonical) {
            self.motion_stats = MotionInterpolationStats::default();
        }
        let mut cmd = self.ctx.begin_command()?;
        if cmd.acquire_for_present(&self.ctx, &mut self.pacer, request)?
            == PresentAcquire::SkipFrame
        {
            return Ok(false);
        }
        let alpha = request.sample_alpha(monotonic_ns());
        let encoded = self.prepare_scene(scene, SpriteList::Presented, ALL_LAYERS, alpha)?;
        let composed = self.scene_pass.draw(
            &mut cmd,
            &encoded,
            &self.ctx,
            &self.assets,
            self.options,
            &self.geometry,
        )?;
        let post = self.postprocess.apply(
            &mut cmd,
            composed,
            &self.ctx,
            &self.geometry,
            self.options,
            self.postprocess.active(),
        )?;
        let oriented = self.rotator.apply(&mut cmd, post, &self.ctx)?;
        self.last_presented = cmd.present(
            &self.ctx,
            oriented,
            request.left_pane,
            self.scale_mode,
            self.options,
            self.linear,
            self.rotator.rotated(),
            self.overlay,
            request.capture_path,
            &self.geometry,
        )?;
        if self.last_presented {
            self.presented_frames += 1;
        }
        self.unrotated_surface = post;
        self.rendered = true;
        Ok(self.last_presented)
    }

    /// Renders the canonical scene through postprocess and rotation into `destination`,
    /// re-allocating it when its size differs. Offscreen; never presents.
    pub fn capture_surface(
        &mut self,
        scene: &SceneSnapshot,
        destination: &mut GpuTexture,
    ) -> Result<(), String> {
        let encoded = self.prepare_scene(scene, SpriteList::Presented, ALL_LAYERS, None)?;
        let mut cmd = self.ctx.begin_command()?;
        let composed = self.scene_pass.draw(
            &mut cmd,
            &encoded,
            &self.ctx,
            &self.assets,
            self.options,
            &self.geometry,
        )?;
        let post = self.postprocess.apply(
            &mut cmd,
            composed,
            &self.ctx,
            &self.geometry,
            self.options,
            self.postprocess.active(),
        )?;
        let oriented = self.rotator.apply(&mut cmd, post, &self.ctx)?;
        destination.ensure(
            &self.ctx,
            TextureFormat::Bgra8Unorm,
            oriented.width,
            oriented.height,
        )?;
        cmd.blit_copy(oriented, destination.surface());
        cmd.submit()
    }

    /// Explicit opt-in; completed emulated frames are captured, never draws.
    pub fn capture_motion(&mut self, scene: &SceneSnapshot, frame: u64) -> Result<(), String> {
        if self.motion.is_none() {
            self.pacer
                .enable(self.ctx.device(), self.ctx.has_window(), self.ctx.vsync())?;
            self.motion = Some(GpuMotionHistory::new(self.options));
        }
        let motion = self.motion.as_mut().expect("motion history created above");
        if scene.sprite_trail_seed.is_empty() {
            motion.capture(scene, frame);
        } else {
            motion.reset();
        }
        Ok(())
    }

    pub fn reset_motion(&mut self) {
        if let Some(motion) = self.motion.as_mut() {
            motion.reset();
        }
        self.motion_stats = MotionInterpolationStats::default();
        self.pacer.reset_prepared();
    }

    pub fn last_motion(&self) -> &MotionInterpolationStats {
        &self.motion_stats
    }

    /// Pace before testing the native-frame deadline so the selected pair is current at this
    /// display tick. Returns false without an active native pacer.
    pub fn pace_motion(&mut self) -> bool {
        let vsync = self.ctx.vsync();
        self.pacer.pace(vsync)
    }

    pub fn last_presented(&self) -> bool {
        self.last_presented
    }

    pub fn display_hz(&self) -> f64 {
        self.pacer.display_hz()
    }

    pub fn display_callback_hz(&self) -> f64 {
        self.pacer.callback_hz()
    }

    pub fn requested_display_hz(&self) -> f64 {
        self.pacer.requested_hz()
    }

    /// Saves the last rendered game image, including active postprocessing, but excluding the
    /// crisp swapchain overlay. A preset/scale change needs a draw.
    pub fn save_surface(&mut self, path: &str) -> Result<(), String> {
        if !self.rendered {
            return Err("No GPU surface has been drawn".to_owned());
        }
        let mut cmd = self.ctx.begin_command()?;
        let source = self
            .rotator
            .apply(&mut cmd, self.unrotated_surface, &self.ctx)?;
        self.saved_pixels
            .resize((source.width * source.height) as usize, 0);
        self.readback
            .finish(&mut cmd, source, &self.ctx, &mut self.saved_pixels)?;
        let png = match path.rfind('.') {
            Some(dot) => path[dot..].eq_ignore_ascii_case(".png"),
            None => false,
        };
        save_surface(
            path,
            &self.saved_pixels,
            source.width,
            source.height,
            1,
            png,
            "Create GPU capture surface",
            "Save GPU surface",
        )
    }

    fn prepare_scene<'a>(
        &mut self,
        scene: &'a SceneSnapshot,
        list: SpriteList,
        layer_mask: u32,
        alpha: Option<f32>,
    ) -> Result<EncodedScene<'a>, String> {
        if scene.sprite_list_for(list).len() > F3_SCENE_SPRITE_COUNT {
            return Err("GPU sprite count out of range".to_owned());
        }
        if scene.fallback
            && let Some(motion) = self.motion.as_mut()
        {
            motion.reset();
        }
        let plan = ScenePlan {
            scene,
            options: self.options,
            sprite_list: list,
            layer_mask,
        };
        let motion = plan.apply_motion(
            self.motion.as_ref(),
            alpha,
            &mut self.prep_scratch.motion_state,
        );
        let prepared = motion.analyze_interpolation(
            self.interp_config,
            self.assets.tile_pen_masks(),
            &mut self.prep_scratch.interp_coefficients,
        );
        let encoded = {
            let upload = self.scene_pass.upload_buffer();
            let mut mapping = upload.map(true, "Map GPU frame upload")?;
            let words = mapping.words(prepared.required_words());
            prepared.encode_into(
                words,
                self.assets.pf_tile_count(),
                self.assets.sp_tile_count(),
            )?
        };
        if alpha.is_some() {
            self.motion_stats = encoded.motion_stats;
        }
        self.interpolation_stats = encoded.interp_stats;
        self.interp_log
            .observe(self.interp_config, &self.interpolation_stats);
        Ok(encoded)
    }
}

fn monotonic_ns() -> u64 {
    use std::time::{SystemTime, UNIX_EPOCH};
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_nanos() as u64)
        .unwrap_or(0)
}

impl Drop for GpuVideo {
    fn drop(&mut self) {
        self.ctx.wait_idle();
    }
}

// The field order above makes the context (and device) drop last.
