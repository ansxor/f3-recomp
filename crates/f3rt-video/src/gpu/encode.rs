//! GPU scene encoding, mechanically ported from `runtime/renderer/gpu/encode.{hpp,cpp}`.
//!
//! The chain is [`ScenePlan::apply_motion`] -> [`MotionScene::analyze_interpolation`] ->
//! [`PreparedScene::encode_into`]. `encode_into` is the only code that writes GPU scene words,
//! straight into the caller's mapped upload buffer (no intermediate copy).

use crate::config::{GameVideoOptions, Geometry, NATIVE_WIDTH, SpriteList, VideoConfig};
use crate::game::LAYER_COUNT;
use crate::game::clip::scene_order;
use crate::game::scene::{ScenePlayfield, SceneRow, SceneSprite, presented_playfield};
use crate::game::scene_snapshot::SceneSnapshot;
use crate::gpu::interp::{
    InterpolationCoefficients, InterpolationConfig, InterpolationStats, VideoInterpolation,
    analyze_gpu_interpolation_into,
};
use crate::gpu::motion::{
    GpuMotionHistory, MOTION_LAYER_MASK, MotionInterpolationStats, MotionRowState, MotionState,
};
use crate::gpu::scene_layout::*;

pub use crate::game::scene::ALL_LAYERS;

/// Word counts of the GPU scene buffer (layout: [`super::scene_layout`]).
pub const ENCODED_SCENE_WORDS: usize = F3_SCENE_WORD_COUNT;
pub const ENCODED_INTERPOLATION_WORDS: usize = F3_SCENE_INTERP_WORD_COUNT;
/// `ENCODED_NATIVE_WORDS`: `320 * geometry::HEIGHT` words for the canonical/default scanout
/// geometry. Per-game geometry travels in [`Geometry`]; use [`encoded_native_words`].
pub const ENCODED_NATIVE_WORDS: usize = NATIVE_WIDTH as usize * 232;

/// The C++ `ENCODED_NATIVE_WORDS` for an arbitrary scanout geometry.
pub const fn encoded_native_words(geometry: &Geometry) -> usize {
    geometry.native_pixels() as usize
}

/// `GpuUniforms`: shader uniforms, three tightly packed uvec4s (see scene.glsl).
pub type GpuUniforms = SceneUniforms;

/// `GpuPrepScratch`: caller-owned buffers the chain borrows instead of returning by value.
#[derive(Clone, Default)]
pub struct GpuPrepScratch {
    pub motion_state: MotionState,
    pub interp_coefficients: InterpolationCoefficients,
}

/// One encoded present.
pub struct EncodedScene<'a> {
    pub uniforms: GpuUniforms,
    pub upload_bytes: u32,
    pub fallback: bool,
    pub use_interpolation: bool,
    pub has_trails: bool,
    pub blend_weighted: bool,
    pub trail_seed: &'a [u16],
    pub motion_stats: MotionInterpolationStats,
    pub interp_stats: InterpolationStats,
}

/// Stage 1: one snapshot plus how to draw it. Borrows the snapshot; it must outlive the chain.
#[derive(Clone, Copy)]
pub struct ScenePlan<'a> {
    pub scene: &'a SceneSnapshot,
    pub options: GameVideoOptions,
    pub sprite_list: SpriteList,
    pub layer_mask: u32,
}

impl<'a> ScenePlan<'a> {
    /// Temporal geometry at `alpha` (None: canonical). The sampled state is written into
    /// `scratch` and referenced from the result only when `history` pairs with this scene. Only
    /// the presented sprite list is paired by the history; other lists stay canonical.
    pub fn apply_motion<'s>(
        &self,
        history: Option<&GpuMotionHistory>,
        alpha: Option<f32>,
        scratch: &'s mut MotionState,
    ) -> MotionScene<'a, 's> {
        let mut result = MotionScene {
            plan: *self,
            motion: None,
            motion_stats: MotionInterpolationStats::default(),
        };
        let Some(alpha) = alpha else { return result };
        let Some(history) = history else {
            return result;
        };
        if self.sprite_list != SpriteList::Presented || !self.scene.sprite_trail_seed.is_empty() {
            return result;
        }
        result.motion_stats = history.apply_into(self.scene, alpha, scratch);
        if result.motion_stats.paired {
            result.motion = Some(scratch);
        }
        result
    }
}

/// Stage 2: the plan plus motion-effective geometry.
#[derive(Clone, Copy)]
pub struct MotionScene<'a, 's> {
    pub plan: ScenePlan<'a>,
    pub motion: Option<&'s MotionState>,
    pub motion_stats: MotionInterpolationStats,
}

impl<'a, 's> MotionScene<'a, 's> {
    pub fn analyze_interpolation<'c>(
        &self,
        config: InterpolationConfig,
        tile_pen_masks: &[u64],
        scratch: &'c mut InterpolationCoefficients,
    ) -> PreparedScene<'a, 's, 'c> {
        let mut prepared = PreparedScene {
            motion: *self,
            coefficients: None,
            interp_stats: InterpolationStats::default(),
            use_interpolation: false,
        };
        prepared.interp_stats = analyze_gpu_interpolation_into(
            self.plan.scene,
            self.plan.options,
            config.mode,
            config.fields,
            self.motion,
            tile_pen_masks,
            scratch,
        );
        let active = !self.plan.scene.fallback
            && config.mode != VideoInterpolation::Off
            && self.plan.options.scale > 1;
        prepared.use_interpolation = active;
        if active {
            prepared.coefficients = Some(scratch);
        }
        prepared
    }
}

/// Stage 3: an encoded word count plus everything `encode_into` needs.
#[derive(Clone, Copy)]
pub struct PreparedScene<'a, 's, 'c> {
    pub motion: MotionScene<'a, 's>,
    pub coefficients: Option<&'c InterpolationCoefficients>,
    pub interp_stats: InterpolationStats,
    /// Fallback frames never set this, even when `scale > 1 && mode != Off`.
    pub use_interpolation: bool,
}

impl<'a, 's, 'c> PreparedScene<'a, 's, 'c> {
    pub fn required_words(&self) -> usize {
        if self.motion.plan.scene.fallback {
            return encoded_native_words(&self.motion.plan.scene.geometry());
        }
        if self.use_interpolation {
            ENCODED_INTERPOLATION_WORDS
        } else {
            ENCODED_SCENE_WORDS
        }
    }

    pub fn upload_bytes(&self) -> u32 {
        (self.required_words() * std::mem::size_of::<u32>()) as u32
    }

    /// The only code that writes GPU scene words: a one-way encoding of the snapshot into `out`,
    /// typically the mapped upload buffer, with no intermediate copy.
    ///
    /// * fallback frame: copies native_pixels (320 x `geometry::HEIGHT` ARGB) to
    ///   `out[0..ENCODED_NATIVE_WORDS)`.
    /// * otherwise writes the canonical scene (`ENCODED_SCENE_WORDS`), with `motion`'s
    ///   sprite/playfield/text geometry replacing the snapshot values when non-null. Expanded
    ///   output (`options.expanded()`) writes playfield rows through `presented_playfield()`;
    ///   the snapshot rows themselves stay canonical.
    /// * non-null `coefficients` additionally writes the interpolation region
    ///   (`ENCODED_INTERPOLATION_WORDS` in total).
    ///
    /// `options.border` bounds layer clip ranges. Returns an error if `out` is too small.
    pub fn encode_into(
        &self,
        out: &mut [u32],
        pf_tile_count: u32,
        sp_tile_count: u32,
    ) -> Result<EncodedScene<'a>, String> {
        let plan = self.motion.plan;
        let scene = plan.scene;
        let options = plan.options;
        let sprites = scene.sprite_list_for(plan.sprite_list);
        write_words(
            scene,
            options,
            self.motion.motion,
            self.coefficients,
            out,
            plan.sprite_list,
        )?;

        let mut encoded = EncodedScene {
            uniforms: GpuUniforms::default(),
            upload_bytes: self.upload_bytes(),
            fallback: scene.fallback,
            use_interpolation: self.use_interpolation,
            has_trails: !scene.fallback && !scene.sprite_trail_seed.is_empty(),
            blend_weighted: false,
            trail_seed: &[],
            motion_stats: self.motion.motion_stats,
            interp_stats: self.interp_stats,
        };
        encoded.blend_weighted =
            !scene.fallback && !encoded.has_trails && sprites.iter().any(SceneSprite::weighted);
        if encoded.has_trails {
            encoded.trail_seed = &scene.sprite_trail_seed;
        }

        let geometry = scene.geometry();
        let u = &mut encoded.uniforms;
        u.scale = options.scale;
        u.border = options.border;
        u.width = options.width(&geometry);
        u.height = options.height(&geometry);
        u.sprite_count = sprites.len() as u32;
        u.pen_mask = u32::from(scene.pen_mask);
        u.fallback = u32::from(scene.fallback);
        u.layer_mask = plan.layer_mask & ALL_LAYERS;
        u.first_line = geometry.first_line();
        u.native_height = geometry.height();
        u.pf_tile_count = pf_tile_count;
        u.sp_tile_count = sp_tile_count;
        if encoded.has_trails {
            u.layer_mask |= F3_MASK_SPRITE_TRAILS;
        }
        if scene.flipped {
            u.layer_mask |= F3_MASK_SCREEN_FLIPPED;
        }
        if self.motion.motion.is_some() && self.motion.motion_stats.alpha < 1.0 {
            u.layer_mask |= MOTION_LAYER_MASK;
        }
        Ok(encoded)
    }
}

fn layer_id(i: usize) -> crate::game::scene::LayerId {
    use crate::game::scene::LayerId;
    match i {
        0 => LayerId::Pf0,
        1 => LayerId::Pf1,
        2 => LayerId::Pf2,
        3 => LayerId::Pf3,
        4 => LayerId::Sp0,
        5 => LayerId::Sp1,
        6 => LayerId::Sp2,
        7 => LayerId::Sp3,
        _ => LayerId::Text,
    }
}

fn layer_flags(l: &crate::game::scene::SceneLayer) -> u32 {
    u32::from(l.priority)
        | (u32::from(l.blend_mode) << F3_LAYER_MODE_SHIFT)
        | if l.enabled { F3_LAYER_ENABLED } else { 0 }
        | (u32::from(l.blend_select) << F3_LAYER_SELECT_SHIFT)
        | if l.mosaic { F3_LAYER_MOSAIC } else { 0 }
}

/// `presented` (expanded output) routes playfield geometry through `presented_playfield()`:
/// alt-map rows with an exact full-resolution twin sample the main map at twice the step. Motion
/// keeps moving such a row horizontally (the remap is linear in source_x); a row whose vertical
/// phase moved samples map rows the twin was not solved for, so it keeps the canonical
/// alternate map.
#[allow(clippy::too_many_arguments)]
fn encode_row(
    r: &SceneRow,
    text: &crate::game::text::GameText,
    flipped: bool,
    motion: Option<&MotionRowState>,
    presented: bool,
    video: &VideoConfig,
    left: i16,
    right: i16,
    at: &mut [u32],
) {
    let mut pf: [ScenePlayfield; 4] = r.playfields;
    let mut moved_y = [false; 4];
    if let Some(motion) = motion {
        for i in 0..4 {
            pf[i].source_x = motion.source_x[i];
            moved_y[i] = motion.phase[i]
                != (r.playfields[i].source_y as u32 * 256 + u32::from(r.playfields[i].y_fraction))
                    as i32;
        }
    }
    if presented {
        for i in 0..4 {
            if !moved_y[i] {
                pf[i] = presented_playfield(&pf[i], video);
            }
        }
    }
    at[F3_ROW_BACKGROUND] = u32::from(r.background);
    at[F3_ROW_MOSAIC] = u32::from(r.mosaic_period)
        | if r.palette_15bit { F3_ROW_PALETTE15 } else { 0 }
        | if r.blur { F3_ROW_BLUR } else { 0 };
    at[F3_ROW_TEXT_X] = r.text_x as u32;
    at[F3_ROW_TEXT_Y] = r.text_y as u32;
    at[F3_ROW_BITMAP_MAP_ROW] = if r.bitmap {
        text.bitmap_map_row(r.text_y as u32, flipped)
    } else {
        0
    };
    at[F3_ROW_BLEND] = u32::from(r.blend[0])
        | (u32::from(r.blend[1]) << 8)
        | (u32::from(r.blend[2]) << 16)
        | (u32::from(r.blend[3]) << 24);
    let order = scene_order(r);
    for i in 0..LAYER_COUNT {
        at[F3_ROW_ORDER + i] = order[i] as u32;
        let l = r.layer(layer_id(i));
        let la = &mut at[F3_ROW_LAYERS + i * F3_LAYER_STRIDE..];
        la[F3_LAYER_FLAGS] = layer_flags(l);
        if i < 4 && pf[i].alt_map {
            la[F3_LAYER_FLAGS] |= F3_LAYER_ALT_MAP;
        }
        if i < 4 && video.extend {
            la[F3_LAYER_FLAGS] |= F3_LAYER_EXTEND;
        }
        if i < 4 && r.playfields[i].empty_row {
            la[F3_LAYER_FLAGS] &= !F3_LAYER_ENABLED;
        }
        if i == 8 {
            let text_y_adj = if flipped {
                511 - i32::from(r.text_y)
            } else {
                i32::from(r.text_y)
            };
            if r.bitmap {
                la[F3_LAYER_FLAGS] |= F3_LAYER_BITMAP;
            } else if !text.row_used(text_y_adj as u32) {
                la[F3_LAYER_FLAGS] &= !F3_LAYER_ENABLED;
            }
        }
        let clips = crate::game::clip::clip_ranges_for_row(r, l, left, right);
        la[F3_LAYER_CLIP_COUNT] = clips.count as u32;
        for c in 0..clips.count {
            la[F3_LAYER_CLIPS + c * 2] = clips.ranges[c].left as i32 as u32;
            la[F3_LAYER_CLIPS + c * 2 + 1] = clips.ranges[c].right as i32 as u32;
        }
    }
    for i in 0..4 {
        let p = pf[i];
        let pa = &mut at[F3_ROW_PF + i * F3_PF_STRIDE..];
        pa[F3_PF_SOURCE_X] = p.source_x as u32;
        pa[F3_PF_SOURCE_Y] = p.source_y as u32;
        pa[F3_PF_X_STEP] = p.x_step as u32;
        pa[F3_PF_Y_STEP] = p.y_step as u32;
        pa[F3_PF_Y_FRACTION] = u32::from(p.y_fraction);
        pa[F3_PF_PALETTE_ADD] = u32::from(p.palette_add);
        // The snapshot phase is kept bit-exact where motion did not move it.
        if moved_y[i] {
            let phase = motion.unwrap().phase[i] as u32;
            pa[F3_PF_SOURCE_Y] = phase >> 8;
            pa[F3_PF_Y_FRACTION] = phase & 255;
        }
    }
    at[F3_ROW_MOTION_TEXT_X] = motion.map_or(0, |m| m.text_x as u32);
    at[F3_ROW_MOTION_TEXT_Y] = motion.map_or(0, |m| m.text_y as u32);
}

fn encode_coefficients(coefficients: &InterpolationCoefficients, out: &mut [u32]) {
    for y in 0..F3_SCENE_ROW_COUNT {
        for pf in 0..4 {
            let c = &coefficients.rows[y][pf];
            let at = &mut out[y * F3_SCENE_INTERP_ROW_STRIDE + pf * F3_INTERP_PF_STRIDE..];
            at[0] = u32::from(c.flags);
            if c.flags & F3_INTERP_FLAG_PALETTE as u8 != 0 {
                at[0] |= u32::from(c.palette_stride) << F3_INTERP_PALETTE_STRIDE_SHIFT;
            }
            const OFFSETS: [usize; 4] = [
                F3_INTERP_SOURCE,
                F3_INTERP_ZOOM,
                F3_INTERP_VERTICAL,
                F3_INTERP_PALETTE,
            ];
            let triples = [&c.source, &c.zoom, &c.vertical, &c.palette];
            for t in 0..4 {
                for k in 0..3 {
                    at[OFFSETS[t] + k] = triples[t][k].to_bits();
                }
            }
        }
    }
}

fn write_words(
    frame: &SceneSnapshot,
    options: GameVideoOptions,
    motion: Option<&MotionState>,
    coefficients: Option<&InterpolationCoefficients>,
    out: &mut [u32],
    list: SpriteList,
) -> Result<(), String> {
    if frame.fallback {
        let native = encoded_native_words(&frame.geometry());
        if out.len() < native {
            return Err("GPU native upload too small".to_string());
        }
        out[..native].copy_from_slice(&frame.native_pixels[..native]);
        return Ok(());
    }
    let sprites = frame.sprite_list_for(list);
    let required = if coefficients.is_some() {
        ENCODED_INTERPOLATION_WORDS
    } else {
        ENCODED_SCENE_WORDS
    };
    if out.len() < required || sprites.len() > F3_SCENE_SPRITE_COUNT {
        return Err("GPU scene upload too small".to_string());
    }
    let alternate_maps = frame.tiles.map_count() > 4;
    for l in 0..4 {
        // Raw 4-byte video-RAM cell (attributes<<16 | code); the shader decodes it exactly like
        // `GameTiles::RowSampler`. Word 1 of each slot is unused.
        let cells = frame.tiles.cells(l);
        let dst =
            &mut out[F3_SCENE_PF_CELLS + l * F3_SCENE_PF_LAYER_CELLS * F3_SCENE_PF_CELL_STRIDE..];
        if alternate_maps {
            // Only PF2/PF3 have alternates (maps 4/5); word 1 carries them.
            let alternate = l >= 2;
            let alt_cells = frame.tiles.cells(if alternate { l + 2 } else { l });
            for i in 0..cells.len() {
                dst[i * 2] = cells[i];
                dst[i * 2 + 1] = if alternate { alt_cells[i] } else { 0 };
            }
        } else {
            for i in 0..cells.len() {
                dst[i * 2] = cells[i];
                dst[i * 2 + 1] = 0;
            }
        }
    }
    // Raw big-endian text-map words; the shader decodes them like `GameText::pixel`.
    let map = frame.text.map();
    for (i, value) in map.iter().enumerate() {
        out[F3_SCENE_TEXT_CELLS + i] = u32::from(*value);
    }
    // Raw glyph RAM, byte-packed four bytes per word; the shader indexes it as bytes.
    debug_assert_eq!(0x2000, F3_SCENE_GLYPH_WORDS * 4);
    for (i, chunk) in frame.text.glyphs().chunks_exact(4).enumerate() {
        out[F3_SCENE_GLYPHS + i] = u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
    }
    let pivot = frame.text.pivot();
    debug_assert_eq!(pivot.len(), F3_SCENE_PIVOT_WORDS * 4);
    for (i, chunk) in pivot.chunks_exact(4).enumerate() {
        out[F3_SCENE_PIVOT + i] = u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
    }
    out[F3_SCENE_PALETTE..F3_SCENE_PALETTE + F3_SCENE_PALETTE_WORDS].copy_from_slice(&frame.colors);
    let left = (46 - options.border as i32) as i16;
    let right = (366 + options.border as i32) as i16;
    for y in 0..F3_SCENE_ROW_COUNT {
        let row_at = &mut out[F3_SCENE_ROWS + y * F3_SCENE_ROW_STRIDE..][..F3_SCENE_ROW_STRIDE];
        encode_row(
            &frame.rows[y],
            &frame.text,
            frame.flipped,
            motion.map(|m| &m.rows[y]),
            options.expanded(),
            frame.video(),
            left,
            right,
            row_at,
        );
    }
    for i in 0..sprites.len() {
        let s = &sprites[i];
        let at = &mut out[F3_SCENE_SPRITES + i * F3_SCENE_SPRITE_STRIDE..];
        at[F3_SPRITE_X] = motion.map_or(s.x as u32, |m| m.sprite_x[i] as u32);
        at[F3_SPRITE_Y] = motion.map_or(s.y as u32, |m| m.sprite_y[i] as u32);
        at[F3_SPRITE_SCALE_X] = u32::from(s.scale_x);
        at[F3_SPRITE_SCALE_Y] = u32::from(s.scale_y);
        at[F3_SPRITE_TILE] = s.tile;
        at[F3_SPRITE_PALETTE] = u32::from(s.palette);
        at[F3_SPRITE_WEIGHT] = 0;
        if s.weighted() {
            let weight = s.blend_weight.max(0.0);
            let fixed = (weight * F3_SPRITE_WEIGHT_SCALE as f32 + 0.5) as u32;
            at[F3_SPRITE_WEIGHT] = fixed.clamp(1, F3_SPRITE_WEIGHT_SCALE - 1);
        }
        at[F3_SPRITE_FLIP] = (if s.flip_x { F3_SPRITE_FLIP_X } else { 0 })
            | (if s.flip_y { F3_SPRITE_FLIP_Y } else { 0 })
            | if frame.sprite_flipped {
                F3_SPRITE_SCREEN_FLIPPED
            } else {
                0
            };
    }
    if let Some(coefficients) = coefficients {
        encode_coefficients(coefficients, &mut out[F3_SCENE_INTERP..]);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::scene::{SceneClip, SceneSprite};
    use crate::gpu::interp::{
        InterpolationFields, InterpolationReason, analyze_gpu_interpolation,
        interpolation_reason_name,
    };
    use crate::gpu::motion::GpuMotionHistory;

    fn drawable_scene() -> Box<SceneSnapshot> {
        let mut scene = Box::new(SceneSnapshot::new(&crate::config::VideoConfig::default()));
        scene.fallback = false;
        scene
    }

    fn sprite(scene: &mut SceneSnapshot, slot: usize, x: i32, tile: u32) {
        scene.sprites[slot] = SceneSprite::default();
        scene.sprites[slot].y = 20 * 256;
        scene.sprites[slot].x = x * 256;
        scene.sprites[slot].tile = tile;
        scene.sprite_count = scene.sprite_count.max(slot + 1);
    }

    fn scaled(scale: u32, border: u32) -> GameVideoOptions {
        GameVideoOptions { scale, border }
    }

    struct OwnedEncoded {
        uniforms: SceneUniforms,
        upload_bytes: u32,
        fallback: bool,
        use_interpolation: bool,
        has_trails: bool,
        blend_weighted: bool,
        trail_seed: Vec<u16>,
        motion_stats: MotionInterpolationStats,
        interp_stats: InterpolationStats,
    }

    struct ChainResult {
        use_interpolation: bool,
        has_coefficients: bool,
        motion_is_some: bool,
        encoded: OwnedEncoded,
        words: Vec<u32>,
    }

    #[allow(clippy::too_many_arguments)]
    fn run_chain(
        scene: &SceneSnapshot,
        options: GameVideoOptions,
        list: SpriteList,
        layer_mask: u32,
        history: Option<&GpuMotionHistory>,
        alpha: Option<f32>,
        interpolation: InterpolationConfig,
        scratch: &mut GpuPrepScratch,
        pf_tiles: u32,
        sp_tiles: u32,
    ) -> ChainResult {
        let plan = ScenePlan {
            scene,
            options,
            sprite_list: list,
            layer_mask,
        };
        let motion_scene = plan.apply_motion(history, alpha, &mut scratch.motion_state);
        let motion_is_some = motion_scene.motion.is_some();
        let prepared = motion_scene.analyze_interpolation(
            interpolation,
            &[],
            &mut scratch.interp_coefficients,
        );
        let mut words = vec![0u32; prepared.required_words()];
        let encoded = prepared
            .encode_into(&mut words, pf_tiles, sp_tiles)
            .expect("chain encode");
        ChainResult {
            use_interpolation: prepared.use_interpolation,
            has_coefficients: prepared.coefficients.is_some(),
            motion_is_some,
            encoded: OwnedEncoded {
                uniforms: encoded.uniforms,
                upload_bytes: encoded.upload_bytes,
                fallback: encoded.fallback,
                use_interpolation: encoded.use_interpolation,
                has_trails: encoded.has_trails,
                blend_weighted: encoded.blend_weighted,
                trail_seed: encoded.trail_seed.to_vec(),
                motion_stats: encoded.motion_stats,
                interp_stats: encoded.interp_stats,
            },
            words,
        }
    }

    fn sprite_word(words: &[u32], slot: usize, field: usize) -> u32 {
        words[F3_SCENE_SPRITES + slot * F3_SCENE_SPRITE_STRIDE + field]
    }

    #[test]
    fn layout_constants_match_cpp() {
        assert_eq!(ENCODED_SCENE_WORDS, 172032);
        assert_eq!(ENCODED_INTERPOLATION_WORDS, 185344);
        assert_eq!(ENCODED_NATIVE_WORDS, 320 * 232);
        assert_eq!(
            encoded_native_words(&Geometry::default()),
            ENCODED_NATIVE_WORDS
        );
        assert_eq!(std::mem::size_of::<SceneUniforms>(), 48);
    }

    #[test]
    fn fallback_encodes_only_native_pixels() {
        let mut scene = drawable_scene();
        scene.fallback = true;
        for (i, pixel) in scene.native_pixels.iter_mut().enumerate() {
            *pixel = 0xff00_0000u32 | i as u32;
        }
        let mut scratch = GpuPrepScratch::default();
        let result = run_chain(
            &scene,
            scaled(2, 0),
            SpriteList::Presented,
            ALL_LAYERS,
            None,
            Some(0.5),
            InterpolationConfig {
                mode: VideoInterpolation::Linear,
                fields: InterpolationFields::All,
            },
            &mut scratch,
            0,
            0,
        );
        assert_eq!(result.words.len(), ENCODED_NATIVE_WORDS);
        assert!(result.words.iter().eq(scene.native_pixels.iter()));
        assert!(result.encoded.fallback);
        assert_eq!(result.encoded.uniforms.fallback, 1);
        assert_eq!(
            result.encoded.upload_bytes,
            (ENCODED_NATIVE_WORDS * 4) as u32
        );
        assert!(!result.has_coefficients);
        assert!(!result.use_interpolation);
        assert!(!result.encoded.use_interpolation);
        assert_eq!(
            result.encoded.interp_stats.reason,
            InterpolationReason::Oracle
        );
        assert!(!result.encoded.motion_stats.paired);

        let plan = ScenePlan {
            scene: &scene,
            options: scaled(2, 0),
            sprite_list: SpriteList::Presented,
            layer_mask: ALL_LAYERS,
        };
        let prepared = plan
            .apply_motion(None, Some(0.5), &mut scratch.motion_state)
            .analyze_interpolation(
                InterpolationConfig::default(),
                &[],
                &mut scratch.interp_coefficients,
            );
        let mut too_small = vec![0u32; ENCODED_NATIVE_WORDS - 1];
        assert!(prepared.encode_into(&mut too_small, 0, 0).is_err());
    }

    #[test]
    fn canonical_scene_uniforms_and_words() {
        let mut scene = drawable_scene();
        scene.pen_mask = 63;
        scene.flipped = true;
        scene.colors[3] = 0x0012_3456;
        sprite(&mut scene, 0, 100, 7);
        let mut scratch = GpuPrepScratch::default();

        let options = scaled(1, 8);
        let result = run_chain(
            &scene,
            options,
            SpriteList::Presented,
            ALL_LAYERS,
            None,
            None,
            InterpolationConfig::default(),
            &mut scratch,
            11,
            22,
        );
        assert_eq!(result.words.len(), ENCODED_SCENE_WORDS);
        assert_eq!(
            result.encoded.upload_bytes,
            (ENCODED_SCENE_WORDS * 4) as u32
        );
        assert!(!result.encoded.fallback);
        assert!(!result.encoded.use_interpolation);
        assert!(!result.encoded.has_trails);
        assert!(!result.encoded.blend_weighted);
        let u = &result.encoded.uniforms;
        assert_eq!(u.scale, 1);
        assert_eq!(u.border, 8);
        assert_eq!(u.width, options.width(&scene.geometry()));
        assert_eq!(u.height, options.height(&scene.geometry()));
        assert_eq!(u.first_line, 24);
        assert_eq!(u.native_height, 232);
        assert_eq!(u.sprite_count, 1);
        assert_eq!(u.pen_mask, 63);
        assert_eq!(u.fallback, 0);
        assert_eq!(u.layer_mask, ALL_LAYERS | F3_MASK_SCREEN_FLIPPED);
        assert_eq!(u.pf_tile_count, 11);
        assert_eq!(u.sp_tile_count, 22);
        assert_eq!(
            sprite_word(&result.words, 0, F3_SPRITE_X),
            (100 * 256) as u32
        );
        assert_eq!(sprite_word(&result.words, 0, F3_SPRITE_TILE), 7);
        assert_eq!(result.words[F3_SCENE_PALETTE + 3], 0x0012_3456);

        let masked = run_chain(
            &scene,
            options,
            SpriteList::Presented,
            0x3,
            None,
            None,
            InterpolationConfig::default(),
            &mut scratch,
            0,
            0,
        );
        assert_eq!(
            masked.encoded.uniforms.layer_mask,
            0x3 | F3_MASK_SCREEN_FLIPPED
        );

        let plan = ScenePlan {
            scene: &scene,
            options,
            sprite_list: SpriteList::Presented,
            layer_mask: ALL_LAYERS,
        };
        let prepared = plan
            .apply_motion(None, None, &mut scratch.motion_state)
            .analyze_interpolation(
                InterpolationConfig::default(),
                &[],
                &mut scratch.interp_coefficients,
            );
        let mut too_small = vec![0u32; ENCODED_SCENE_WORDS - 1];
        assert!(prepared.encode_into(&mut too_small, 0, 0).is_err());
    }

    #[test]
    fn sprite_list_choice_trails_and_weights() {
        let mut scene = drawable_scene();
        sprite(&mut scene, 0, 10, 10);
        scene.canonical_sprites[0] = scene.sprites[0];
        scene.canonical_sprites[1] = scene.sprites[0];
        scene.canonical_sprite_count = 2;
        let mut scratch = GpuPrepScratch::default();
        let options = scaled(1, 0);

        let presented = run_chain(
            &scene,
            options,
            SpriteList::Presented,
            ALL_LAYERS,
            None,
            None,
            InterpolationConfig::default(),
            &mut scratch,
            0,
            0,
        );
        assert_eq!(presented.encoded.uniforms.sprite_count, 1);
        let canonical = run_chain(
            &scene,
            options,
            SpriteList::Canonical,
            ALL_LAYERS,
            None,
            None,
            InterpolationConfig::default(),
            &mut scratch,
            0,
            0,
        );
        assert_eq!(canonical.encoded.uniforms.sprite_count, 2);

        scene.sprites[0].blend_weight = 0.5;
        let weighted = run_chain(
            &scene,
            options,
            SpriteList::Presented,
            ALL_LAYERS,
            None,
            None,
            InterpolationConfig::default(),
            &mut scratch,
            0,
            0,
        );
        assert!(weighted.encoded.blend_weighted);
        assert_eq!(
            sprite_word(&weighted.words, 0, F3_SPRITE_WEIGHT),
            F3_SPRITE_WEIGHT_SCALE / 2
        );

        scene.sprite_trail_seed = vec![0x1234; 8];
        let trailed = run_chain(
            &scene,
            options,
            SpriteList::Presented,
            ALL_LAYERS,
            None,
            None,
            InterpolationConfig::default(),
            &mut scratch,
            0,
            0,
        );
        assert!(trailed.encoded.has_trails);
        assert!(
            !trailed.encoded.blend_weighted,
            "trail frames are never blend-weighted"
        );
        assert_ne!(
            trailed.encoded.uniforms.layer_mask & F3_MASK_SPRITE_TRAILS,
            0
        );
        assert_eq!(trailed.encoded.trail_seed, vec![0x1234; 8]);
    }

    #[test]
    fn motion_samples_history_at_alpha() {
        let mut a = drawable_scene();
        sprite(&mut a, 0, 100, 10);
        let mut b = a.clone();
        sprite(&mut b, 0, 110, 10);
        b.canonical_sprites[0] = b.sprites[0];
        b.canonical_sprite_count = 1;
        let mut history = GpuMotionHistory::default();
        history.capture(&a, 10);
        history.capture(&b, 11);
        let mut scratch = GpuPrepScratch::default();
        let options = scaled(1, 0);

        let sampled = ScenePlan {
            scene: &b,
            options,
            sprite_list: SpriteList::Presented,
            layer_mask: ALL_LAYERS,
        }
        .apply_motion(Some(&history), Some(0.5), &mut scratch.motion_state);
        let sampled_stats = sampled.motion_stats;
        let sampled_motion = sampled.motion.is_some();
        assert!(sampled_stats.paired);
        assert!(sampled_motion);
        assert_eq!(scratch.motion_state.sprite_x[0], 105 * 256);
        assert_eq!(sampled_stats.alpha, 0.5);

        let mid = run_chain(
            &b,
            options,
            SpriteList::Presented,
            ALL_LAYERS,
            Some(&history),
            Some(0.5),
            InterpolationConfig::default(),
            &mut scratch,
            0,
            0,
        );
        assert_eq!(sprite_word(&mid.words, 0, F3_SPRITE_X), (105 * 256) as u32);
        assert_ne!(mid.encoded.uniforms.layer_mask & MOTION_LAYER_MASK, 0);
        assert!(mid.encoded.motion_stats.paired);

        let end = run_chain(
            &b,
            options,
            SpriteList::Presented,
            ALL_LAYERS,
            Some(&history),
            Some(1.0),
            InterpolationConfig::default(),
            &mut scratch,
            0,
            0,
        );
        assert_eq!(sprite_word(&end.words, 0, F3_SPRITE_X), (110 * 256) as u32);
        assert_eq!(end.encoded.uniforms.layer_mask & MOTION_LAYER_MASK, 0);

        for (history_opt, alpha, list) in [
            (Some(&history), None, SpriteList::Presented),
            (None, Some(0.5), SpriteList::Presented),
            (Some(&history), Some(0.5), SpriteList::Canonical),
        ] {
            let canonical = run_chain(
                &b,
                options,
                list,
                ALL_LAYERS,
                history_opt,
                alpha,
                InterpolationConfig::default(),
                &mut scratch,
                0,
                0,
            );
            assert!(!canonical.encoded.motion_stats.paired);
            assert!(!canonical.motion_is_some);
            assert_eq!(
                sprite_word(&canonical.words, 0, F3_SPRITE_X),
                (110 * 256) as u32
            );
        }

        b.sprite_trail_seed = vec![1; 8];
        let trailed = run_chain(
            &b,
            options,
            SpriteList::Presented,
            ALL_LAYERS,
            Some(&history),
            Some(0.5),
            InterpolationConfig::default(),
            &mut scratch,
            0,
            0,
        );
        assert!(!trailed.motion_is_some);
        assert_eq!(
            sprite_word(&trailed.words, 0, F3_SPRITE_X),
            (110 * 256) as u32
        );
    }

    fn ramped_scene(delta: i32, offset: i32) -> Box<SceneSnapshot> {
        let mut scene = drawable_scene();
        for y in 24..256 {
            let row = (y - 24) as i32;
            let p = &mut scene.rows[y].playfields[0];
            p.layer.enabled = true;
            p.layer.clip_inverse = true;
            p.source_x = (100 + delta * row + offset) * 256;
            p.source_y = 100 + row;
            p.x_step = 256;
            p.y_step = 256;
        }
        scene
    }

    #[test]
    fn interpolation_modes_and_word_regions() {
        let scene = ramped_scene(2, 0);
        let mut scratch = GpuPrepScratch::default();
        let options = scaled(2, 0);

        let linear = run_chain(
            &scene,
            options,
            SpriteList::Presented,
            ALL_LAYERS,
            None,
            None,
            InterpolationConfig {
                mode: VideoInterpolation::Linear,
                fields: InterpolationFields::Geometry,
            },
            &mut scratch,
            0,
            0,
        );
        assert_eq!(
            linear.encoded.interp_stats.reason,
            InterpolationReason::Applied
        );
        assert!(linear.encoded.interp_stats.layers[0].source_rows > 0);
        assert!(linear.use_interpolation);
        assert!(linear.has_coefficients);
        assert_eq!(linear.words.len(), ENCODED_INTERPOLATION_WORDS);
        assert_eq!(
            linear.encoded.upload_bytes,
            (ENCODED_INTERPOLATION_WORDS * 4) as u32
        );
        let row = 24 + 10;
        assert_ne!(
            linear.words[F3_SCENE_INTERP + row * F3_SCENE_INTERP_ROW_STRIDE]
                & F3_INTERP_FLAG_SOURCE,
            0
        );
        assert_eq!(
            linear.words[F3_SCENE_INTERP + row * F3_SCENE_INTERP_ROW_STRIDE + F3_INTERP_PF_STRIDE],
            0
        );

        let palette_only = run_chain(
            &scene,
            options,
            SpriteList::Presented,
            ALL_LAYERS,
            None,
            None,
            InterpolationConfig {
                mode: VideoInterpolation::Linear,
                fields: InterpolationFields::Palette,
            },
            &mut scratch,
            0,
            0,
        );
        assert_eq!(palette_only.encoded.interp_stats.layers[0].source_rows, 0);

        let off = run_chain(
            &scene,
            options,
            SpriteList::Presented,
            ALL_LAYERS,
            None,
            None,
            InterpolationConfig::default(),
            &mut scratch,
            0,
            0,
        );
        assert_eq!(off.encoded.interp_stats.reason, InterpolationReason::Off);
        assert!(!off.encoded.use_interpolation);
        assert!(!off.has_coefficients);
        assert_eq!(off.words.len(), ENCODED_SCENE_WORDS);

        let native = run_chain(
            &scene,
            scaled(1, 0),
            SpriteList::Presented,
            ALL_LAYERS,
            None,
            None,
            InterpolationConfig {
                mode: VideoInterpolation::Fit,
                fields: InterpolationFields::Geometry,
            },
            &mut scratch,
            0,
            0,
        );
        assert_eq!(
            native.encoded.interp_stats.reason,
            InterpolationReason::NativeScale
        );
        assert!(!native.encoded.use_interpolation);
        assert_eq!(native.words.len(), ENCODED_SCENE_WORDS);

        let plan = ScenePlan {
            scene: &scene,
            options,
            sprite_list: SpriteList::Presented,
            layer_mask: ALL_LAYERS,
        };
        let prepared = plan
            .apply_motion(None, None, &mut scratch.motion_state)
            .analyze_interpolation(
                InterpolationConfig {
                    mode: VideoInterpolation::Linear,
                    fields: InterpolationFields::Geometry,
                },
                &[],
                &mut scratch.interp_coefficients,
            );
        let mut scene_sized = vec![0u32; ENCODED_SCENE_WORDS];
        assert!(prepared.encode_into(&mut scene_sized, 0, 0).is_err());
    }

    #[test]
    fn guard_word_after_required_words_is_untouched() {
        let scene = ramped_scene(2, 0);
        let mut scratch = GpuPrepScratch::default();
        let plan = ScenePlan {
            scene: &scene,
            options: scaled(2, 0),
            sprite_list: SpriteList::Presented,
            layer_mask: ALL_LAYERS,
        };
        let prepared = plan
            .apply_motion(None, None, &mut scratch.motion_state)
            .analyze_interpolation(
                InterpolationConfig {
                    mode: VideoInterpolation::Linear,
                    fields: InterpolationFields::Geometry,
                },
                &[],
                &mut scratch.interp_coefficients,
            );
        let required = prepared.required_words();
        assert_eq!(required, ENCODED_INTERPOLATION_WORDS);
        let mut words = vec![0u32; required + 1];
        let guard = 0xDEAD_BEEF;
        words[required] = guard;
        prepared.encode_into(&mut words, 0, 0).unwrap();
        assert_eq!(words[required], guard);
    }

    #[test]
    fn interpolation_composes_with_motion() {
        let a = ramped_scene(2, 0);
        let b = ramped_scene(2, 4);
        let mut history = GpuMotionHistory::default();
        history.capture(&a, 1);
        history.capture(&b, 2);
        let mut scratch = GpuPrepScratch::default();
        let result = run_chain(
            &b,
            scaled(2, 0),
            SpriteList::Presented,
            ALL_LAYERS,
            Some(&history),
            Some(0.5),
            InterpolationConfig {
                mode: VideoInterpolation::Linear,
                fields: InterpolationFields::Geometry,
            },
            &mut scratch,
            0,
            0,
        );
        assert!(result.encoded.motion_stats.paired);
        assert!(result.encoded.motion_stats.playfield_rows > 0);
        assert_eq!(
            result.encoded.interp_stats.reason,
            InterpolationReason::Applied
        );
        assert_eq!(result.words.len(), ENCODED_INTERPOLATION_WORDS);
        let y = 24;
        assert_eq!(
            result.words[F3_SCENE_ROWS + y * F3_SCENE_ROW_STRIDE + F3_ROW_PF + F3_PF_SOURCE_X],
            (102 * 256) as u32
        );
    }

    #[test]
    fn dirty_gpu_prep_scratch_matches_fresh() {
        let scene = ramped_scene(2, 0);
        let mut fresh = GpuPrepScratch::default();
        let a = run_chain(
            &scene,
            scaled(2, 0),
            SpriteList::Presented,
            ALL_LAYERS,
            None,
            None,
            InterpolationConfig {
                mode: VideoInterpolation::Linear,
                fields: InterpolationFields::Geometry,
            },
            &mut fresh,
            0,
            0,
        );

        let mut dirty = GpuPrepScratch::default();
        dirty.motion_state.sprite_x[0] = 12345;
        dirty.motion_state.rows[40].source_x[0] = -99;
        dirty.interp_coefficients.rows[40][0].flags = 0xff;
        dirty.interp_coefficients.rows[40][0].source = [7.0, 8.0, 9.0];
        let b = run_chain(
            &scene,
            scaled(2, 0),
            SpriteList::Presented,
            ALL_LAYERS,
            None,
            None,
            InterpolationConfig {
                mode: VideoInterpolation::Linear,
                fields: InterpolationFields::Geometry,
            },
            &mut dirty,
            0,
            0,
        );
        assert_eq!(a.words, b.words);
        assert_eq!(a.encoded.interp_stats, b.encoded.interp_stats);
    }

    #[test]
    fn expanded_output_remaps_solved_alt_rows() {
        let video = crate::config::VideoConfig {
            extend: true,
            extended_alt_maps: true,
            full_resolution_alt_maps: true,
            ..crate::config::VideoConfig::default()
        };
        let mut scene = Box::new(SceneSnapshot::new(&video));
        scene.fallback = false;
        scene.rows[24].playfields[0].layer.enabled = true;
        scene.rows[24].playfields[0].alt_map = true;
        scene.rows[24].playfields[0].full_res_offset = 5;
        scene.rows[24].playfields[0].x_step = 128;
        scene.rows[24].playfields[0].source_x = 1000;

        let mut scratch = GpuPrepScratch::default();
        let result = run_chain(
            &scene,
            scaled(2, 0),
            SpriteList::Presented,
            ALL_LAYERS,
            None,
            None,
            InterpolationConfig::default(),
            &mut scratch,
            0,
            0,
        );
        let at = F3_SCENE_ROWS + 24 * F3_SCENE_ROW_STRIDE + F3_ROW_PF;
        assert_eq!(
            result.words[at + F3_PF_SOURCE_X],
            (1000u32 * 2 + (5u32 << 8)) & (1024 * 256 - 1)
        );
        assert_eq!(result.words[at + F3_PF_X_STEP], 256);
        let flags = result.words[F3_SCENE_ROWS + 24 * F3_SCENE_ROW_STRIDE + F3_ROW_LAYERS];
        assert_eq!(flags & F3_LAYER_ALT_MAP, 0, "presented row clears alt_map");
    }

    #[test]
    fn row_and_sprite_field_layout_is_exact() {
        let mut scene = drawable_scene();
        scene.rows[24].background = 0x1234;
        scene.rows[24].mosaic_period = 7;
        scene.rows[24].palette_15bit = true;
        scene.rows[24].blur = true;
        scene.rows[24].text_x = -2;
        scene.rows[24].text_y = 100;
        scene.rows[24].blend = [1, 2, 3, 4];
        scene.rows[24].clips[0] = SceneClip {
            left: 46,
            right: 100,
        };
        scene.rows[24].playfields[0].layer.enabled = true;
        scene.rows[24].playfields[0].layer.clip_enabled = 1;
        scene.rows[24].playfields[0].layer.clip_inverted = 1;
        scene.rows[24].playfields[0].layer.clip_inverse = false;
        scene.rows[24].playfields[0].source_x = 5;
        scene.rows[24].playfields[0].source_y = 6;
        scene.rows[24].playfields[0].x_step = 7;
        scene.rows[24].playfields[0].y_step = 8;
        scene.rows[24].playfields[0].y_fraction = 9;
        scene.rows[24].playfields[0].palette_add = 10;
        sprite(&mut scene, 0, 3, 4);

        let mut scratch = GpuPrepScratch::default();
        let result = run_chain(
            &scene,
            scaled(1, 0),
            SpriteList::Presented,
            ALL_LAYERS,
            None,
            None,
            InterpolationConfig::default(),
            &mut scratch,
            0,
            0,
        );
        let at = F3_SCENE_ROWS + 24 * F3_SCENE_ROW_STRIDE;
        assert_eq!(result.words[at + F3_ROW_BACKGROUND], 0x1234);
        assert_eq!(
            result.words[at + F3_ROW_MOSAIC],
            7 | F3_ROW_PALETTE15 | F3_ROW_BLUR
        );
        assert_eq!(result.words[at + F3_ROW_TEXT_X], (-2i32) as u32);
        assert_eq!(
            result.words[at + F3_ROW_BLEND],
            1 | (2 << 8) | (3 << 16) | (4 << 24)
        );
        assert_eq!(result.words[at + F3_ROW_PF + F3_PF_SOURCE_X], 5u32);
        assert_eq!(result.words[at + F3_ROW_PF + F3_PF_SOURCE_Y], 6u32);
        assert_eq!(result.words[at + F3_ROW_PF + F3_PF_X_STEP], 7);
        assert_eq!(result.words[at + F3_ROW_PF + F3_PF_Y_STEP], 8);
        assert_eq!(result.words[at + F3_ROW_PF + F3_PF_Y_FRACTION], 9);
        assert_eq!(result.words[at + F3_ROW_PF + F3_PF_PALETTE_ADD], 10);
        let layer = at + F3_ROW_LAYERS;
        assert_eq!(result.words[layer + F3_LAYER_CLIP_COUNT], 1);
        assert_eq!(result.words[layer + F3_LAYER_CLIPS], 46);
        assert_eq!(result.words[layer + F3_LAYER_CLIPS + 1], 100);
        let sprite_at = F3_SCENE_SPRITES;
        assert_eq!(result.words[sprite_at + F3_SPRITE_X], (3 * 256) as u32);
        assert_eq!(result.words[sprite_at + F3_SPRITE_Y], (20 * 256) as u32);
        assert_eq!(result.words[sprite_at + F3_SPRITE_SCALE_X], 256);
        assert_eq!(result.words[sprite_at + F3_SPRITE_TILE], 4);
        assert_eq!(result.words[sprite_at + F3_SPRITE_WEIGHT], 0);
        assert_eq!(result.words[sprite_at + F3_SPRITE_FLIP], 0);
    }

    #[test]
    fn palette_and_interp_region_offsets_match_cpp() {
        assert_eq!(F3_SCENE_INTERP, 172032);
        assert_eq!(F3_SCENE_INTERP_WORD_COUNT, 185344);
        assert_eq!(F3_SCENE_INTERP_ROW_STRIDE, 52);
        assert_eq!(4 * F3_INTERP_PF_STRIDE, F3_SCENE_INTERP_ROW_STRIDE);
        let analyzed = analyze_gpu_interpolation(
            &ramped_scene(2, 0),
            scaled(2, 0),
            VideoInterpolation::Linear,
            InterpolationFields::Geometry,
            None,
            &[],
        );
        assert_eq!(analyzed.stats.reason, InterpolationReason::Applied);
        assert_eq!(interpolation_reason_name(analyzed.stats.reason), "applied");
    }
}
