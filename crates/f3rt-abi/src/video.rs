//! C++ runtime boundary for the F3 renderer.

use f3rt_video as v;
use f3rt_video::config::SpriteList as RVSpriteList;
use f3rt_video::config::VideoConfig as RVVideoConfig;
use f3rt_video::config::{GameVideoOptions as RVOptions, PixelFormat as RVPixelFormat};
use f3rt_video::decode as rdecode;
use f3rt_video::game::flicker_hold as rflicker;
use f3rt_video::game::full_detail as rfulldetail;
use f3rt_video::game::video::{
    FrameSpritePresentation, GameVideo as RGameVideo, SpriteEnhancements, VideoStage,
};
use f3rt_video::game::{SceneRow as RSceneRow, SceneSprite as RSceneSprite};

use std::pin::Pin;

// CXX generates Rust ABI shims with the existing C++ argument lists.
#[allow(clippy::too_many_arguments)]
#[cxx::bridge(namespace = "f3rt::rust_video")]
mod ffi {
    /// `f3rt::VideoConfig`.
    struct VideoConfig {
        rotation: u32,
        sprite_lag: u32,
        visible_y: u32,
        visible_height: u32,
        extend: bool,
        extended_alt_maps: bool,
        full_resolution_alt_maps: bool,
    }

    /// `f3rt::GameVideoOptions` (only the fields the renderer reads).
    struct GameVideoOptions {
        scale: u32,
        border: u32,
    }

    /// `f3rt::ScenePixel`.
    struct ScenePixel {
        palette: u16,
        flags: u8,
    }

    /// `f3rt::SceneSprite`; layout shared with the C++ mirror (`scene.hpp`).
    struct SceneSprite {
        x: i32,
        y: i32,
        scale_x: u16,
        scale_y: u16,
        tile: u32,
        palette: u8,
        flip_x: bool,
        flip_y: bool,
        group: u64,
        group_x: i32,
        group_y: i32,
        group_pc: u32,
        blend_weight: f32,
    }

    /// `f3rt::SceneLayer`.
    struct SceneLayer {
        priority: u8,
        blend_mode: u8,
        clip_enabled: u8,
        clip_inverted: u8,
        clip_inverse: bool,
        enabled: bool,
        blend_select: bool,
        mosaic: bool,
    }

    /// `f3rt::ScenePlayfield`.
    struct ScenePlayfield {
        layer: SceneLayer,
        source_x: i32,
        source_y: i32,
        x_step: i32,
        y_step: i32,
        y_fraction: u8,
        palette_add: u16,
        alt_map: bool,
        empty_row: bool,
        full_res_offset: i16,
    }

    /// `f3rt::SceneClip`.
    struct SceneClip {
        left: i16,
        right: i16,
    }

    /// `f3rt::SceneRow`; layout shared with the C++ mirror.
    struct SceneRow {
        playfields: [ScenePlayfield; 4],
        sprites: [SceneLayer; 4],
        text: SceneLayer,
        clips: [SceneClip; 4],
        blend: [u8; 4],
        background: u16,
        text_x: i16,
        text_y: i16,
        mosaic_period: u8,
        bitmap: bool,
        palette_15bit: bool,
        blur: bool,
    }

    /// `f3rt::SpriteRamState` (`renderer/decode.hpp`).
    struct SpriteRamState {
        flipscreen: bool,
        bank: bool,
        trails: bool,
        extra_planes: u8,
        pen_mask: u8,
    }

    /// `f3rt::DecodedSpriteEntry` (`renderer/decode.hpp`).
    struct DecodedSpriteEntry {
        x: i32,
        y: i32,
        scale_x: i32,
        scale_y: i32,
        tile: u32,
        color: u8,
        flip_x: bool,
        flip_y: bool,
        pri: u8,
        group: u64,
        group_x: i32,
        group_y: i32,
        group_pc: u32,
    }

    /// `f3rt::MotionInterpolationStats`.
    struct MotionInterpolationStats {
        sprites: u32,
        playfield_rows: u32,
        text_rows: u32,
        moving_sprites: u32,
        moving_playfield_rows: u32,
        moving_text_rows: u32,
        rejected_sprites: u32,
        rejected_rows: u32,
        sprite_count_rejections: u32,
        sprite_group_rejections: u32,
        sprite_transform_rejections: u32,
        sprite_jump_rejections: u32,
        row_control_rejections: u32,
        row_transform_rejections: u32,
        row_jump_rejections: u32,
        paired: bool,
        alpha: f32,
    }

    struct MotionResult {
        stats: MotionInterpolationStats,
        sprite_x: [i32; 4096],
        sprite_y: [i32; 4096],
    }

    /// Live element counts for the C++ host mirror of a `SceneSnapshot`.
    struct SceneMeta {
        sprite_count: u32,
        canonical_sprite_count: u32,
        pen_mask: u8,
        flipped: bool,
        sprite_flipped: bool,
        fallback: bool,
        trail_count: u32,
    }

    /// Scanout geometry (`f3rt::geometry`) for the GPU renderer, which carries it as data.
    struct GpuGeometry {
        first_line: u32,
        height: u32,
    }

    /// `f3rt::RenderRequest`.
    struct GpuRenderRequest {
        sprites: SpriteList,
        layer_mask: u32,
        has_alpha: bool,
        alpha: f32,
        postprocess: bool,
    }

    /// `f3rt::PresentTiming` reduced to an ABI tag plus its payload.
    enum GpuPresentTiming {
        Canonical,
        Fixed,
        Live,
    }

    /// `f3rt::PresentRequest`; `capture_path` is empty when unset.
    struct GpuPresentRequest {
        wait: bool,
        timing: GpuPresentTiming,
        alpha: f32,
        frame_start_ns: u64,
        period_ns: i64,
        left_pane_handle: usize,
        left_pane_width: u32,
        left_pane_height: u32,
        capture_path: String,
    }

    enum VideoInterpolation {
        Off,
        Linear,
        Fit,
    }

    enum InterpolationFields {
        None,
        Geometry,
        Palette,
        All,
    }

    enum SpriteList {
        Presented,
        Canonical,
    }

    /// `f3rt::VideoScaleMode` (`renderer/scale.hpp`).
    enum VideoScaleMode {
        Fixed,
        Auto,
        AutoInteger,
    }

    enum Postprocess {
        Off,
        Crt,
        User,
    }

    enum PixelFormat {
        Argb8888,
        Rgba8888,
    }

    // Host-side adaptors; see `include/f3rt/bridge_video_host.hpp`.
    unsafe extern "C++" {
        include!("f3rt/bridge_video_host.hpp");

        type EnhancementsHost;
        fn empty(&self) -> bool;
        fn present(
            self: Pin<&mut EnhancementsHost>,
            latched: &[SceneSprite],
            flipped: bool,
            out: &mut [SceneSprite],
        ) -> usize;
        fn reset(self: Pin<&mut EnhancementsHost>);
        fn report(&self) -> String;
    }

    unsafe extern "C++" {
        type ProbeHost;
        fn enter(self: Pin<&mut ProbeHost>, stage: u8);
    }

    struct SpriteRoot {
        key: u64,
        x: i32,
        y: i32,
        pc: u32,
    }

    struct FullDetailRecord {
        valid: bool,
        flags0: u8,
        color: u8,
        desc: u16,
        screen_x: i16,
        screen_y: i16,
        zoom_x: u8,
        zoom_y: u8,
    }

    unsafe extern "C++" {
        type SpriteGroupsHost;
        fn current_root(&self, key: u64) -> SpriteRoot;
        fn full_detail_record(&self, key: u64, group_x: i32, group_y: i32) -> FullDetailRecord;
    }

    extern "Rust" {
        type GraphicsAssets;
        fn new_graphics_assets() -> Box<GraphicsAssets>;
        fn load_roms(
            &mut self,
            sprites: &[u8],
            sprites_hi: &[u8],
            tilemap: &[u8],
            tilemap_hi: &[u8],
        ) -> bool;
        fn sprite_tiles(&self) -> &[u8];
        fn playfield_tiles(&self) -> &[u8];
        fn sprite_tile(&self, code: u32) -> &[u8];
        fn playfield_tile(&self, code: u32) -> &[u8];
    }

    extern "Rust" {
        type SpritePresentation;
        fn new_sprite_presentation() -> Box<SpritePresentation>;
        fn clear(&mut self);
        fn push_group(&mut self, key: u64, x: i32, y: i32, pc: u32);
        fn is_empty(&self) -> bool;

        fn decode_sprites(
            spriteram: &[u8],
            visible_y: i32,
            visible_height: i32,
            out: &mut [DecodedSpriteEntry],
            state: &mut SpriteRamState,
            apply_flip: bool,
            presentation: &SpritePresentation,
        ) -> usize;
    }

    extern "Rust" {
        type SceneSnapshot;
        fn new_scene_snapshot(video: VideoConfig) -> Box<SceneSnapshot>;
        fn clone_scene_snapshot(&self) -> Box<SceneSnapshot>;
        fn capture_fallback(&mut self, native: &[u32]);
        fn compose(
            &self,
            output: &mut [u32],
            stride: usize,
            format: PixelFormat,
            border: u32,
            assets: &GraphicsAssets,
            list: SpriteList,
        ) -> Result<()>;
        fn sprite_count(&self, list: SpriteList) -> usize;
        fn trail_len(&self) -> usize;
        fn text_map(&self, out: &mut [u16]);
        fn set_text_map(&mut self, map: &[u16]);
        fn text_pixel(&self, x: i32, y: i32, flipped: bool) -> ScenePixel;
        fn export_scene(
            &self,
            rows: &mut [SceneRow],
            sprites: &mut [SceneSprite],
            canonical: &mut [SceneSprite],
            colors: &mut [u32],
            trail: &mut [u16],
        ) -> SceneMeta;
        fn export_native(&self, native: &mut [u32]);
        fn import_scene(
            &mut self,
            rows: &[SceneRow],
            sprites: &[SceneSprite],
            canonical: &[SceneSprite],
            colors: &[u32],
            trail: &[u16],
            meta: &SceneMeta,
        );
        fn import_native(&mut self, native: &[u32]);
    }

    extern "Rust" {
        type GameVideo;
        fn new_game_video(video: VideoConfig) -> Box<GameVideo>;
        fn reset(&mut self, assets: &GraphicsAssets);
        fn reset_with(&mut self, assets: &GraphicsAssets, enhancements: Pin<&mut EnhancementsHost>);
        fn render_frame(
            &mut self,
            assets: &GraphicsAssets,
            graphics: &[u8],
            control: &[u8],
            palette: &[u8],
            frame: u64,
            presentation: &SpritePresentation,
            probe: Pin<&mut ProbeHost>,
            enhancements: Pin<&mut EnhancementsHost>,
            target: &mut SceneSnapshot,
        );
        fn materialize_native(&mut self, assets: &GraphicsAssets, target: &SceneSnapshot);
        fn native_pixels(&self) -> &[u32];
        fn set_native_pixels(&mut self, pixels: &[u32]);
        fn state_size(&self) -> usize;
        fn save_state(&mut self, dst: &mut [u8], assets: &GraphicsAssets) -> Result<()>;
        fn load_state(
            &mut self,
            src: &[u8],
            enhancements: Pin<&mut EnhancementsHost>,
            target: &mut SceneSnapshot,
        ) -> Result<()>;
        fn report(&self) -> String;
        fn row(&self, screen_y: u32) -> SceneRow;
        fn playfield_pixel(
            &self,
            layer: u32,
            x: i32,
            y: i32,
            flipped: bool,
            assets: &GraphicsAssets,
            alt: bool,
        ) -> ScenePixel;
        fn text_pixel(&self, x: i32, y: i32, flipped: bool) -> ScenePixel;
        fn raster_sprites(&self, assets: &GraphicsAssets, plane: &mut [u16]);
        fn sprite_list(&self, out: &mut [SceneSprite]) -> usize;
    }

    extern "Rust" {
        type GpuMotionHistory;
        fn new_motion_history(options: GameVideoOptions) -> Box<GpuMotionHistory>;
        fn capture(&mut self, scene: &SceneSnapshot, frame: u64);

        fn apply(&self, scene: &SceneSnapshot, alpha: f32) -> MotionResult;
    }

    extern "Rust" {
        type GpuVideo;
        unsafe fn new_gpu_video(
            window: usize,
            options: GameVideoOptions,
            geometry: GpuGeometry,
            playfield_assets: &[u8],
            sprite_assets: &[u8],
            linear: bool,
            vsync: bool,
            interpolation: VideoInterpolation,
            fields: InterpolationFields,
            rotation: u32,
        ) -> Result<Box<GpuVideo>>;
        fn driver(&self) -> String;

        fn set_scale(&mut self, scale: u32) -> Result<()>;
        fn set_scale_mode(&mut self, mode: VideoScaleMode);
        fn presented_frames(&self) -> u64;
        fn device(&self) -> usize;
        fn set_linear(&mut self, linear: bool);
        fn set_interpolation(
            &mut self,
            mode: VideoInterpolation,
            fields: InterpolationFields,
        ) -> Result<()>;
        unsafe fn set_overlay(&mut self, callback: usize, userdata: usize);
        fn set_postprocess(&mut self, preset: Postprocess, user_shader: &str) -> Result<()>;
        fn postprocess(&self) -> Postprocess;
        fn render(
            &mut self,
            scene: &SceneSnapshot,
            output: &mut [u32],
            request: &GpuRenderRequest,
        ) -> Result<()>;
        unsafe fn present(
            &mut self,
            scene: &SceneSnapshot,
            request: &GpuPresentRequest,
        ) -> Result<bool>;
        fn capture_surface(
            &mut self,
            scene: &SceneSnapshot,
            destination: &mut GpuTexture,
        ) -> Result<()>;
        fn capture_motion(&mut self, scene: &SceneSnapshot, frame: u64) -> Result<()>;
        fn reset_motion(&mut self);
        fn last_motion(&self) -> MotionInterpolationStats;
        fn pace_motion(&mut self) -> bool;
        fn last_presented(&self) -> bool;
        fn display_hz(&self) -> f64;
        fn display_callback_hz(&self) -> f64;
        fn requested_display_hz(&self) -> f64;
        fn save_surface(&mut self, path: &str) -> Result<()>;
    }

    extern "Rust" {
        type GpuTexture;
        fn new_gpu_texture() -> Box<GpuTexture>;
        fn texture_handle(&self) -> usize;
        fn texture_width(&self) -> u32;
        fn texture_height(&self) -> u32;
    }

    extern "Rust" {
        type FlickerHold;
        fn new_flicker_hold(
            name: &str,
            group_class: u8,
            anchor_class: u8,
            period: u8,
            self_follow: bool,
        ) -> Box<FlickerHold>;
        fn present(
            &mut self,
            sprites: &mut Vec<SceneSprite>,
            groups: &SpriteGroupsHost,
            game_frame: u16,
            flipped: bool,
        );
        fn reset(&mut self);
        fn report(&self) -> String;
    }

    extern "Rust" {
        type CommandwFullDetail;
        fn new_commandw_full_detail(visible_y: i32, visible_height: i32)
        -> Box<CommandwFullDetail>;
        fn present(
            &mut self,
            sprites: &mut Vec<SceneSprite>,
            groups: &SpriteGroupsHost,
            rom: &[u8],
            flipped: bool,
        );
        fn reset(&mut self);
        fn report(&self) -> String;
        fn expanded_objects(&self) -> u64;
    }
}

fn video_interpolation(value: ffi::VideoInterpolation) -> v::gpu::VideoInterpolation {
    match value.repr {
        1 => v::gpu::VideoInterpolation::Linear,
        2 => v::gpu::VideoInterpolation::Fit,
        _ => v::gpu::VideoInterpolation::Off,
    }
}

fn interpolation_fields(value: ffi::InterpolationFields) -> v::gpu::InterpolationFields {
    v::gpu::InterpolationFields::from_bits_truncate(value.repr)
}

fn sprite_list(value: ffi::SpriteList) -> RVSpriteList {
    match value.repr {
        1 => RVSpriteList::Canonical,
        _ => RVSpriteList::Presented,
    }
}

fn pixel_format(value: ffi::PixelFormat) -> RVPixelFormat {
    match value.repr {
        1 => RVPixelFormat::Rgba8888,
        _ => RVPixelFormat::Argb8888,
    }
}

fn video_config(value: &ffi::VideoConfig) -> RVVideoConfig {
    RVVideoConfig {
        rotation: value.rotation,
        sprite_lag: value.sprite_lag,
        visible_y: value.visible_y,
        visible_height: value.visible_height,
        extend: value.extend,
        extended_alt_maps: value.extended_alt_maps,
        full_resolution_alt_maps: value.full_resolution_alt_maps,
    }
}

fn options(value: &ffi::GameVideoOptions) -> RVOptions {
    RVOptions {
        scale: value.scale,
        border: value.border,
    }
}

fn scene_layer(value: &ffi::SceneLayer) -> v::game::scene::SceneLayer {
    v::game::scene::SceneLayer {
        priority: value.priority,
        blend_mode: value.blend_mode,
        clip_enabled: value.clip_enabled,
        clip_inverted: value.clip_inverted,
        clip_inverse: value.clip_inverse,
        enabled: value.enabled,
        blend_select: value.blend_select,
        mosaic: value.mosaic,
    }
}

fn scene_playfield(value: &ffi::ScenePlayfield) -> v::game::scene::ScenePlayfield {
    v::game::scene::ScenePlayfield {
        layer: scene_layer(&value.layer),
        source_x: value.source_x,
        source_y: value.source_y,
        x_step: value.x_step,
        y_step: value.y_step,
        y_fraction: value.y_fraction,
        palette_add: value.palette_add,
        alt_map: value.alt_map,
        empty_row: value.empty_row,
        full_res_offset: value.full_res_offset,
    }
}

fn scene_row(value: &ffi::SceneRow) -> RSceneRow {
    RSceneRow {
        playfields: value.playfields.each_ref().map(scene_playfield),
        sprites: value.sprites.each_ref().map(scene_layer),
        text: scene_layer(&value.text),
        clips: value.clips.each_ref().map(|clip| v::game::SceneClip {
            left: clip.left,
            right: clip.right,
        }),
        blend: value.blend,
        background: value.background,
        text_x: value.text_x,
        text_y: value.text_y,
        mosaic_period: value.mosaic_period,
        bitmap: value.bitmap,
        palette_15bit: value.palette_15bit,
        blur: value.blur,
    }
}

fn scene_sprite(value: &ffi::SceneSprite) -> RSceneSprite {
    RSceneSprite {
        x: value.x,
        y: value.y,
        scale_x: value.scale_x,
        scale_y: value.scale_y,
        tile: value.tile,
        palette: value.palette,
        flip_x: value.flip_x,
        flip_y: value.flip_y,
        group: value.group,
        group_x: value.group_x,
        group_y: value.group_y,
        group_pc: value.group_pc,
        blend_weight: value.blend_weight,
    }
}

fn out_scene_layer(value: &v::game::scene::SceneLayer) -> ffi::SceneLayer {
    ffi::SceneLayer {
        priority: value.priority,
        blend_mode: value.blend_mode,
        clip_enabled: value.clip_enabled,
        clip_inverted: value.clip_inverted,
        clip_inverse: value.clip_inverse,
        enabled: value.enabled,
        blend_select: value.blend_select,
        mosaic: value.mosaic,
    }
}

fn out_scene_playfield(value: &v::game::scene::ScenePlayfield) -> ffi::ScenePlayfield {
    ffi::ScenePlayfield {
        layer: out_scene_layer(&value.layer),
        source_x: value.source_x,
        source_y: value.source_y,
        x_step: value.x_step,
        y_step: value.y_step,
        y_fraction: value.y_fraction,
        palette_add: value.palette_add,
        alt_map: value.alt_map,
        empty_row: value.empty_row,
        full_res_offset: value.full_res_offset,
    }
}

fn out_scene_row(value: &RSceneRow) -> ffi::SceneRow {
    ffi::SceneRow {
        playfields: value.playfields.each_ref().map(out_scene_playfield),
        sprites: value.sprites.each_ref().map(out_scene_layer),
        text: out_scene_layer(&value.text),
        clips: value.clips.each_ref().map(|clip| ffi::SceneClip {
            left: clip.left,
            right: clip.right,
        }),
        blend: value.blend,
        background: value.background,
        text_x: value.text_x,
        text_y: value.text_y,
        mosaic_period: value.mosaic_period,
        bitmap: value.bitmap,
        palette_15bit: value.palette_15bit,
        blur: value.blur,
    }
}

fn out_scene_sprite(value: &RSceneSprite) -> ffi::SceneSprite {
    ffi::SceneSprite {
        x: value.x,
        y: value.y,
        scale_x: value.scale_x,
        scale_y: value.scale_y,
        tile: value.tile,
        palette: value.palette,
        flip_x: value.flip_x,
        flip_y: value.flip_y,
        group: value.group,
        group_x: value.group_x,
        group_y: value.group_y,
        group_pc: value.group_pc,
        blend_weight: value.blend_weight,
    }
}

fn out_scene_pixel(value: v::game::scene::ScenePixel) -> ffi::ScenePixel {
    ffi::ScenePixel {
        palette: value.palette,
        flags: value.flags,
    }
}

fn out_motion_stats(value: &v::gpu::MotionInterpolationStats) -> ffi::MotionInterpolationStats {
    ffi::MotionInterpolationStats {
        sprites: value.sprites,
        playfield_rows: value.playfield_rows,
        text_rows: value.text_rows,
        moving_sprites: value.moving_sprites,
        moving_playfield_rows: value.moving_playfield_rows,
        moving_text_rows: value.moving_text_rows,
        rejected_sprites: value.rejected_sprites,
        rejected_rows: value.rejected_rows,
        sprite_count_rejections: value.sprite_count_rejections,
        sprite_group_rejections: value.sprite_group_rejections,
        sprite_transform_rejections: value.sprite_transform_rejections,
        sprite_jump_rejections: value.sprite_jump_rejections,
        row_control_rejections: value.row_control_rejections,
        row_transform_rejections: value.row_transform_rejections,
        row_jump_rejections: value.row_jump_rejections,
        paired: value.paired,
        alpha: value.alpha,
    }
}

fn new_graphics_assets() -> Box<GraphicsAssets> {
    GraphicsAssets::new_graphics_assets()
}

fn new_sprite_presentation() -> Box<SpritePresentation> {
    SpritePresentation::new_sprite_presentation()
}

fn new_scene_snapshot(video: ffi::VideoConfig) -> Box<SceneSnapshot> {
    SceneSnapshot::new_scene_snapshot(video)
}

fn new_game_video(video: ffi::VideoConfig) -> Box<GameVideo> {
    GameVideo::new_game_video(video)
}

fn new_motion_history(options: ffi::GameVideoOptions) -> Box<GpuMotionHistory> {
    GpuMotionHistory::new_motion_history(options)
}

fn new_flicker_hold(
    name: &str,
    group_class: u8,
    anchor_class: u8,
    period: u8,
    self_follow: bool,
) -> Box<FlickerHold> {
    FlickerHold::new_flicker_hold(name, group_class, anchor_class, period, self_follow)
}

fn new_commandw_full_detail(visible_y: i32, visible_height: i32) -> Box<CommandwFullDetail> {
    CommandwFullDetail::new_commandw_full_detail(visible_y, visible_height)
}

pub struct GraphicsAssets {
    inner: v::GraphicsAssets,
}

impl GraphicsAssets {
    fn new_graphics_assets() -> Box<Self> {
        Box::new(Self {
            inner: v::GraphicsAssets::default(),
        })
    }

    fn load_roms(
        &mut self,
        sprites: &[u8],
        sprites_hi: &[u8],
        tilemap: &[u8],
        tilemap_hi: &[u8],
    ) -> bool {
        self.inner
            .load_roms(sprites, sprites_hi, tilemap, tilemap_hi)
    }

    fn sprite_tiles(&self) -> &[u8] {
        self.inner.sprite_tiles()
    }

    fn playfield_tiles(&self) -> &[u8] {
        self.inner.playfield_tiles()
    }

    fn sprite_tile(&self, code: u32) -> &[u8] {
        self.inner.sprite_tile(code)
    }

    fn playfield_tile(&self, code: u32) -> &[u8] {
        self.inner.playfield_tile(code)
    }
}

pub struct SpritePresentation {
    groups: Vec<rdecode::SpriteGroupSlot>,
}

impl SpritePresentation {
    fn new_sprite_presentation() -> Box<Self> {
        Box::new(Self { groups: Vec::new() })
    }

    fn clear(&mut self) {
        self.groups.clear();
    }

    fn push_group(&mut self, key: u64, x: i32, y: i32, pc: u32) {
        self.groups.push(rdecode::SpriteGroupSlot { key, x, y, pc });
    }

    fn is_empty(&self) -> bool {
        self.groups.is_empty()
    }
}

fn decode_sprites(
    spriteram: &[u8],
    visible_y: i32,
    visible_height: i32,
    out: &mut [ffi::DecodedSpriteEntry],
    state: &mut ffi::SpriteRamState,
    apply_flip: bool,
    presentation: &SpritePresentation,
) -> usize {
    let mut ram = rdecode::SpriteRamState {
        flipscreen: state.flipscreen,
        bank: state.bank,
        trails: state.trails,
        extra_planes: state.extra_planes,
        pen_mask: state.pen_mask,
    };
    let mut decoded = vec![rdecode::DecodedSpriteEntry::default(); out.len()];
    let view = rdecode::SpritePresentation {
        groups: &presentation.groups,
    };
    let count = rdecode::decode_sprite_list(
        spriteram,
        visible_y..visible_y + visible_height,
        &mut decoded,
        &mut ram,
        apply_flip,
        (!view.empty()).then_some(&view),
    );
    for (dst, src) in out.iter_mut().zip(decoded.iter()).take(count) {
        *dst = ffi::DecodedSpriteEntry {
            x: src.x,
            y: src.y,
            scale_x: src.scale_x,
            scale_y: src.scale_y,
            tile: src.tile,
            color: src.color,
            flip_x: src.flip_x,
            flip_y: src.flip_y,
            pri: src.pri,
            group: src.group,
            group_x: src.group_x,
            group_y: src.group_y,
            group_pc: src.group_pc,
        };
    }
    state.flipscreen = ram.flipscreen;
    state.bank = ram.bank;
    state.trails = ram.trails;
    state.extra_planes = ram.extra_planes;
    state.pen_mask = ram.pen_mask;
    count
}

pub struct SceneSnapshot {
    inner: v::game::SceneSnapshot,
}

impl SceneSnapshot {
    fn new_scene_snapshot(video: ffi::VideoConfig) -> Box<Self> {
        Box::new(Self {
            inner: v::game::SceneSnapshot::new(&video_config(&video)),
        })
    }

    fn clone_scene_snapshot(&self) -> Box<Self> {
        Box::new(Self {
            inner: self.inner.clone(),
        })
    }

    fn capture_fallback(&mut self, native: &[u32]) {
        self.inner.capture_fallback(native);
    }

    fn compose(
        &self,
        output: &mut [u32],
        stride: usize,
        format: ffi::PixelFormat,
        border: u32,
        assets: &GraphicsAssets,
        list: ffi::SpriteList,
    ) -> Result<(), String> {
        let tile_rows =
            v::game::indexed_compositor::TileRowTable::new(assets.inner.playfield_tiles());
        let mut scratch = v::game::indexed_compositor::IndexedScratch::new();
        self.inner.compose(
            v::game::indexed_compositor::IndexedOutput {
                pixels: output,
                layout: v::game::indexed_compositor::IndexedLayout {
                    stride,
                    format: pixel_format(format),
                },
            },
            border,
            v::game::indexed_compositor::IndexedTiles {
                playfield_rows: &tile_rows,
                playfield_pixels: assets.inner.playfield_tiles(),
                sprite_pixels: assets.inner.sprite_tiles(),
            },
            &mut scratch,
            sprite_list(list),
            v::game::indexed_compositor::RowExecution::default(),
        )
    }

    fn sprite_count(&self, list: ffi::SpriteList) -> usize {
        self.inner.sprite_list_for(sprite_list(list)).len()
    }

    fn trail_len(&self) -> usize {
        self.inner.sprite_trail_seed.len()
    }

    fn text_map(&self, out: &mut [u16]) {
        let map = self.inner.text.map();
        let count = out.len().min(map.len());
        out[..count].copy_from_slice(&map[..count]);
    }

    fn set_text_map(&mut self, map: &[u16]) {
        let target = self.inner.text.map_mut();
        let count = map.len().min(target.len());
        target[..count].copy_from_slice(&map[..count]);
    }

    fn text_pixel(&self, x: i32, y: i32, flipped: bool) -> ffi::ScenePixel {
        out_scene_pixel(self.inner.text.pixel(x, y, flipped, false))
    }

    fn export_scene(
        &self,
        rows: &mut [ffi::SceneRow],
        sprites: &mut [ffi::SceneSprite],
        canonical: &mut [ffi::SceneSprite],
        colors: &mut [u32],
        trail: &mut [u16],
    ) -> ffi::SceneMeta {
        let presented = self.inner.sprite_list();
        let canonical_list = self.inner.canonical_sprite_list();
        for (dst, src) in rows.iter_mut().zip(self.inner.rows.iter()) {
            *dst = out_scene_row(src);
        }
        for (dst, src) in sprites.iter_mut().zip(presented.iter()) {
            *dst = out_scene_sprite(src);
        }
        for (dst, src) in canonical.iter_mut().zip(canonical_list.iter()) {
            *dst = out_scene_sprite(src);
        }
        let colors_len = colors.len().min(self.inner.colors.len());
        colors[..colors_len].copy_from_slice(&self.inner.colors[..colors_len]);
        let trail_len = trail.len().min(self.inner.sprite_trail_seed.len());
        trail[..trail_len].copy_from_slice(&self.inner.sprite_trail_seed[..trail_len]);
        ffi::SceneMeta {
            sprite_count: presented.len() as u32,
            canonical_sprite_count: canonical_list.len() as u32,
            pen_mask: self.inner.pen_mask,
            flipped: self.inner.flipped,
            sprite_flipped: self.inner.sprite_flipped,
            fallback: self.inner.fallback,
            trail_count: trail_len as u32,
        }
    }

    fn export_native(&self, native: &mut [u32]) {
        let len = native.len().min(self.inner.native_pixels.len());
        native[..len].copy_from_slice(&self.inner.native_pixels[..len]);
    }

    fn import_scene(
        &mut self,
        rows: &[ffi::SceneRow],
        sprites: &[ffi::SceneSprite],
        canonical: &[ffi::SceneSprite],
        colors: &[u32],
        trail: &[u16],
        meta: &ffi::SceneMeta,
    ) {
        for (dst, src) in self.inner.rows.iter_mut().zip(rows.iter()) {
            *dst = scene_row(src);
        }
        let sprite_count = (meta.sprite_count as usize).min(sprites.len());
        for (dst, src) in self
            .inner
            .sprites
            .iter_mut()
            .zip(sprites.iter())
            .take(sprite_count)
        {
            *dst = scene_sprite(src);
        }
        self.inner.sprite_count = sprite_count;
        let canonical_count = (meta.canonical_sprite_count as usize).min(canonical.len());
        for (dst, src) in self
            .inner
            .canonical_sprites
            .iter_mut()
            .zip(canonical.iter())
            .take(canonical_count)
        {
            *dst = scene_sprite(src);
        }
        self.inner.canonical_sprite_count = canonical_count;
        let colors_len = colors.len().min(self.inner.colors.len());
        self.inner.colors[..colors_len].copy_from_slice(&colors[..colors_len]);
        let trail_count = (meta.trail_count as usize).min(trail.len());
        self.inner.sprite_trail_seed.clear();
        self.inner
            .sprite_trail_seed
            .extend_from_slice(&trail[..trail_count]);
        self.inner.pen_mask = meta.pen_mask;
        self.inner.flipped = meta.flipped;
        self.inner.sprite_flipped = meta.sprite_flipped;
        self.inner.fallback = meta.fallback;
    }

    fn import_native(&mut self, native: &[u32]) {
        let len = native.len().min(self.inner.native_pixels.len());
        self.inner.native_pixels[..len].copy_from_slice(&native[..len]);
    }
}

#[derive(Default)]
struct EnhancementScratch {
    input: Vec<ffi::SceneSprite>,
    output: Vec<ffi::SceneSprite>,
}

struct HostEnhancements<'a> {
    host: Pin<&'a mut ffi::EnhancementsHost>,
    scratch: &'a mut EnhancementScratch,
}

impl SpriteEnhancements for HostEnhancements<'_> {
    fn present(
        &mut self,
        latched: &[RSceneSprite],
        frame: Option<bool>,
        out: &mut Vec<RSceneSprite>,
    ) {
        let EnhancementScratch { input, output } = self.scratch;
        input.clear();
        input.extend(latched.iter().map(out_scene_sprite));
        if output.is_empty() {
            output.resize_with(v::game::MAX_PRESENTED_SPRITES, || {
                out_scene_sprite(&RSceneSprite::default())
            });
        }
        let count = self
            .host
            .as_mut()
            .present(input, frame.unwrap_or(false), output);
        out.clear();
        out.extend(output[..count].iter().map(scene_sprite));
    }

    fn reset(&mut self) {
        self.host.as_mut().reset();
    }

    fn report(&self, out: &mut String) {
        out.push_str(&self.host.report());
    }
}

pub struct GameVideo {
    inner: RGameVideo,
    enhancements: EnhancementScratch,
}

fn video_stage_index(stage: VideoStage) -> u8 {
    match stage {
        VideoStage::Idle => 0,
        VideoStage::Decode => 1,
        VideoStage::Scene => 2,
        VideoStage::Capture => 3,
        VideoStage::Latch => 4,
    }
}

impl GameVideo {
    fn new_game_video(video: ffi::VideoConfig) -> Box<Self> {
        Box::new(Self {
            inner: RGameVideo::new(&video_config(&video), None),
            enhancements: EnhancementScratch::default(),
        })
    }

    fn reset(&mut self, assets: &GraphicsAssets) {
        self.inner.reset(&assets.inner);
    }

    fn reset_with(
        &mut self,
        assets: &GraphicsAssets,
        enhancements: Pin<&mut ffi::EnhancementsHost>,
    ) {
        if enhancements.as_ref().get_ref().empty() {
            self.inner.reset(&assets.inner);
            return;
        }
        let mut adapter = HostEnhancements {
            host: enhancements,
            scratch: &mut self.enhancements,
        };
        self.inner.reset_with(&assets.inner, Some(&mut adapter));
    }

    #[allow(clippy::too_many_arguments)]
    fn render_frame(
        &mut self,
        assets: &GraphicsAssets,
        graphics: &[u8],
        control: &[u8],
        palette: &[u8],
        frame: u64,
        presentation: &SpritePresentation,
        mut probe: Pin<&mut ffi::ProbeHost>,
        enhancements: Pin<&mut ffi::EnhancementsHost>,
        target: &mut SceneSnapshot,
    ) {
        let vram = v::game::VideoRam {
            graphics,
            control,
            frame,
            palette,
        };
        let view = rdecode::SpritePresentation {
            groups: &presentation.groups,
        };
        let mut stage_cb = |stage| probe.as_mut().enter(video_stage_index(stage));
        let mut adapter = (!enhancements.as_ref().get_ref().empty()).then_some(HostEnhancements {
            host: enhancements,
            scratch: &mut self.enhancements,
        });
        self.inner.render_frame_with(
            &vram,
            &assets.inner,
            FrameSpritePresentation {
                decode: view,
                enhancements: adapter
                    .as_mut()
                    .map(|adapter| adapter as &mut dyn SpriteEnhancements),
            },
            Some(&mut stage_cb),
            Some(&mut target.inner),
        );
    }

    fn materialize_native(&mut self, assets: &GraphicsAssets, target: &SceneSnapshot) {
        self.inner
            .materialize_native_from(&assets.inner, Some(&target.inner));
    }

    fn native_pixels(&self) -> &[u32] {
        self.inner.native_pixels()
    }

    fn set_native_pixels(&mut self, pixels: &[u32]) {
        self.inner.set_native_pixels(pixels);
    }

    fn state_size(&self) -> usize {
        self.inner.state_size()
    }

    fn save_state(&mut self, dst: &mut [u8], assets: &GraphicsAssets) -> Result<(), String> {
        self.inner.save_state(dst, &assets.inner)
    }

    fn load_state(
        &mut self,
        src: &[u8],
        enhancements: Pin<&mut ffi::EnhancementsHost>,
        target: &mut SceneSnapshot,
    ) -> Result<(), String> {
        if enhancements.as_ref().get_ref().empty() {
            return self.inner.load_state(src, Some(&mut target.inner));
        }
        let mut adapter = HostEnhancements {
            host: enhancements,
            scratch: &mut self.enhancements,
        };
        self.inner
            .load_state_with(src, Some(&mut target.inner), Some(&mut adapter))
    }

    fn report(&self) -> String {
        let mut out = String::new();
        self.inner.report(&mut out);
        out
    }

    fn row(&self, screen_y: u32) -> ffi::SceneRow {
        out_scene_row(self.inner.lines().row(screen_y as usize))
    }

    fn playfield_pixel(
        &self,
        layer: u32,
        x: i32,
        y: i32,
        flipped: bool,
        assets: &GraphicsAssets,
        alt: bool,
    ) -> ffi::ScenePixel {
        out_scene_pixel(self.inner.tiles().playfield_pixel(
            layer as usize,
            x,
            y,
            flipped,
            assets.inner.playfield_tiles(),
            alt,
        ))
    }

    fn text_pixel(&self, x: i32, y: i32, flipped: bool) -> ffi::ScenePixel {
        out_scene_pixel(self.inner.text().pixel(x, y, flipped, false))
    }

    fn raster_sprites(&self, assets: &GraphicsAssets, plane: &mut [u16]) {
        plane.fill(0);
        let seed = self.inner.sprites().trail_seed();
        let count = seed.len().min(plane.len());
        plane[..count].copy_from_slice(&seed[..count]);
        self.inner
            .sprites()
            .raster(assets.inner.sprite_tiles(), plane, RVOptions::default());
    }

    fn sprite_list(&self, out: &mut [ffi::SceneSprite]) -> usize {
        let sprites = self.inner.sprites().sprites();
        let count = sprites.len().min(out.len());
        for (dst, src) in out.iter_mut().zip(sprites.iter()).take(count) {
            *dst = out_scene_sprite(src);
        }
        count
    }
}

pub struct GpuMotionHistory {
    inner: v::gpu::GpuMotionHistory,
}

impl GpuMotionHistory {
    fn new_motion_history(opts: ffi::GameVideoOptions) -> Box<Self> {
        Box::new(Self {
            inner: v::gpu::GpuMotionHistory::new(options(&opts)),
        })
    }

    fn capture(&mut self, scene: &SceneSnapshot, frame: u64) {
        self.inner.capture(&scene.inner, frame);
    }

    fn apply(&self, scene: &SceneSnapshot, alpha: f32) -> ffi::MotionResult {
        let result = self.inner.apply(&scene.inner, alpha);
        ffi::MotionResult {
            stats: out_motion_stats(&result.stats),
            sprite_x: result.state.sprite_x,
            sprite_y: result.state.sprite_y,
        }
    }
}

fn gpu_geometry(value: &ffi::GpuGeometry) -> v::config::Geometry {
    v::config::Geometry {
        visible_y: value.first_line,
        visible_height: value.height,
    }
}

fn rust_postprocess(value: ffi::Postprocess) -> v::gpu::Postprocess {
    match value.repr {
        1 => v::gpu::Postprocess::Crt,
        2 => v::gpu::Postprocess::User,
        _ => v::gpu::Postprocess::Off,
    }
}

fn abi_postprocess(value: v::gpu::Postprocess) -> ffi::Postprocess {
    ffi::Postprocess {
        repr: match value {
            v::gpu::Postprocess::Off => 0,
            v::gpu::Postprocess::Crt => 1,
            v::gpu::Postprocess::User => 2,
        },
    }
}

pub struct GpuVideo {
    inner: v::gpu::GpuVideo,
}

#[allow(clippy::too_many_arguments)]
unsafe fn new_gpu_video(
    window: usize,
    opts: ffi::GameVideoOptions,
    geometry: ffi::GpuGeometry,
    playfield_assets: &[u8],
    sprite_assets: &[u8],
    linear: bool,
    vsync: bool,
    interpolation: ffi::VideoInterpolation,
    fields: ffi::InterpolationFields,
    rotation: u32,
) -> Result<Box<GpuVideo>, String> {
    let config = v::gpu::GpuVideoConfig {
        options: options(&opts),
        geometry: gpu_geometry(&geometry),
        linear,
        vsync,
        interpolation: video_interpolation(interpolation),
        fields: interpolation_fields(fields),
        rotation,
    };
    // SAFETY: the C++ GPU constructor supplies the SDL window and owns its lifetime.
    let inner = unsafe { v::gpu::GpuVideo::new(window, config, playfield_assets, sprite_assets) }?;
    Ok(Box::new(GpuVideo { inner }))
}

impl GpuVideo {
    fn driver(&self) -> String {
        self.inner.driver_name()
    }

    fn set_scale(&mut self, scale: u32) -> Result<(), String> {
        self.inner.set_scale(scale)
    }

    fn set_scale_mode(&mut self, mode: ffi::VideoScaleMode) {
        self.inner.set_scale_mode(match mode.repr {
            1 => v::config::VideoScaleMode::Auto,
            2 => v::config::VideoScaleMode::AutoInteger,
            _ => v::config::VideoScaleMode::Fixed,
        });
    }

    fn presented_frames(&self) -> u64 {
        self.inner.presented_frames()
    }

    fn device(&self) -> usize {
        self.inner.device()
    }

    fn set_linear(&mut self, linear: bool) {
        self.inner.set_linear(linear);
    }

    fn set_interpolation(
        &mut self,
        mode: ffi::VideoInterpolation,
        fields: ffi::InterpolationFields,
    ) -> Result<(), String> {
        self.inner
            .set_interpolation(video_interpolation(mode), interpolation_fields(fields))
    }

    unsafe fn set_overlay(&mut self, callback: usize, userdata: usize) {
        // SAFETY: the C++ façade passes a typed callback and keeps its userdata live.
        unsafe { self.inner.set_overlay(callback, userdata) };
    }

    fn set_postprocess(
        &mut self,
        preset: ffi::Postprocess,
        user_shader: &str,
    ) -> Result<(), String> {
        self.inner
            .set_postprocess(rust_postprocess(preset), user_shader)
    }

    fn postprocess(&self) -> ffi::Postprocess {
        abi_postprocess(self.inner.postprocess())
    }

    fn render(
        &mut self,
        scene: &SceneSnapshot,
        output: &mut [u32],
        request: &ffi::GpuRenderRequest,
    ) -> Result<(), String> {
        let request = v::gpu::RenderRequest {
            sprites: sprite_list(request.sprites),
            layer_mask: request.layer_mask,
            alpha: request.has_alpha.then_some(request.alpha),
            postprocess: request.postprocess,
        };
        self.inner.render(&scene.inner, output, &request)
    }

    unsafe fn present(
        &mut self,
        scene: &SceneSnapshot,
        request: &ffi::GpuPresentRequest,
    ) -> Result<bool, String> {
        use v::gpu::PresentTiming;
        let timing = match request.timing.repr {
            1 => PresentTiming::Fixed(request.alpha),
            2 => PresentTiming::Live {
                frame_start_ns: request.frame_start_ns,
                period_ns: request.period_ns,
            },
            _ => PresentTiming::Canonical,
        };
        let left_pane = if request.left_pane_handle == 0 {
            v::gpu::GpuSurface::default()
        } else {
            v::gpu::GpuSurface {
                texture: request.left_pane_handle,
                width: request.left_pane_width,
                height: request.left_pane_height,
            }
        };
        let capture_path =
            (!request.capture_path.is_empty()).then_some(request.capture_path.as_str());
        let request = v::gpu::PresentRequest {
            wait: request.wait,
            timing,
            left_pane,
            capture_path,
        };
        // SAFETY: the C++ caller keeps the optional same-device texture alive across the call.
        unsafe { self.inner.present(&scene.inner, &request) }
    }

    fn capture_surface(
        &mut self,
        scene: &SceneSnapshot,
        destination: &mut GpuTexture,
    ) -> Result<(), String> {
        self.inner
            .capture_surface(&scene.inner, &mut destination.inner)
    }

    fn capture_motion(&mut self, scene: &SceneSnapshot, frame: u64) -> Result<(), String> {
        self.inner.capture_motion(&scene.inner, frame)
    }

    fn reset_motion(&mut self) {
        self.inner.reset_motion();
    }

    fn last_motion(&self) -> ffi::MotionInterpolationStats {
        out_motion_stats(self.inner.last_motion())
    }

    fn pace_motion(&mut self) -> bool {
        self.inner.pace_motion()
    }

    fn last_presented(&self) -> bool {
        self.inner.last_presented()
    }

    fn display_hz(&self) -> f64 {
        self.inner.display_hz()
    }

    fn display_callback_hz(&self) -> f64 {
        self.inner.display_callback_hz()
    }

    fn requested_display_hz(&self) -> f64 {
        self.inner.requested_display_hz()
    }

    fn save_surface(&mut self, path: &str) -> Result<(), String> {
        self.inner.save_surface(path)
    }
}

pub struct GpuTexture {
    inner: v::gpu::GpuTexture,
}

fn new_gpu_texture() -> Box<GpuTexture> {
    Box::new(GpuTexture {
        inner: v::gpu::GpuTexture::empty(),
    })
}

impl GpuTexture {
    fn texture_handle(&self) -> usize {
        self.inner.texture_handle()
    }

    fn texture_width(&self) -> u32 {
        self.inner.width()
    }

    fn texture_height(&self) -> u32 {
        self.inner.height()
    }
}

struct GroupsAdapter<'a>(&'a ffi::SpriteGroupsHost);

impl rflicker::SpriteGroups for GroupsAdapter<'_> {
    fn current_root(&self, key: u64) -> rdecode::SpriteGroupSlot {
        let ffi::SpriteRoot { key, x, y, pc } = self.0.current_root(key);
        rdecode::SpriteGroupSlot { key, x, y, pc }
    }
}

impl rfulldetail::FullDetailLookup for GroupsAdapter<'_> {
    fn full_detail_record(
        &self,
        key: u64,
        group_x: i32,
        group_y: i32,
    ) -> Option<f3rt_sprites::commandw::FullDetailRecord> {
        let r = self.0.full_detail_record(key, group_x, group_y);
        r.valid.then_some(f3rt_sprites::commandw::FullDetailRecord {
            flags0: r.flags0,
            color: r.color,
            desc: r.desc,
            projected: f3rt_sprites::commandw::ProjectedBody {
                screen_x: r.screen_x,
                screen_y: r.screen_y,
                zoom_x: r.zoom_x,
                zoom_y: r.zoom_y,
            },
        })
    }
}

pub struct FlickerHold {
    inner: rflicker::FlickerHoldStage,
    sprites: Vec<RSceneSprite>,
}

impl FlickerHold {
    fn new_flicker_hold(
        name: &str,
        group_class: u8,
        anchor_class: u8,
        period: u8,
        self_follow: bool,
    ) -> Box<Self> {
        Box::new(Self {
            inner: rflicker::FlickerHoldStage::new(rflicker::FlickerHoldConfig {
                name: name.to_string(),
                group_class,
                anchor_class,
                period,
                follow: if self_follow {
                    rflicker::FlickerFollow::SelfFollow
                } else {
                    rflicker::FlickerFollow::Anchor
                },
            }),
            sprites: Vec::new(),
        })
    }

    fn present(
        &mut self,
        sprites: &mut Vec<ffi::SceneSprite>,
        groups: &ffi::SpriteGroupsHost,
        game_frame: u16,
        flipped: bool,
    ) {
        self.sprites.clear();
        self.sprites.extend(sprites.iter().map(scene_sprite));
        let frame = rflicker::SpriteFrame {
            groups: &GroupsAdapter(groups),
            game_frame,
            flipped,
        };
        self.inner.present(&mut self.sprites, &frame);
        sprites.clear();
        sprites.extend(self.sprites.iter().map(out_scene_sprite));
    }

    fn reset(&mut self) {
        self.inner.reset();
    }

    fn report(&self) -> String {
        self.inner.report()
    }
}

pub struct CommandwFullDetail {
    inner: rfulldetail::CommandwFullDetailStage,
    sprites: Vec<RSceneSprite>,
}

impl CommandwFullDetail {
    fn new_commandw_full_detail(visible_y: i32, visible_height: i32) -> Box<Self> {
        Box::new(Self {
            inner: rfulldetail::CommandwFullDetailStage::new(visible_y, visible_height),
            sprites: Vec::new(),
        })
    }

    fn present(
        &mut self,
        sprites: &mut Vec<ffi::SceneSprite>,
        groups: &ffi::SpriteGroupsHost,
        rom: &[u8],
        flipped: bool,
    ) {
        self.sprites.clear();
        self.sprites.extend(sprites.iter().map(scene_sprite));
        self.inner
            .present(&mut self.sprites, &GroupsAdapter(groups), rom, flipped);
        sprites.clear();
        sprites.extend(self.sprites.iter().map(out_scene_sprite));
    }

    fn reset(&mut self) {
        self.inner.reset();
    }

    fn report(&self) -> String {
        self.inner.report()
    }

    fn expanded_objects(&self) -> u64 {
        self.inner.stats().expanded_objects
    }
}
