//! The GPU video façade, mechanically ported from `runtime/renderer/gpu/video.{hpp,cpp}`.
//!
//! `prepare_scene` runs the whole `ScenePlan -> MotionScene -> PreparedScene -> encode_into`
//! chain inside Rust, encoding straight into the mapped upload slice: no intermediate copy and no
//! per-frame FFI crossing.

use super::driver::{DriverRef, RawDevice, RawWindow, TextureFormat};
use super::encode::{ALL_LAYERS, EncodedScene, GpuPrepScratch, ScenePlan};
use super::interp::{
    InterpolationConfig, InterpolationFields, InterpolationLog, InterpolationStats,
    VideoInterpolation,
};
use super::motion::{GpuMotionHistory, MotionInterpolationStats};
use super::pacing::DisplayPacer;
use super::postprocess::{Postprocess, Postprocessor, SurfaceRotator};
use super::scene_pass::{GpuTileAssets, ScenePass};
use super::sdl_gpu::{
    GpuContext, GpuSurface, GpuTexture, PresentAcquire, PresentRequest, PresentTiming,
    ReadbackBuffer,
};
use crate::config::{GameVideoOptions, Geometry, SpriteList, VideoScaleMode};
use crate::game::scene_snapshot::SceneSnapshot;
use crate::gpu::scene_layout::F3_SCENE_SPRITE_COUNT;

/// One offscreen draw request.
#[derive(Clone, Copy, Debug)]
pub struct RenderRequest {
    pub sprites: SpriteList,
    /// Output/layer-mask diagnostics remain baseline unless `postprocess` is explicitly true.
    pub layer_mask: u32,
    /// Temporal geometry at this alpha (needs captured motion history); `None` renders the
    /// canonical frame.
    pub alpha: Option<f32>,
    pub postprocess: bool,
}

impl Default for RenderRequest {
    fn default() -> Self {
        Self {
            sprites: SpriteList::Presented,
            layer_mask: ALL_LAYERS,
            alpha: None,
            postprocess: false,
        }
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
    overlay: bool,
    rendered: bool,
    last_presented: bool,
    presented_frames: u64,
    unrotated_surface: GpuSurface,
    saved_pixels: Vec<u32>,
    ctx: GpuContext,
}

fn validate_config(
    options: GameVideoOptions,
    rotation: u32,
    playfield_assets: &[u8],
    sprite_assets: &[u8],
) -> Result<(), String> {
    SurfaceRotator::validate(rotation)?;
    if options.scale == 0
        || options.scale > GameVideoOptions::MAX_GPU_SCALE
        || options.border > GameVideoOptions::MAX_BORDER
    {
        return Err("Game GPU presentation scale/border out of range".to_owned());
    }
    GpuTileAssets::validate(playfield_assets, sprite_assets)
}

impl GpuVideo {
    /// SDL video must be initialized. Window may be absent for offscreen parity.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        driver: DriverRef,
        window: Option<RawWindow>,
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
        validate_config(options, rotation, playfield_assets, sprite_assets)?;
        let ctx = GpuContext::new(driver.clone(), window, vsync)?;
        let assets = GpuTileAssets::new(&ctx, playfield_assets, sprite_assets)?;
        let scene_pass = ScenePass::new(
            &ctx,
            &geometry,
            options,
            interpolation != VideoInterpolation::Off,
        )?;
        let rotator = SurfaceRotator::new(&ctx, &geometry, options, rotation)?;
        let pacer = DisplayPacer::new(driver.clone());
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
            overlay: false,
            rendered: false,
            last_presented: false,
            presented_frames: 0,
            unrotated_surface: GpuSurface::default(),
            saved_pixels: Vec::new(),
            ctx,
        })
    }

    pub fn driver(&self) -> &dyn super::driver::GpuDriver {
        self.ctx.driver()
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

    pub fn device(&self) -> RawDevice {
        self.ctx.device()
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

    pub fn set_overlay(&mut self, enabled: bool) {
        self.overlay = enabled;
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
        let alpha = request.sample_alpha(self.ctx.driver());
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
            self.pacer.enable(
                self.ctx.device(),
                self.ctx.window().is_some(),
                self.ctx.vsync(),
            )?;
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
        self.ctx.driver().save_surface(
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

impl Drop for GpuVideo {
    fn drop(&mut self) {
        self.ctx.wait_idle();
    }
}

// The field order above makes the context (and device) drop last.

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::VideoConfig;
    use crate::gpu::driver::{DriverRef, RawWindow, TextureFormat};
    use crate::gpu::mock_driver::{MockGpuDriver, MockSwapchain};
    use std::ffi::c_void;
    use std::rc::Rc;

    const TILE_BYTES: usize = 256;

    fn window() -> Option<RawWindow> {
        Some(RawWindow(std::ptr::dangling_mut::<c_void>()))
    }

    fn make_video_with_vsync(
        mock: &Rc<MockGpuDriver>,
        window: Option<RawWindow>,
        vsync: bool,
    ) -> GpuVideo {
        let playfield = [0u8; TILE_BYTES];
        let sprites = [1u8; TILE_BYTES];
        let driver: DriverRef = mock.clone();
        GpuVideo::new(
            driver,
            window,
            GameVideoOptions::default(),
            Geometry::default(),
            &playfield,
            &sprites,
            false,
            vsync,
            VideoInterpolation::Off,
            InterpolationFields::Geometry,
            0,
        )
        .expect("mock GPU video")
    }

    fn make_video(mock: &Rc<MockGpuDriver>, window: Option<RawWindow>) -> GpuVideo {
        make_video_with_vsync(mock, window, false)
    }

    fn scene() -> SceneSnapshot {
        SceneSnapshot::new(&VideoConfig::default())
    }

    #[test]
    fn set_scale_failure_keeps_the_previous_scale_and_targets() {
        let mock = Rc::new(MockGpuDriver::default());
        let mut video = make_video(&mock, None);
        let baseline = video.scene_pass.targets().surface.surface().texture;
        mock.fail_after("texture", 1);
        let error = video.set_scale(2).unwrap_err();
        assert_eq!(error, "Create GPU render texture: mock failure");
        assert_eq!(video.options.scale, 1);
        assert_eq!(
            video.scene_pass.targets().surface.surface().texture,
            baseline
        );
        assert!(
            !mock.released().is_empty(),
            "the partial target is released"
        );

        mock.clear_failures();
        video.set_scale(2).expect("scale");
        assert_eq!(video.options.scale, 2);
        let surface = video.scene_pass.targets().surface.surface();
        assert_eq!(surface.width, 640);
        assert_eq!(surface.height, 464);
    }

    #[test]
    fn set_interpolation_failure_keeps_the_previous_mode() {
        let mock = Rc::new(MockGpuDriver::default());
        let mut video = make_video(&mock, None);
        mock.fail_after("pipeline", 0);
        let error = video
            .set_interpolation(VideoInterpolation::Linear, InterpolationFields::Geometry)
            .unwrap_err();
        assert_eq!(error, "Create GPU graphics pipeline: mock failure");
        assert_eq!(video.interp_config.mode, VideoInterpolation::Off);
        assert!(video.scene_pass.interpolation_pipeline().is_none());

        mock.clear_failures();
        video
            .set_interpolation(VideoInterpolation::Linear, InterpolationFields::All)
            .expect("interpolation");
        assert_eq!(video.interp_config.mode, VideoInterpolation::Linear);
        assert!(video.scene_pass.interpolation_pipeline().is_some());
    }

    #[test]
    fn set_postprocess_failure_retains_the_last_valid_preset() {
        let mock = Rc::new(MockGpuDriver::default());
        let mut video = make_video(&mock, None);
        mock.fail_after("texture", 0);
        let error = video.set_postprocess(Postprocess::Crt, "").unwrap_err();
        assert_eq!(error, "Create GPU render texture: mock failure");
        assert_eq!(video.postprocess(), Postprocess::Off);
        assert!(video.postprocess.pipeline().is_none());
        assert!(video.postprocess.surface().is_none());

        mock.clear_failures();
        video.set_postprocess(Postprocess::Crt, "").expect("crt");
        assert_eq!(video.postprocess(), Postprocess::Crt);
        assert!(video.postprocess.surface().is_some());
        let error = video
            .set_postprocess(Postprocess::User, "/missing.spv")
            .unwrap_err();
        assert!(error.starts_with("Cannot open user shader"), "{error}");
        assert_eq!(video.postprocess(), Postprocess::Crt);
        assert!(video.postprocess.surface().is_some());

        video.set_postprocess(Postprocess::Off, "").expect("off");
        assert_eq!(video.postprocess(), Postprocess::Off);
        assert!(video.postprocess.pipeline().is_none());
        assert!(video.postprocess.surface().is_none());
    }

    #[test]
    fn render_rejects_a_short_output_and_reads_back_a_full_one() {
        let mock = Rc::new(MockGpuDriver::default());
        let mut video = make_video(&mock, None);
        let scene = scene();
        let mut short = vec![0u32; 10];
        assert_eq!(
            video
                .render(&scene, &mut short, &RenderRequest::default())
                .unwrap_err(),
            "Incomplete GPU output buffer"
        );
        let mut output = vec![
            0u32;
            (video.options.width(&video.geometry) * video.options.height(&video.geometry))
                as usize
        ];
        video
            .render(&scene, &mut output, &RenderRequest::default())
            .expect("render");
        assert_eq!(mock.call_count("download_texture"), 1);
        assert!(video.rendered);
        assert!(!video.last_presented());
    }

    #[test]
    fn present_skips_a_busy_swapchain_without_uploading() {
        let mock = Rc::new(MockGpuDriver::default());
        let mut video = make_video(&mock, window());
        mock.set_swapchain(MockSwapchain::None);
        let uploads = mock.call_count("upload_buffer");
        let presented = video
            .present(&scene(), &PresentRequest::default())
            .expect("present");
        assert!(!presented);
        assert_eq!(mock.call_count("upload_buffer"), uploads);
        assert_eq!(video.presented_frames(), 0);
        assert!(!video.last_presented());
    }

    #[test]
    fn present_composes_a_left_pane_and_captures_the_window() {
        let mock = Rc::new(MockGpuDriver::default());
        let mut video = make_video(&mock, window());
        mock.set_swapchain(MockSwapchain::Drawable {
            width: 64,
            height: 32,
        });
        let left = video
            .ctx
            .create_texture(TextureFormat::Bgra8Unorm, 16, 16)
            .expect("left pane");
        video.set_overlay(true);
        let request = PresentRequest {
            wait: true,
            left_pane: left.surface(),
            capture_path: Some("shot.png"),
            ..PresentRequest::default()
        };
        assert!(video.present(&scene(), &request).expect("present"));
        assert_eq!(video.presented_frames(), 1);
        assert_eq!(mock.call_count("blit"), 3, "two panes plus the drawable");
        assert_eq!(mock.call_count("download_texture"), 1);
        assert_eq!(mock.call_count("invoke_overlay"), 1);
        let saved = mock.saved_surfaces();
        assert_eq!(saved.len(), 1);
        assert_eq!(saved[0].path, "shot.png");
        assert_eq!((saved[0].width, saved[0].height), (64, 32));
        assert_eq!(saved[0].pixel_format, 1);
        assert!(saved[0].png);
    }

    #[test]
    fn capture_surface_reallocates_the_destination() {
        let mock = Rc::new(MockGpuDriver::default());
        let mut video = make_video(&mock, None);
        let mut destination = GpuTexture::empty();
        video
            .capture_surface(&scene(), &mut destination)
            .expect("capture");
        assert!(destination.is_valid());
        let surface = destination.surface();
        assert_eq!(surface.width, 320);
        assert_eq!(surface.height, 232);
        assert!(mock.call_count("upload_buffer") > 0);
    }

    #[test]
    fn capture_motion_enables_the_pacer_once() {
        let mock = Rc::new(MockGpuDriver::default());
        let mut video = make_video(&mock, window());
        video.capture_motion(&scene(), 1).expect("capture");
        video.capture_motion(&scene(), 2).expect("capture");
        assert_eq!(mock.frames_in_flight(), vec![1]);
        assert_eq!(
            mock.call_count("create_pacer"),
            0,
            "vsync off has no display link"
        );
        assert!(!video.pace_motion());
        video.reset_motion();
        assert!(!video.last_motion().paired);
    }

    #[test]
    fn capture_motion_creates_the_display_link_when_vsync_is_on() {
        let mock = Rc::new(MockGpuDriver::default());
        let mut video = make_video_with_vsync(&mock, window(), true);
        video.capture_motion(&scene(), 1).expect("capture");
        assert_eq!(mock.call_count("create_pacer"), 1);
        assert_eq!(mock.frames_in_flight(), vec![1]);
    }

    #[test]
    fn save_surface_needs_a_draw_then_writes_png() {
        let mock = Rc::new(MockGpuDriver::default());
        let mut video = make_video(&mock, None);
        assert_eq!(
            video.save_surface("shot.png").unwrap_err(),
            "No GPU surface has been drawn"
        );
        let mut output = vec![
            0u32;
            (video.options.width(&video.geometry) * video.options.height(&video.geometry))
                as usize
        ];
        video
            .render(&scene(), &mut output, &RenderRequest::default())
            .expect("render");
        video.save_surface("shot.PNG").expect("save");
        let saved = mock.saved_surfaces();
        assert_eq!(saved.len(), 1);
        assert!(saved[0].png);
        assert_eq!(saved[0].pixel_format, 1);
    }

    #[test]
    fn scale_validation_and_rotation_boundaries() {
        let mock = Rc::new(MockGpuDriver::default());
        let mut video = make_video(&mock, None);
        assert_eq!(video.set_scale(0).unwrap_err(), "GPU scale must be 1..8");
        assert_eq!(video.set_scale(9).unwrap_err(), "GPU scale must be 1..8");
        let playfield = [0u8; TILE_BYTES];
        let sprites = [0u8; TILE_BYTES];
        let driver: DriverRef = mock.clone();
        let error = GpuVideo::new(
            driver,
            None,
            GameVideoOptions::default(),
            Geometry::default(),
            &playfield,
            &sprites,
            false,
            false,
            VideoInterpolation::Off,
            InterpolationFields::Geometry,
            45,
        )
        .err()
        .expect("rotation must fail");
        assert_eq!(error, "Unsupported GPU presentation rotation");
    }
}
