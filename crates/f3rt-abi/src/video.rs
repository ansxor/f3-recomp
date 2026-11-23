//! The renderer bridge (`f3rt::rust_video`).
//!
//! Every game-side renderer and GPU-prep implementation lives in `f3rt-video`; this module is the
//! only place the C++ runtime sees them. The C++ side keeps thin façades with the same public API
//! (see `runtime/renderer/game/*.cpp`, `runtime/renderer/gpu/*.cpp`), so callers and the Wave 4
//! SDL3 backend are unchanged.
//!
//! `SceneSnapshot` carries a C++ host mirror (see `renderer/game/scene_snapshot.hpp`); the
//! `export_*`/`import_*` methods move only the live element counts, never whole capacity buffers.

#![allow(clippy::too_many_arguments)]

use f3rt_video as v;
use f3rt_video::config::SpriteList as RVSpriteList;
use f3rt_video::config::VideoConfig as RVVideoConfig;
use f3rt_video::config::{GameVideoOptions as RVOptions, PixelFormat as RVPixelFormat};
use f3rt_video::decode as rdecode;
use f3rt_video::game::flicker_hold as rflicker;
use f3rt_video::game::video::{GameVideo as RGameVideo, SpriteEnhancements, VideoStage};
use f3rt_video::game::{SceneRow as RSceneRow, SceneSprite as RSceneSprite};
use f3rt_video::gpu::encode as rencode;
use f3rt_video::gpu::interp as rinterp;
use f3rt_video::gpu::motion as rmotion;
use std::pin::Pin;

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

    /// `f3rt::GpuUniforms` (`renderer/gpu/scene_layout.h`, `scene.glsl`).
    struct SceneUniforms {
        scale: u32,
        border: u32,
        width: u32,
        height: u32,
        sprite_count: u32,
        pen_mask: u32,
        fallback: u32,
        layer_mask: u32,
        first_line: u32,
        native_height: u32,
        pf_tile_count: u32,
        sp_tile_count: u32,
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

    /// `f3rt::MotionState::Row`.
    struct MotionRowState {
        source_x: [i32; 4],
        phase: [i32; 4],
        text_x: i32,
        text_y: i32,
    }

    /// `f3rt::MotionState`.
    struct MotionState {
        sprite_x: [i32; 4096],
        sprite_y: [i32; 4096],
        rows: [MotionRowState; 256],
    }

    /// `f3rt::MotionResult`.
    struct MotionResult {
        stats: MotionInterpolationStats,
        state: MotionState,
    }

    /// `f3rt::LayerInterpolationStats`.
    struct LayerInterpolationStats {
        reason: InterpolationReason,
        source_rows: u32,
        zoom_rows: u32,
        vertical_rows: u32,
        palette_rows: u32,
        invalid_rows: u32,
        discontinuities: u32,
        unsafe_palette_pairs: u32,
    }

    /// `f3rt::InterpolationStats`. `row_fields` is flattened `[playfield][row]`.
    struct InterpolationStats {
        reason: InterpolationReason,
        layers: [LayerInterpolationStats; 4],
        row_fields: [u8; 1024],
    }

    /// `f3rt::PlayfieldCoefficients`.
    struct PlayfieldCoefficients {
        flags: u8,
        palette_stride: u16,
        source: [f32; 3],
        zoom: [f32; 3],
        vertical: [f32; 3],
        palette: [f32; 3],
    }

    /// `f3rt::InterpolationCoefficients`. `rows` is flattened `[scanline][playfield]`.
    struct InterpolationCoefficients {
        rows: [PlayfieldCoefficients; 1024],
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

    /// One stage of the GPU-prep chain: stats plus whether coefficients must be uploaded.
    struct InterpolationResult {
        stats: InterpolationStats,
        use_interpolation: bool,
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

    /// One encoded present (`f3rt::EncodedScene`, minus the borrowed trail seed).
    struct EncodedSceneMeta {
        uniforms: SceneUniforms,
        upload_bytes: u32,
        fallback: bool,
        use_interpolation: bool,
        has_trails: bool,
        blend_weighted: bool,
        motion_stats: MotionInterpolationStats,
        interp_stats: InterpolationStats,
        trail_seed_len: usize,
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

    enum InterpolationReason {
        Off,
        NativeScale,
        Oracle,
        NoEffect,
        InvalidRows,
        DiscreteBoundary,
        PaletteDiscontinuity,
        Applied,
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
            latched: &[u8],
            flipped: bool,
            out: &mut [u8],
        ) -> usize;
        fn reset(self: Pin<&mut EnhancementsHost>);
        fn report(&self) -> String;
    }

    unsafe extern "C++" {
        type ProbeHost;
        fn enter(self: Pin<&mut ProbeHost>, stage: u8);
    }

    unsafe extern "C++" {
        type SpriteGroupsHost;
        fn current_root(&self, key: u64, x: &mut i32, y: &mut i32, pc: &mut u32) -> u64;
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
        fn push_splice(
            &mut self,
            bank: bool,
            first: u16,
            last: u16,
            real: &[u8],
            replacement: &[u8],
        );
        fn push_splice_group(&mut self, key: u64, x: i32, y: i32, pc: u32);
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
        fn reset(&mut self);
        fn apply_into(
            &self,
            scene: &SceneSnapshot,
            alpha: f32,
            out: &mut MotionState,
        ) -> MotionInterpolationStats;
        fn apply(&self, scene: &SceneSnapshot, alpha: f32) -> MotionResult;
    }

    extern "Rust" {
        type GpuPrepScratch;
        fn new_prep_scratch() -> Box<GpuPrepScratch>;
        fn motion_state(&mut self) -> &mut MotionState;
        fn interp_coefficients(&mut self) -> &mut InterpolationCoefficients;
    }

    extern "Rust" {
        type GpuVideo;
        fn new_gpu_video(
            vtable: usize,
            shim: usize,
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
        fn last_interpolation(&self) -> InterpolationStats;
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
        fn set_overlay(&mut self, enabled: bool);
        fn set_postprocess(&mut self, preset: Postprocess, user_shader: &str) -> Result<()>;
        fn postprocess(&self) -> Postprocess;
        fn render(
            &mut self,
            scene: &SceneSnapshot,
            output: &mut [u32],
            request: &GpuRenderRequest,
        ) -> Result<()>;
        fn present(&mut self, scene: &SceneSnapshot, request: &GpuPresentRequest) -> Result<bool>;
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
        type InterpolationLog;
        fn new_interp_log() -> Box<InterpolationLog>;
        fn reset(&mut self);
        fn log_if_changed(
            &mut self,
            mode: VideoInterpolation,
            fields: InterpolationFields,
            stats: &InterpolationStats,
        );
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
        fn apply_motion(
            scene: &SceneSnapshot,
            options: GameVideoOptions,
            sprite_list: SpriteList,
            layer_mask: u32,
            out: &mut MotionState,
        ) -> MotionInterpolationStats;
        fn apply_motion_history(
            scene: &SceneSnapshot,
            options: GameVideoOptions,
            sprite_list: SpriteList,
            layer_mask: u32,
            history: &GpuMotionHistory,
            alpha: f32,
            out: &mut MotionState,
        ) -> MotionInterpolationStats;
        fn analyze_interpolation(
            scene: &SceneSnapshot,
            options: GameVideoOptions,
            sprite_list: SpriteList,
            layer_mask: u32,
            mode: VideoInterpolation,
            fields: InterpolationFields,
            tile_pen_masks: &[u64],
            out: &mut InterpolationCoefficients,
        ) -> InterpolationResult;
        fn analyze_interpolation_motion(
            scene: &SceneSnapshot,
            options: GameVideoOptions,
            sprite_list: SpriteList,
            layer_mask: u32,
            motion: &MotionState,
            mode: VideoInterpolation,
            fields: InterpolationFields,
            tile_pen_masks: &[u64],
            out: &mut InterpolationCoefficients,
        ) -> InterpolationResult;
        fn encode_prepared(
            scene: &SceneSnapshot,
            options: GameVideoOptions,
            sprite_list: SpriteList,
            layer_mask: u32,
            motion_stats: &MotionInterpolationStats,
            interp_stats: &InterpolationStats,
            use_interpolation: bool,
            out: &mut [u32],
            trail_out: &mut [u16],
            pf_tile_count: u32,
            sp_tile_count: u32,
        ) -> Result<EncodedSceneMeta>;
        fn encode_prepared_motion(
            scene: &SceneSnapshot,
            options: GameVideoOptions,
            sprite_list: SpriteList,
            layer_mask: u32,
            motion_stats: &MotionInterpolationStats,
            interp_stats: &InterpolationStats,
            use_interpolation: bool,
            motion: &MotionState,
            out: &mut [u32],
            trail_out: &mut [u16],
            pf_tile_count: u32,
            sp_tile_count: u32,
        ) -> Result<EncodedSceneMeta>;
        fn encode_prepared_coefficients(
            scene: &SceneSnapshot,
            options: GameVideoOptions,
            sprite_list: SpriteList,
            layer_mask: u32,
            motion_stats: &MotionInterpolationStats,
            interp_stats: &InterpolationStats,
            use_interpolation: bool,
            coefficients: &InterpolationCoefficients,
            out: &mut [u32],
            trail_out: &mut [u16],
            pf_tile_count: u32,
            sp_tile_count: u32,
        ) -> Result<EncodedSceneMeta>;
        fn encode_prepared_motion_coefficients(
            scene: &SceneSnapshot,
            options: GameVideoOptions,
            sprite_list: SpriteList,
            layer_mask: u32,
            motion_stats: &MotionInterpolationStats,
            interp_stats: &InterpolationStats,
            use_interpolation: bool,
            motion: &MotionState,
            coefficients: &InterpolationCoefficients,
            out: &mut [u32],
            trail_out: &mut [u16],
            pf_tile_count: u32,
            sp_tile_count: u32,
        ) -> Result<EncodedSceneMeta>;
        fn required_scene_words(
            scene: &SceneSnapshot,
            options: GameVideoOptions,
            use_interpolation: bool,
        ) -> usize;
        fn analyze_gpu_interpolation(
            scene: &SceneSnapshot,
            options: GameVideoOptions,
            mode: VideoInterpolation,
            fields: InterpolationFields,
            tile_pen_masks: &[u64],
            out: &mut InterpolationCoefficients,
        ) -> InterpolationStats;
        fn analyze_gpu_interpolation_motion(
            scene: &SceneSnapshot,
            options: GameVideoOptions,
            mode: VideoInterpolation,
            fields: InterpolationFields,
            motion: &MotionState,
            tile_pen_masks: &[u64],
            out: &mut InterpolationCoefficients,
        ) -> InterpolationStats;

        fn validate_user_spirv(words: &[u32]) -> Result<()>;
        fn load_user_shader(path: &str, metal: bool) -> Result<LoadedUserShader>;
    }

    struct LoadedUserShader {
        code: Vec<u32>,
        size: usize,
    }
}
// ---------------------------------------------------------------------------
// Enum conversions
// ---------------------------------------------------------------------------

fn video_interpolation(value: ffi::VideoInterpolation) -> rinterp::VideoInterpolation {
    match value.repr {
        1 => rinterp::VideoInterpolation::Linear,
        2 => rinterp::VideoInterpolation::Fit,
        _ => rinterp::VideoInterpolation::Off,
    }
}

fn interpolation_fields(value: ffi::InterpolationFields) -> rinterp::InterpolationFields {
    rinterp::InterpolationFields::from_bits_truncate(value.repr)
}

fn interpolation_reason(value: rinterp::InterpolationReason) -> ffi::InterpolationReason {
    use rinterp::InterpolationReason as R;
    let repr = match value {
        R::Off => 0,
        R::NativeScale => 1,
        R::Oracle => 2,
        R::NoEffect => 3,
        R::InvalidRows => 4,
        R::DiscreteBoundary => 5,
        R::PaletteDiscontinuity => 6,
        R::Applied => 7,
    };
    ffi::InterpolationReason { repr }
}

fn layer_reason(repr: u8) -> rinterp::InterpolationReason {
    use rinterp::InterpolationReason as R;
    match repr {
        0 => R::Off,
        1 => R::NativeScale,
        2 => R::Oracle,
        4 => R::InvalidRows,
        5 => R::DiscreteBoundary,
        6 => R::PaletteDiscontinuity,
        7 => R::Applied,
        _ => R::NoEffect,
    }
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

// ---------------------------------------------------------------------------
// Struct conversions (bridge -> renderer)
// ---------------------------------------------------------------------------

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
    let mut row = RSceneRow::default();
    for i in 0..4 {
        row.playfields[i] = scene_playfield(&value.playfields[i]);
        row.sprites[i] = scene_layer(&value.sprites[i]);
        row.clips[i] = v::game::scene::SceneClip {
            left: value.clips[i].left,
            right: value.clips[i].right,
        };
        row.blend[i] = value.blend[i];
    }
    row.text = scene_layer(&value.text);
    row.background = value.background;
    row.text_x = value.text_x;
    row.text_y = value.text_y;
    row.mosaic_period = value.mosaic_period;
    row.bitmap = value.bitmap;
    row.palette_15bit = value.palette_15bit;
    row.blur = value.blur;
    row
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

// ---------------------------------------------------------------------------
// Struct conversions (renderer -> bridge)
// ---------------------------------------------------------------------------

fn default_layer() -> ffi::SceneLayer {
    ffi::SceneLayer {
        priority: 0,
        blend_mode: 0,
        clip_enabled: 0,
        clip_inverted: 0,
        clip_inverse: false,
        enabled: false,
        blend_select: false,
        mosaic: false,
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

fn default_playfield() -> ffi::ScenePlayfield {
    ffi::ScenePlayfield {
        layer: default_layer(),
        source_x: 0,
        source_y: 0,
        x_step: 256,
        y_step: 256,
        y_fraction: 0,
        palette_add: 0,
        alt_map: false,
        empty_row: false,
        full_res_offset: -1,
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
    let mut row = ffi::SceneRow {
        playfields: std::array::from_fn(|_| default_playfield()),
        sprites: std::array::from_fn(|_| default_layer()),
        text: default_layer(),
        clips: std::array::from_fn(|_| ffi::SceneClip { left: 0, right: 0 }),
        blend: [0; 4],
        background: 0,
        text_x: 0,
        text_y: 0,
        mosaic_period: 16,
        bitmap: false,
        palette_15bit: false,
        blur: false,
    };
    for i in 0..4 {
        row.playfields[i] = out_scene_playfield(&value.playfields[i]);
        row.sprites[i] = out_scene_layer(&value.sprites[i]);
        row.clips[i] = ffi::SceneClip {
            left: value.clips[i].left,
            right: value.clips[i].right,
        };
        row.blend[i] = value.blend[i];
    }
    row.text = out_scene_layer(&value.text);
    row.background = value.background;
    row.text_x = value.text_x;
    row.text_y = value.text_y;
    row.mosaic_period = value.mosaic_period;
    row.bitmap = value.bitmap;
    row.palette_15bit = value.palette_15bit;
    row.blur = value.blur;
    row
}

fn default_sprite() -> ffi::SceneSprite {
    ffi::SceneSprite {
        x: 0,
        y: 0,
        scale_x: 256,
        scale_y: 256,
        tile: 0,
        palette: 0,
        flip_x: false,
        flip_y: false,
        group: 0,
        group_x: 0,
        group_y: 0,
        group_pc: 0,
        blend_weight: 1.0,
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

fn out_motion_stats(value: &rmotion::MotionInterpolationStats) -> ffi::MotionInterpolationStats {
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

fn motion_stats(value: &ffi::MotionInterpolationStats) -> rmotion::MotionInterpolationStats {
    rmotion::MotionInterpolationStats {
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

fn default_row_state() -> ffi::MotionRowState {
    ffi::MotionRowState {
        source_x: [0; 4],
        phase: [0; 4],
        text_x: 0,
        text_y: 0,
    }
}

fn out_motion_state(value: &rmotion::MotionState) -> ffi::MotionState {
    let mut out = ffi::MotionState {
        sprite_x: [0; 4096],
        sprite_y: [0; 4096],
        rows: std::array::from_fn(|_| default_row_state()),
    };
    out.sprite_x.copy_from_slice(&value.sprite_x);
    out.sprite_y.copy_from_slice(&value.sprite_y);
    for (dst, src) in out.rows.iter_mut().zip(value.rows.iter()) {
        dst.source_x = src.source_x;
        dst.phase = src.phase;
        dst.text_x = src.text_x;
        dst.text_y = src.text_y;
    }
    out
}

fn motion_state(value: &ffi::MotionState) -> rmotion::MotionState {
    let mut out = rmotion::MotionState::default();
    out.sprite_x.copy_from_slice(&value.sprite_x);
    out.sprite_y.copy_from_slice(&value.sprite_y);
    for (dst, src) in out.rows.iter_mut().zip(value.rows.iter()) {
        dst.source_x = src.source_x;
        dst.phase = src.phase;
        dst.text_x = src.text_x;
        dst.text_y = src.text_y;
    }
    out
}

fn default_layer_stats() -> ffi::LayerInterpolationStats {
    ffi::LayerInterpolationStats {
        reason: ffi::InterpolationReason { repr: 3 },
        source_rows: 0,
        zoom_rows: 0,
        vertical_rows: 0,
        palette_rows: 0,
        invalid_rows: 0,
        discontinuities: 0,
        unsafe_palette_pairs: 0,
    }
}

fn out_interp_stats(value: &rinterp::InterpolationStats) -> ffi::InterpolationStats {
    let mut out = ffi::InterpolationStats {
        reason: interpolation_reason(value.reason),
        layers: std::array::from_fn(|_| default_layer_stats()),
        row_fields: [0; 1024],
    };
    for (dst, src) in out.layers.iter_mut().zip(value.layers.iter()) {
        *dst = ffi::LayerInterpolationStats {
            reason: interpolation_reason(src.reason),
            source_rows: src.source_rows,
            zoom_rows: src.zoom_rows,
            vertical_rows: src.vertical_rows,
            palette_rows: src.palette_rows,
            invalid_rows: src.invalid_rows,
            discontinuities: src.discontinuities,
            unsafe_palette_pairs: src.unsafe_palette_pairs,
        };
    }
    for (pf, rows) in value.row_fields.iter().enumerate() {
        out.row_fields[pf * 256..pf * 256 + 256].copy_from_slice(rows);
    }
    out
}

fn interp_stats(value: &ffi::InterpolationStats) -> rinterp::InterpolationStats {
    let mut out = rinterp::InterpolationStats {
        reason: layer_reason(value.reason.repr),
        ..rinterp::InterpolationStats::default()
    };
    for (dst, src) in out.layers.iter_mut().zip(value.layers.iter()) {
        dst.reason = layer_reason(src.reason.repr);
        dst.source_rows = src.source_rows;
        dst.zoom_rows = src.zoom_rows;
        dst.vertical_rows = src.vertical_rows;
        dst.palette_rows = src.palette_rows;
        dst.invalid_rows = src.invalid_rows;
        dst.discontinuities = src.discontinuities;
        dst.unsafe_palette_pairs = src.unsafe_palette_pairs;
    }
    for (pf, rows) in out.row_fields.iter_mut().enumerate() {
        rows.copy_from_slice(&value.row_fields[pf * 256..pf * 256 + 256]);
    }
    out
}

fn default_coefficients() -> ffi::PlayfieldCoefficients {
    ffi::PlayfieldCoefficients {
        flags: 0,
        palette_stride: 0,
        source: [0.0; 3],
        zoom: [0.0; 3],
        vertical: [0.0; 3],
        palette: [0.0; 3],
    }
}

fn out_coefficients(value: &rinterp::InterpolationCoefficients) -> ffi::InterpolationCoefficients {
    let mut out = ffi::InterpolationCoefficients {
        rows: std::array::from_fn(|_| default_coefficients()),
    };
    for (i, src) in value.rows.iter().flatten().enumerate() {
        out.rows[i] = ffi::PlayfieldCoefficients {
            flags: src.flags,
            palette_stride: src.palette_stride,
            source: src.source,
            zoom: src.zoom,
            vertical: src.vertical,
            palette: src.palette,
        };
    }
    out
}

fn coefficients(value: &ffi::InterpolationCoefficients) -> rinterp::InterpolationCoefficients {
    let mut out = rinterp::InterpolationCoefficients::default();
    for (dst, src) in out.rows.iter_mut().flatten().zip(value.rows.iter()) {
        dst.flags = src.flags;
        dst.palette_stride = src.palette_stride;
        dst.source = src.source;
        dst.zoom = src.zoom;
        dst.vertical = src.vertical;
        dst.palette = src.palette;
    }
    out
}

fn out_uniforms(value: &rencode::GpuUniforms) -> ffi::SceneUniforms {
    ffi::SceneUniforms {
        scale: value.scale,
        border: value.border,
        width: value.width,
        height: value.height,
        sprite_count: value.sprite_count,
        pen_mask: value.pen_mask,
        fallback: value.fallback,
        layer_mask: value.layer_mask,
        first_line: value.first_line,
        native_height: value.native_height,
        pf_tile_count: value.pf_tile_count,
        sp_tile_count: value.sp_tile_count,
    }
}

fn out_meta(value: &rencode::EncodedScene<'_>) -> ffi::EncodedSceneMeta {
    ffi::EncodedSceneMeta {
        uniforms: out_uniforms(&value.uniforms),
        upload_bytes: value.upload_bytes,
        fallback: value.fallback,
        use_interpolation: value.use_interpolation,
        has_trails: value.has_trails,
        blend_weighted: value.blend_weighted,
        motion_stats: out_motion_stats(&value.motion_stats),
        interp_stats: out_interp_stats(&value.interp_stats),
        trail_seed_len: value.trail_seed.len(),
    }
}

// ---------------------------------------------------------------------------
// Opaque type constructors (cxx free-function entry points)
// ---------------------------------------------------------------------------

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

fn new_prep_scratch() -> Box<GpuPrepScratch> {
    GpuPrepScratch::new_prep_scratch()
}

fn new_interp_log() -> Box<InterpolationLog> {
    InterpolationLog::new_interp_log()
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

// ---------------------------------------------------------------------------
// Graphics assets
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Sprite presentation
// ---------------------------------------------------------------------------

struct OwnedSplice {
    bank: bool,
    first: u16,
    last: u16,
    real: Vec<u8>,
    replacement: Vec<u8>,
    groups: Vec<rdecode::SpriteGroupSlot>,
}

pub struct SpritePresentation {
    groups: Vec<rdecode::SpriteGroupSlot>,
    splices: Vec<OwnedSplice>,
}

impl SpritePresentation {
    fn new_sprite_presentation() -> Box<Self> {
        Box::new(Self {
            groups: Vec::new(),
            splices: Vec::new(),
        })
    }

    fn clear(&mut self) {
        self.groups.clear();
        self.splices.clear();
    }

    fn push_group(&mut self, key: u64, x: i32, y: i32, pc: u32) {
        self.groups.push(rdecode::SpriteGroupSlot { key, x, y, pc });
    }

    fn push_splice(&mut self, bank: bool, first: u16, last: u16, real: &[u8], replacement: &[u8]) {
        self.splices.push(OwnedSplice {
            bank,
            first,
            last,
            real: real.to_vec(),
            replacement: replacement.to_vec(),
            groups: Vec::new(),
        });
    }

    fn push_splice_group(&mut self, key: u64, x: i32, y: i32, pc: u32) {
        if let Some(splice) = self.splices.last_mut() {
            splice
                .groups
                .push(rdecode::SpriteGroupSlot { key, x, y, pc });
        }
    }

    fn is_empty(&self) -> bool {
        self.groups.is_empty() && self.splices.is_empty()
    }

    fn view(&self) -> (Vec<rdecode::SpriteSplice<'_>>, bool) {
        let views: Vec<rdecode::SpriteSplice<'_>> = self
            .splices
            .iter()
            .map(|s| rdecode::SpriteSplice {
                bank: s.bank,
                first: s.first,
                last: s.last,
                real: &s.real,
                replacement: &s.replacement,
                groups: &s.groups,
            })
            .collect();
        (views, self.is_empty())
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
    let (views, empty) = presentation.view();
    let view = rdecode::SpritePresentation {
        groups: &presentation.groups,
        splices: &views,
    };
    let count = rdecode::decode_sprite_list(
        spriteram,
        visible_y,
        visible_height,
        &mut decoded,
        &mut ram,
        apply_flip,
        if empty { None } else { Some(&view) },
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

// ---------------------------------------------------------------------------
// Scene snapshot
// ---------------------------------------------------------------------------

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
            output,
            stride,
            pixel_format(format),
            border,
            &tile_rows,
            assets.inner.playfield_tiles(),
            assets.inner.sprite_tiles(),
            &mut scratch,
            sprite_list(list),
            None,
            None,
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

// ---------------------------------------------------------------------------
// Game video
// ---------------------------------------------------------------------------

struct HostEnhancements<'a> {
    host: &'a mut ffi::EnhancementsHost,
    input: Vec<ffi::SceneSprite>,
    output: Vec<ffi::SceneSprite>,
}

fn pin_host(host: &mut ffi::EnhancementsHost) -> Pin<&mut ffi::EnhancementsHost> {
    // SAFETY: the host outlives the call and is never moved while borrowed.
    unsafe { Pin::new_unchecked(host) }
}

fn pin_probe(probe: &mut ffi::ProbeHost) -> Pin<&mut ffi::ProbeHost> {
    // SAFETY: the probe outlives the call and is never moved while borrowed.
    unsafe { Pin::new_unchecked(probe) }
}

impl<'a> HostEnhancements<'a> {
    fn new(host: &'a mut ffi::EnhancementsHost) -> Self {
        Self {
            host,
            input: Vec::with_capacity(v::game::MAX_PRESENTED_SPRITES),
            output: (0..v::game::MAX_PRESENTED_SPRITES)
                .map(|_| default_sprite())
                .collect(),
        }
    }
}

/// The bridge's shared `SceneSprite` is `#[repr(C)]` and sized exactly like the C++ mirror
/// (`static_assert` in `scene_snapshot.cpp`), so the host can read/write the slice as bytes.
fn sprites_as_bytes(sprites: &[ffi::SceneSprite]) -> &[u8] {
    // SAFETY: `SceneSprite` is `#[repr(C)]` and contains only integers/floats/bools.
    unsafe {
        std::slice::from_raw_parts(
            sprites.as_ptr().cast::<u8>(),
            std::mem::size_of_val(sprites),
        )
    }
}

fn sprites_as_bytes_mut(sprites: &mut [ffi::SceneSprite]) -> &mut [u8] {
    // SAFETY: see `sprites_as_bytes`.
    unsafe {
        std::slice::from_raw_parts_mut(
            sprites.as_mut_ptr().cast::<u8>(),
            std::mem::size_of_val(sprites),
        )
    }
}

impl SpriteEnhancements for HostEnhancements<'_> {
    fn present(
        &mut self,
        latched: &[RSceneSprite],
        frame: Option<bool>,
        out: &mut Vec<RSceneSprite>,
    ) {
        self.input.clear();
        self.input.extend(latched.iter().map(out_scene_sprite));
        let count = {
            let input = sprites_as_bytes(&self.input);
            let output = sprites_as_bytes_mut(&mut self.output);
            pin_host(self.host).present(input, frame.unwrap_or(false), output)
        };
        out.clear();
        out.extend(
            self.output[..count.min(self.output.len())]
                .iter()
                .map(scene_sprite),
        );
    }

    fn reset(&mut self) {
        pin_host(self.host).reset();
    }

    fn report(&self, out: &mut String) {
        out.push_str(&self.host.report());
    }
}

pub struct GameVideo {
    inner: RGameVideo,
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
        let host = unsafe { enhancements.get_unchecked_mut() };
        if host.empty() {
            self.inner.reset(&assets.inner);
            return;
        }
        let mut adapter = HostEnhancements::new(host);
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
        let (views, empty) = presentation.view();
        let view = rdecode::SpritePresentation {
            groups: &presentation.groups,
            splices: &views,
        };
        let _ = empty;
        let mut stage_cb = |stage: VideoStage| {
            let probe = unsafe { probe.as_mut().get_unchecked_mut() };
            pin_probe(probe).enter(video_stage_index(stage));
        };
        let host = unsafe { enhancements.get_unchecked_mut() };
        if host.empty() {
            self.inner.render_frame_with(
                &vram,
                &assets.inner,
                view,
                Some(&mut stage_cb),
                Some(&mut target.inner),
                None,
            );
        } else {
            let mut adapter = HostEnhancements::new(host);
            self.inner.render_frame_with(
                &vram,
                &assets.inner,
                view,
                Some(&mut stage_cb),
                Some(&mut target.inner),
                Some(&mut adapter),
            );
        }
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
        let host = unsafe { enhancements.get_unchecked_mut() };
        if host.empty() {
            return self.inner.load_state(src, Some(&mut target.inner));
        }
        let mut adapter = HostEnhancements::new(host);
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

// ---------------------------------------------------------------------------
// GPU prep
// ---------------------------------------------------------------------------

pub struct GpuMotionHistory {
    inner: rmotion::GpuMotionHistory,
}

impl GpuMotionHistory {
    fn new_motion_history(opts: ffi::GameVideoOptions) -> Box<Self> {
        Box::new(Self {
            inner: rmotion::GpuMotionHistory::new(options(&opts)),
        })
    }

    fn capture(&mut self, scene: &SceneSnapshot, frame: u64) {
        self.inner.capture(&scene.inner, frame);
    }

    fn reset(&mut self) {
        self.inner.reset();
    }

    fn apply_into(
        &self,
        scene: &SceneSnapshot,
        alpha: f32,
        out: &mut ffi::MotionState,
    ) -> ffi::MotionInterpolationStats {
        let mut state = rmotion::MotionState::default();
        let stats = self.inner.apply_into(&scene.inner, alpha, &mut state);
        *out = out_motion_state(&state);
        out_motion_stats(&stats)
    }

    fn apply(&self, scene: &SceneSnapshot, alpha: f32) -> ffi::MotionResult {
        let result = self.inner.apply(&scene.inner, alpha);
        ffi::MotionResult {
            stats: out_motion_stats(&result.stats),
            state: out_motion_state(&result.state),
        }
    }
}

pub struct GpuPrepScratch {
    motion_state: ffi::MotionState,
    interp_coefficients: ffi::InterpolationCoefficients,
}

impl GpuPrepScratch {
    fn new_prep_scratch() -> Box<Self> {
        Box::new(Self {
            motion_state: ffi::MotionState {
                sprite_x: [0; 4096],
                sprite_y: [0; 4096],
                rows: std::array::from_fn(|_| default_row_state()),
            },
            interp_coefficients: ffi::InterpolationCoefficients {
                rows: std::array::from_fn(|_| default_coefficients()),
            },
        })
    }

    fn motion_state(&mut self) -> &mut ffi::MotionState {
        &mut self.motion_state
    }

    fn interp_coefficients(&mut self) -> &mut ffi::InterpolationCoefficients {
        &mut self.interp_coefficients
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

// ---------------------------------------------------------------------------
// GPU video (SDL3 backend behind the C++ driver shim)
// ---------------------------------------------------------------------------

pub struct GpuVideo {
    inner: v::gpu::GpuVideo,
}

#[allow(clippy::too_many_arguments)]
fn new_gpu_video(
    vtable: usize,
    shim: usize,
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
    // SAFETY: `f3rt-gpu` passes its static driver table and keeps the shim alive for the video's
    // whole lifetime (the C++ façade destroys the Rust box first).
    let driver: v::gpu::DriverRef = std::rc::Rc::new(unsafe {
        v::gpu::SdlDriver::from_raw(
            vtable as *const v::gpu::GpuDriverVtable,
            shim as *mut std::ffi::c_void,
        )
    });
    let window = (window != 0).then_some(v::gpu::RawWindow(window as *mut std::ffi::c_void));
    let inner = v::gpu::GpuVideo::new(
        driver,
        window,
        options(&opts),
        gpu_geometry(&geometry),
        playfield_assets,
        sprite_assets,
        linear,
        vsync,
        video_interpolation(interpolation),
        interpolation_fields(fields),
        rotation,
    )?;
    Ok(Box::new(GpuVideo { inner }))
}

impl GpuVideo {
    fn driver(&self) -> String {
        self.inner.driver_name()
    }

    fn last_interpolation(&self) -> ffi::InterpolationStats {
        out_interp_stats(self.inner.last_interpolation())
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
        self.inner.device().0 as usize
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

    fn set_overlay(&mut self, enabled: bool) {
        self.inner.set_overlay(enabled);
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

    fn present(
        &mut self,
        scene: &SceneSnapshot,
        request: &ffi::GpuPresentRequest,
    ) -> Result<bool, String> {
        use v::gpu::driver::RawTexture;
        use v::gpu::sdl_gpu::PresentTiming;
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
                texture: RawTexture(request.left_pane_handle as *mut std::ffi::c_void),
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
        self.inner.present(&scene.inner, &request)
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
        self.inner.get().0 as usize
    }

    fn texture_width(&self) -> u32 {
        self.inner.width()
    }

    fn texture_height(&self) -> u32 {
        self.inner.height()
    }
}

pub struct InterpolationLog {
    inner: rinterp::InterpolationLog,
}

impl InterpolationLog {
    fn new_interp_log() -> Box<Self> {
        Box::new(Self {
            inner: rinterp::InterpolationLog::default(),
        })
    }

    fn reset(&mut self) {
        self.inner.reset();
    }

    fn log_if_changed(
        &mut self,
        mode: ffi::VideoInterpolation,
        fields: ffi::InterpolationFields,
        stats: &ffi::InterpolationStats,
    ) {
        let config = rinterp::InterpolationConfig {
            mode: video_interpolation(mode),
            fields: interpolation_fields(fields),
        };
        self.inner.observe(config, &interp_stats(stats));
    }
}

fn scene_plan<'a>(
    scene: &'a SceneSnapshot,
    opts: ffi::GameVideoOptions,
    list: ffi::SpriteList,
    layer_mask: u32,
) -> rencode::ScenePlan<'a> {
    rencode::ScenePlan {
        scene: &scene.inner,
        options: options(&opts),
        sprite_list: sprite_list(list),
        layer_mask,
    }
}

fn apply_motion(
    scene: &SceneSnapshot,
    opts: ffi::GameVideoOptions,
    list: ffi::SpriteList,
    layer_mask: u32,
    out: &mut ffi::MotionState,
) -> ffi::MotionInterpolationStats {
    let plan = scene_plan(scene, opts, list, layer_mask);
    let mut motion = motion_state(out);
    let stats = plan.apply_motion(None, None, &mut motion).motion_stats;
    *out = out_motion_state(&motion);
    out_motion_stats(&stats)
}

#[allow(clippy::too_many_arguments)]
fn apply_motion_history(
    scene: &SceneSnapshot,
    opts: ffi::GameVideoOptions,
    list: ffi::SpriteList,
    layer_mask: u32,
    history: &GpuMotionHistory,
    alpha: f32,
    out: &mut ffi::MotionState,
) -> ffi::MotionInterpolationStats {
    let plan = scene_plan(scene, opts, list, layer_mask);
    let mut motion = motion_state(out);
    let stats = plan
        .apply_motion(Some(&history.inner), Some(alpha), &mut motion)
        .motion_stats;
    *out = out_motion_state(&motion);
    out_motion_stats(&stats)
}

fn analyze_impl(
    plan: rencode::ScenePlan<'_>,
    motion: Option<&rmotion::MotionState>,
    mode: ffi::VideoInterpolation,
    fields: ffi::InterpolationFields,
    tile_pen_masks: &[u64],
    out: &mut ffi::InterpolationCoefficients,
) -> ffi::InterpolationResult {
    let motion_scene = rencode::MotionScene {
        plan,
        motion,
        motion_stats: rmotion::MotionInterpolationStats::default(),
    };
    let config = rinterp::InterpolationConfig {
        mode: video_interpolation(mode),
        fields: interpolation_fields(fields),
    };
    let mut coefficients = coefficients(out);
    let (stats, use_interpolation) = {
        let prepared =
            motion_scene.analyze_interpolation(config, tile_pen_masks, &mut coefficients);
        (
            out_interp_stats(&prepared.interp_stats),
            prepared.use_interpolation,
        )
    };
    *out = out_coefficients(&coefficients);
    ffi::InterpolationResult {
        stats,
        use_interpolation,
    }
}

#[allow(clippy::too_many_arguments)]
fn analyze_interpolation(
    scene: &SceneSnapshot,
    opts: ffi::GameVideoOptions,
    list: ffi::SpriteList,
    layer_mask: u32,
    mode: ffi::VideoInterpolation,
    fields: ffi::InterpolationFields,
    tile_pen_masks: &[u64],
    out: &mut ffi::InterpolationCoefficients,
) -> ffi::InterpolationResult {
    analyze_impl(
        scene_plan(scene, opts, list, layer_mask),
        None,
        mode,
        fields,
        tile_pen_masks,
        out,
    )
}

#[allow(clippy::too_many_arguments)]
fn analyze_interpolation_motion(
    scene: &SceneSnapshot,
    opts: ffi::GameVideoOptions,
    list: ffi::SpriteList,
    layer_mask: u32,
    motion: &ffi::MotionState,
    mode: ffi::VideoInterpolation,
    fields: ffi::InterpolationFields,
    tile_pen_masks: &[u64],
    out: &mut ffi::InterpolationCoefficients,
) -> ffi::InterpolationResult {
    let motion = motion_state(motion);
    analyze_impl(
        scene_plan(scene, opts, list, layer_mask),
        Some(&motion),
        mode,
        fields,
        tile_pen_masks,
        out,
    )
}

#[allow(clippy::too_many_arguments)]
fn encode_impl(
    scene: &SceneSnapshot,
    opts: ffi::GameVideoOptions,
    list: ffi::SpriteList,
    layer_mask: u32,
    motion_stats_in: &ffi::MotionInterpolationStats,
    interp_stats_in: &ffi::InterpolationStats,
    use_interpolation: bool,
    motion: Option<&rmotion::MotionState>,
    coefficients_in: Option<&rinterp::InterpolationCoefficients>,
    out: &mut [u32],
    trail_out: &mut [u16],
    pf_tile_count: u32,
    sp_tile_count: u32,
) -> Result<ffi::EncodedSceneMeta, String> {
    let prepared = rencode::PreparedScene {
        motion: rencode::MotionScene {
            plan: scene_plan(scene, opts, list, layer_mask),
            motion,
            motion_stats: motion_stats(motion_stats_in),
        },
        coefficients: coefficients_in,
        interp_stats: interp_stats(interp_stats_in),
        use_interpolation,
    };
    let encoded = prepared.encode_into(out, pf_tile_count, sp_tile_count)?;
    let count = encoded.trail_seed.len().min(trail_out.len());
    trail_out[..count].copy_from_slice(&encoded.trail_seed[..count]);
    Ok(out_meta(&encoded))
}

#[allow(clippy::too_many_arguments)]
fn encode_prepared(
    scene: &SceneSnapshot,
    opts: ffi::GameVideoOptions,
    list: ffi::SpriteList,
    layer_mask: u32,
    motion_stats: &ffi::MotionInterpolationStats,
    interp_stats: &ffi::InterpolationStats,
    use_interpolation: bool,
    out: &mut [u32],
    trail_out: &mut [u16],
    pf_tile_count: u32,
    sp_tile_count: u32,
) -> Result<ffi::EncodedSceneMeta, String> {
    encode_impl(
        scene,
        opts,
        list,
        layer_mask,
        motion_stats,
        interp_stats,
        use_interpolation,
        None,
        None,
        out,
        trail_out,
        pf_tile_count,
        sp_tile_count,
    )
}

#[allow(clippy::too_many_arguments)]
fn encode_prepared_motion(
    scene: &SceneSnapshot,
    opts: ffi::GameVideoOptions,
    list: ffi::SpriteList,
    layer_mask: u32,
    motion_stats: &ffi::MotionInterpolationStats,
    interp_stats: &ffi::InterpolationStats,
    use_interpolation: bool,
    motion: &ffi::MotionState,
    out: &mut [u32],
    trail_out: &mut [u16],
    pf_tile_count: u32,
    sp_tile_count: u32,
) -> Result<ffi::EncodedSceneMeta, String> {
    let motion = motion_state(motion);
    encode_impl(
        scene,
        opts,
        list,
        layer_mask,
        motion_stats,
        interp_stats,
        use_interpolation,
        Some(&motion),
        None,
        out,
        trail_out,
        pf_tile_count,
        sp_tile_count,
    )
}

#[allow(clippy::too_many_arguments)]
fn encode_prepared_coefficients(
    scene: &SceneSnapshot,
    opts: ffi::GameVideoOptions,
    list: ffi::SpriteList,
    layer_mask: u32,
    motion_stats: &ffi::MotionInterpolationStats,
    interp_stats: &ffi::InterpolationStats,
    use_interpolation: bool,
    coefficients_in: &ffi::InterpolationCoefficients,
    out: &mut [u32],
    trail_out: &mut [u16],
    pf_tile_count: u32,
    sp_tile_count: u32,
) -> Result<ffi::EncodedSceneMeta, String> {
    let coeffs = coefficients(coefficients_in);
    encode_impl(
        scene,
        opts,
        list,
        layer_mask,
        motion_stats,
        interp_stats,
        use_interpolation,
        None,
        Some(&coeffs),
        out,
        trail_out,
        pf_tile_count,
        sp_tile_count,
    )
}

#[allow(clippy::too_many_arguments)]
fn encode_prepared_motion_coefficients(
    scene: &SceneSnapshot,
    opts: ffi::GameVideoOptions,
    list: ffi::SpriteList,
    layer_mask: u32,
    motion_stats: &ffi::MotionInterpolationStats,
    interp_stats: &ffi::InterpolationStats,
    use_interpolation: bool,
    motion: &ffi::MotionState,
    coefficients_in: &ffi::InterpolationCoefficients,
    out: &mut [u32],
    trail_out: &mut [u16],
    pf_tile_count: u32,
    sp_tile_count: u32,
) -> Result<ffi::EncodedSceneMeta, String> {
    let motion = motion_state(motion);
    let coeffs = coefficients(coefficients_in);
    encode_impl(
        scene,
        opts,
        list,
        layer_mask,
        motion_stats,
        interp_stats,
        use_interpolation,
        Some(&motion),
        Some(&coeffs),
        out,
        trail_out,
        pf_tile_count,
        sp_tile_count,
    )
}

fn required_scene_words(
    scene: &SceneSnapshot,
    _opts: ffi::GameVideoOptions,
    use_interpolation: bool,
) -> usize {
    if scene.inner.fallback {
        return scene.inner.native_pixels.len();
    }
    if use_interpolation {
        rencode::ENCODED_INTERPOLATION_WORDS
    } else {
        rencode::ENCODED_SCENE_WORDS
    }
}

fn analyze_gpu_impl(
    scene: &SceneSnapshot,
    opts: ffi::GameVideoOptions,
    mode: ffi::VideoInterpolation,
    fields: ffi::InterpolationFields,
    motion: Option<&rmotion::MotionState>,
    tile_pen_masks: &[u64],
    out: &mut ffi::InterpolationCoefficients,
) -> ffi::InterpolationStats {
    let config = rinterp::InterpolationConfig {
        mode: video_interpolation(mode),
        fields: interpolation_fields(fields),
    };
    let analysis = rinterp::analyze_gpu_interpolation(
        &scene.inner,
        options(&opts),
        config.mode,
        config.fields,
        motion,
        tile_pen_masks,
    );
    *out = out_coefficients(&analysis.coefficients);
    out_interp_stats(&analysis.stats)
}

#[allow(clippy::too_many_arguments)]
fn analyze_gpu_interpolation(
    scene: &SceneSnapshot,
    opts: ffi::GameVideoOptions,
    mode: ffi::VideoInterpolation,
    fields: ffi::InterpolationFields,
    tile_pen_masks: &[u64],
    out: &mut ffi::InterpolationCoefficients,
) -> ffi::InterpolationStats {
    analyze_gpu_impl(scene, opts, mode, fields, None, tile_pen_masks, out)
}

#[allow(clippy::too_many_arguments)]
fn analyze_gpu_interpolation_motion(
    scene: &SceneSnapshot,
    opts: ffi::GameVideoOptions,
    mode: ffi::VideoInterpolation,
    fields: ffi::InterpolationFields,
    motion: &ffi::MotionState,
    tile_pen_masks: &[u64],
    out: &mut ffi::InterpolationCoefficients,
) -> ffi::InterpolationStats {
    let motion = motion_state(motion);
    analyze_gpu_impl(
        scene,
        opts,
        mode,
        fields,
        Some(&motion),
        tile_pen_masks,
        out,
    )
}
// ---------------------------------------------------------------------------
// Flicker hold
// ---------------------------------------------------------------------------

struct GroupsAdapter<'a>(&'a ffi::SpriteGroupsHost);

impl rflicker::SpriteGroups for GroupsAdapter<'_> {
    fn current_root(&self, key: u64) -> rdecode::SpriteGroupSlot {
        let mut x = 0;
        let mut y = 0;
        let mut pc = 0;
        let key = self.0.current_root(key, &mut x, &mut y, &mut pc);
        rdecode::SpriteGroupSlot { key, x, y, pc }
    }
}

pub struct FlickerHold {
    inner: rflicker::FlickerHoldStage,
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
        })
    }

    fn present(
        &mut self,
        sprites: &mut Vec<ffi::SceneSprite>,
        groups: &ffi::SpriteGroupsHost,
        game_frame: u16,
        flipped: bool,
    ) {
        let mut list: Vec<RSceneSprite> = sprites.iter().map(scene_sprite).collect();
        let frame = rflicker::SpriteFrame {
            groups: &GroupsAdapter(groups),
            game_frame,
            flipped,
        };
        self.inner.present(&mut list, &frame);
        sprites.clear();
        sprites.extend(list.iter().map(out_scene_sprite));
    }

    fn reset(&mut self) {
        self.inner.reset();
    }

    fn report(&self) -> String {
        self.inner.report()
    }
}

// ---------------------------------------------------------------------------
// User SPIR-V
// ---------------------------------------------------------------------------

fn validate_user_spirv(words: &[u32]) -> Result<(), String> {
    v::gpu::validate_user_spirv(words)
}

fn load_user_shader(path: &str, metal: bool) -> Result<ffi::LoadedUserShader, String> {
    let shader = v::gpu::load_user_shader(path, metal)?;
    Ok(ffi::LoadedUserShader {
        code: shader.code,
        size: shader.size,
    })
}
