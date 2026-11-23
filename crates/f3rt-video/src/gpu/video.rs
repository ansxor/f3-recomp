//! GPU frame preparation, drawing and presentation.

use super::api::{
    GpuVideoConfig, InterpolationFields, Postprocess, PresentAcquire, PresentRequest,
    PresentTiming, RenderRequest, VideoInterpolation, validate_gpu_video_config,
};
use super::encode::{ALL_LAYERS, EncodedScene, GpuPrepScratch, ScenePlan};
use super::interp::{InterpolationConfig, InterpolationLog};
use super::motion::{GpuMotionHistory, MotionInterpolationStats};
use super::pacing::DisplayPacer;
use super::postprocess::{Postprocessor, SurfaceRotator};
use super::scene_pass::{GpuTileAssets, ScenePass};
use super::sdl_gpu::{
    CaptureImage, GpuContext, GpuTexture, GpuTransfer, Overlay, PresentLayout, ReadbackBuffer,
    TextureFormat, TextureRef, save_surface,
};
use crate::config::{GameVideoOptions, Geometry, SpriteList, VideoScaleMode};
use crate::game::scene_snapshot::SceneSnapshot;
use crate::gpu::scene_layout::F3_SCENE_SPRITE_COUNT;

enum RenderedSurface {
    Scene,
    Postprocess,
}

pub struct GpuVideo {
    assets: GpuTileAssets,
    scene_pass: ScenePass,
    postprocess: Postprocessor,
    rotator: SurfaceRotator,
    readback: ReadbackBuffer,
    pacer: DisplayPacer,
    preparation: FramePreparation,
    options: GameVideoOptions,
    geometry: Geometry,
    scale_mode: VideoScaleMode,
    linear: bool,
    overlay: Option<Overlay>,
    rendered: Option<RenderedSurface>,
    last_presented: bool,
    presented_frames: u64,
    saved_pixels: Vec<u32>,
    ctx: GpuContext,
}

impl GpuVideo {
    /// # Safety
    /// SDL video must be initialized. A nonzero window must be a live SDL_Window on the
    /// calling thread and outlive this renderer.
    pub unsafe fn new(
        window: usize,
        config: GpuVideoConfig,
        playfield_assets: &[u8],
        sprite_assets: &[u8],
    ) -> Result<Self, String> {
        let GpuVideoConfig {
            options,
            geometry,
            linear,
            vsync,
            interpolation,
            fields,
            rotation,
        } = config;
        validate_gpu_video_config(options, rotation, playfield_assets, sprite_assets)?;
        // SAFETY: the caller supplies an initialized SDL video subsystem and live window or zero.
        let ctx = unsafe { GpuContext::new(window, vsync)? };
        let assets = GpuTileAssets::new(&ctx, playfield_assets, sprite_assets)?;
        let scene_pass = ScenePass::new(
            &ctx,
            &geometry,
            options,
            interpolation != VideoInterpolation::Off,
        )?;
        let rotator = SurfaceRotator::new(&ctx, &geometry, options, rotation)?;
        let pacer = DisplayPacer::new();
        Ok(Self {
            assets,
            scene_pass,
            postprocess: Postprocessor::default(),
            rotator,
            readback: ReadbackBuffer::default(),
            pacer,
            preparation: FramePreparation {
                config: InterpolationConfig {
                    mode: interpolation,
                    fields,
                },
                ..FramePreparation::default()
            },
            options,
            geometry,
            scale_mode: VideoScaleMode::Fixed,
            linear,
            overlay: None,
            rendered: None,
            last_presented: false,
            presented_frames: 0,
            saved_pixels: Vec::new(),
            ctx,
        })
    }

    pub fn driver_name(&self) -> String {
        self.ctx.driver_name()
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
        self.rendered = None;
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
        self.ctx.device_handle()
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
        if next == self.preparation.config {
            return Ok(());
        }
        if mode != VideoInterpolation::Off {
            self.scene_pass.ensure_interpolation_pipeline(&self.ctx)?;
        }
        self.preparation.config = next;
        self.preparation.log.reset();
        Ok(())
    }

    /// # Safety
    /// A nonzero callback must be an OverlayFn address. It and its userdata must remain valid
    /// until replaced or until this renderer is dropped.
    pub unsafe fn set_overlay(&mut self, callback: usize, userdata: usize) {
        // SAFETY: the host guarantees the ABI and keeps the callback and userdata live.
        self.overlay = unsafe { Overlay::from_host(callback, userdata) };
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
        self.rendered = None;
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
        let encoded = self.preparation.encode(
            ScenePlan {
                scene,
                options: self.options,
                sprite_list: request.sprites,
                layer_mask: request.layer_mask,
            },
            request.alpha,
            &self.assets,
            self.scene_pass.upload_buffer(),
        )?;
        let mut cmd = self.ctx.begin_command()?;
        let composed = self.scene_pass.draw(
            &mut cmd,
            &encoded,
            &self.ctx,
            &self.assets,
            self.options,
            &self.geometry,
        )?;
        let surface = self.postprocess.apply(
            &mut cmd,
            composed,
            &self.ctx,
            self.options.scale,
            request.postprocess,
        )?;
        self.readback.finish(cmd, surface, output)?;
        self.record_rendered(request.postprocess);
        Ok(())
    }

    /// Presents `scene` to the window. Returns whether a swapchain texture was submitted.
    ///
    /// # Safety
    /// Any nonempty left pane must be a live texture belonging to this renderer.
    pub unsafe fn present(
        &mut self,
        scene: &SceneSnapshot,
        request: &PresentRequest<'_>,
    ) -> Result<bool, String> {
        self.last_presented = false;
        if !matches!(request.timing, PresentTiming::Canonical) {
            self.preparation.stats = MotionInterpolationStats::default();
        }
        let mut cmd = self.ctx.begin_command()?;
        if cmd.acquire_for_present(&mut self.pacer, request)? == PresentAcquire::SkipFrame {
            return Ok(false);
        }
        let alpha = request.sample_alpha(monotonic_ns());
        let encoded = self.preparation.encode(
            ScenePlan {
                scene,
                options: self.options,
                sprite_list: SpriteList::Presented,
                layer_mask: ALL_LAYERS,
            },
            alpha,
            &self.assets,
            self.scene_pass.upload_buffer(),
        )?;
        let composed = self.scene_pass.draw(
            &mut cmd,
            &encoded,
            &self.ctx,
            &self.assets,
            self.options,
            &self.geometry,
        )?;
        let post =
            self.postprocess
                .apply(&mut cmd, composed, &self.ctx, self.options.scale, true)?;
        let oriented = self.rotator.apply(&mut cmd, post, &self.ctx)?;
        // SAFETY: the host keeps the optional same-device texture alive through presentation.
        let left_pane = unsafe { TextureRef::from_host(&request.left_pane, &self.ctx) };
        self.last_presented = cmd.present(
            oriented,
            left_pane,
            PresentLayout {
                geometry: &self.geometry,
                options: self.options,
                scale_mode: self.scale_mode,
                linear: self.linear,
                rotated: self.rotator.rotated(),
            },
            self.overlay,
            request.capture_path,
        )?;
        if self.last_presented {
            self.presented_frames += 1;
        }
        self.record_rendered(true);
        Ok(self.last_presented)
    }

    /// Renders the canonical scene through postprocess and rotation into `destination`,
    /// re-allocating it when its size differs. Offscreen; never presents.
    pub fn capture_surface(
        &mut self,
        scene: &SceneSnapshot,
        destination: &mut GpuTexture,
    ) -> Result<(), String> {
        let encoded = self.preparation.encode(
            ScenePlan {
                scene,
                options: self.options,
                sprite_list: SpriteList::Presented,
                layer_mask: ALL_LAYERS,
            },
            None,
            &self.assets,
            self.scene_pass.upload_buffer(),
        )?;
        let mut cmd = self.ctx.begin_command()?;
        let composed = self.scene_pass.draw(
            &mut cmd,
            &encoded,
            &self.ctx,
            &self.assets,
            self.options,
            &self.geometry,
        )?;
        let post =
            self.postprocess
                .apply(&mut cmd, composed, &self.ctx, self.options.scale, true)?;
        let oriented = self.rotator.apply(&mut cmd, post, &self.ctx)?;
        destination.ensure(
            &self.ctx,
            TextureFormat::Bgra8Unorm,
            oriented.width(),
            oriented.height(),
        )?;
        cmd.blit_copy(oriented, destination.as_ref());
        cmd.submit()
    }

    /// Explicit opt-in; completed emulated frames are captured, never draws.
    pub fn capture_motion(&mut self, scene: &SceneSnapshot, frame: u64) -> Result<(), String> {
        if self.preparation.motion.is_none() {
            self.pacer.enable(&self.ctx)?;
            self.preparation.motion = Some(GpuMotionHistory::new(self.options));
        }
        let motion = self
            .preparation
            .motion
            .as_mut()
            .expect("motion history created above");
        if scene.sprite_trail_seed.is_empty() {
            motion.capture(scene, frame);
        } else {
            motion.reset();
        }
        Ok(())
    }

    pub fn reset_motion(&mut self) {
        if let Some(motion) = self.preparation.motion.as_mut() {
            motion.reset();
        }
        self.preparation.stats = MotionInterpolationStats::default();
        self.pacer.reset_prepared();
    }

    pub fn last_motion(&self) -> &MotionInterpolationStats {
        &self.preparation.stats
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
        self.ctx.display_hz()
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
        let input = match self.rendered {
            Some(RenderedSurface::Scene) => self.scene_pass.surface(),
            Some(RenderedSurface::Postprocess) => self.postprocess.surface(),
            None => return Err("No GPU surface has been drawn".to_owned()),
        };
        let mut cmd = self.ctx.begin_command()?;
        let source = self.rotator.apply(&mut cmd, input, &self.ctx)?;
        self.saved_pixels
            .resize((source.width() * source.height()) as usize, 0);
        self.readback.finish(cmd, source, &mut self.saved_pixels)?;
        let png = path
            .rfind('.')
            .is_some_and(|dot| path[dot..].eq_ignore_ascii_case(".png"));
        save_surface(
            path,
            &mut self.saved_pixels,
            source,
            TextureFormat::Bgra8Unorm,
            CaptureImage::Game { png },
        )
    }

    fn record_rendered(&mut self, apply_postprocess: bool) {
        self.rendered = Some(if apply_postprocess && self.postprocess.active() {
            RenderedSurface::Postprocess
        } else {
            RenderedSurface::Scene
        });
    }
}

#[derive(Default)]
struct FramePreparation {
    motion: Option<GpuMotionHistory>,
    scratch: GpuPrepScratch,
    config: InterpolationConfig,
    log: InterpolationLog,
    stats: MotionInterpolationStats,
}

impl FramePreparation {
    fn encode<'a>(
        &mut self,
        plan: ScenePlan<'a>,
        alpha: Option<f32>,
        assets: &GpuTileAssets,
        upload: &mut GpuTransfer,
    ) -> Result<EncodedScene<'a>, String> {
        let scene = plan.scene;
        if scene.sprite_list_for(plan.sprite_list).len() > F3_SCENE_SPRITE_COUNT {
            return Err("GPU sprite count out of range".to_owned());
        }
        if scene.fallback
            && let Some(motion) = self.motion.as_mut()
        {
            motion.reset();
        }
        let motion = plan.apply_motion(self.motion.as_ref(), alpha, &mut self.scratch.motion_state);
        let prepared = motion.analyze_interpolation(
            self.config,
            assets.tile_pen_masks(),
            &mut self.scratch.interp_coefficients,
        );
        let encoded = {
            let mut mapping = upload.map(true, "Map GPU frame upload")?;
            let words = mapping.words(prepared.required_words());
            prepared.encode_into(words, assets.pf_tile_count(), assets.sp_tile_count())?
        };
        if alpha.is_some() {
            self.stats = encoded.motion_stats;
        }
        if let Some(line) = self.log.line_if_changed(self.config, &encoded.interp_stats) {
            println!("{line}");
        }
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
