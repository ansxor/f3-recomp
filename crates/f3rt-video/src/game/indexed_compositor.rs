//! Indexed composition over disjoint scanout chunks.

use std::sync::atomic::{AtomicU32, Ordering};

use crate::config::{
    GameVideoOptions, Geometry, NATIVE_WIDTH, PixelFormat, SpriteList, VideoConfig,
};
use crate::game::clip::{ClipRanges, clip_ranges_for_row, scene_order};
use crate::game::layer_visits::{LayerPass, LayerVisits};
use crate::game::mix::{background_mix, color15, mix};
use crate::game::row_bits::{
    PenRow, clear_columns, extract_columns, for_each_run, glyph_row_pens, nonzero_nibbles,
};
use crate::game::row_workers::{COMPOSE_CHUNK_ROWS, RowWorkers, WorkWindow, compose_chunk_count};
use crate::game::scene::{
    LAYER_COUNT, LayerId, LayerKind, SceneClip, SceneLayer, ScenePixel, ScenePlayfield, SceneRow,
    kind, presented_playfield, sub_index, wrap_tile_index,
};
use crate::game::sprites::{
    SpriteBins, SpriteCoverage, SpriteCoverageRow, SpriteCoverageView, SpriteRaster,
    SpriteRasterOptions,
};
use crate::game::tiles::GameTiles;

const MAX_WIDTH: usize = NATIVE_WIDTH as usize + GameVideoOptions::MAX_BORDER as usize * 2;
const MAX_WORDS: usize = MAX_WIDTH / 32 + 1;

fn floor_divide(value: i32, divisor: i32) -> i32 {
    if value >= 0 {
        value / divisor
    } else {
        -1 - (-1 - value) / divisor
    }
}

fn layer_id(index: usize) -> LayerId {
    match index {
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

#[derive(Clone, Copy, Debug, Default)]
struct LayerPlan {
    resolves: [bool; 2],
    layer: u32,
}

impl LayerPlan {
    fn pass(&self, select: bool) -> LayerPass {
        LayerVisits::pass(self.layer, select)
    }
}

#[derive(Clone, Copy)]
struct ColumnRanges {
    count: usize,
    range: [[i32; 2]; ClipRanges::CAPACITY],
}

impl Default for ColumnRanges {
    fn default() -> Self {
        Self {
            count: 0,
            range: [[0; 2]; ClipRanges::CAPACITY],
        }
    }
}

const SLOT_ZERO: usize = LAYER_COUNT;
const SLOT_BACKGROUND: usize = LAYER_COUNT + 1;

#[derive(Clone, Copy, Debug, Default)]
struct BlendSamples {
    visits: LayerVisits,
    index: [u16; LAYER_COUNT + 2],
}

const FROM_ZERO: u16 = 0;
const FROM_BACKGROUND: u16 = 0xffff;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct BlendResult {
    visits: LayerVisits,
    source: u8,
    destination: u8,
    source_weight: u8,
    destination_weight: u8,
}

struct BlendMemo {
    entries: [BlendResult; 64],
}

impl BlendMemo {
    fn new() -> Self {
        Self {
            entries: [BlendResult::default(); 64],
        }
    }

    fn clear(&mut self) {
        for entry in &mut self.entries {
            entry.visits = LayerVisits::default();
        }
    }

    fn resolve(
        &mut self,
        visits: LayerVisits,
        replay: impl FnOnce() -> BlendResult,
    ) -> BlendResult {
        let mut slot = (visits.hash() >> 26) as usize;
        for _ in 0..4 {
            if self.entries[slot].visits == visits {
                return self.entries[slot];
            }
            if self.entries[slot].visits.empty() {
                let result = replay();
                self.entries[slot] = result;
                return result;
            }
            slot = (slot + 1) & 63;
        }
        replay()
    }
}

fn replay_blend(row: &SceneRow, order: &[LayerId], visits: LayerVisits) -> BlendResult {
    let mut pixel = background_mix(FROM_BACKGROUND);
    for &id in order {
        let layer = row.layer(id);
        let index = id as u32;
        for select in 0..2u32 {
            if !visits.saw(index, select != 0) {
                continue;
            }
            let count = visits.passes(index);
            for _ in 0..count {
                mix(
                    &mut pixel,
                    layer,
                    (index + 1) as u16,
                    select != 0,
                    &row.blend,
                );
            }
        }
    }
    BlendResult {
        visits,
        source: sample_slot(pixel.source),
        destination: sample_slot(pixel.destination),
        source_weight: pixel.source_weight,
        destination_weight: pixel.destination_weight,
    }
}

const fn sample_slot(from: u16) -> u8 {
    if from == FROM_ZERO {
        SLOT_ZERO as u8
    } else if from == FROM_BACKGROUND {
        SLOT_BACKGROUND as u8
    } else {
        (from - 1) as u8
    }
}

struct RowSetup {
    layers: [SceneLayer; LAYER_COUNT],
    clips: [SceneClip; 4],
    active: u32,
    valid: bool,
    order: [LayerId; LAYER_COUNT],
    order_count: usize,
    settle: u32,
    ranges: [ColumnRanges; LAYER_COUNT],
}

impl RowSetup {
    fn new() -> Self {
        Self {
            layers: [SceneLayer::default(); LAYER_COUNT],
            clips: [SceneClip::default(); 4],
            active: 0,
            valid: false,
            order: [LayerId::Pf0; LAYER_COUNT],
            order_count: 0,
            settle: 0,
            ranges: [ColumnRanges::default(); LAYER_COUNT],
        }
    }
}

struct SharedJob<'a> {
    snapshot: &'a SceneSnapshotView<'a>,
    tile_pixels: &'a [u8],
    sprite_tiles: &'a [u8],
    sprites: &'a [crate::game::SceneSprite],
    tile_rows: &'a [TileRows],
    glyph_rows: &'a [u8; 256],
    palette: &'a [u32],
    palette15: &'a [u32],
    stride: usize,
    format: PixelFormat,
    options: GameVideoOptions,
    geometry: &'a Geometry,
    video: &'a VideoConfig,
    stats: &'a IndexedFrameStats,
    bins: &'a SpriteBins,
}

pub struct SceneSnapshotView<'a> {
    pub tiles: &'a GameTiles,
    pub text: &'a crate::game::text::GameText,
    pub rows: &'a [SceneRow; 256],
    pub sprites: &'a [crate::game::SceneSprite],
    pub canonical_sprites: &'a [crate::game::SceneSprite],
    pub colors: &'a [u32; 8192],
    pub pen_mask: u8,
    pub flipped: bool,
    pub sprite_flipped: bool,
    pub sprite_trail_seed: &'a [u16],
    pub fallback: bool,
}

/// Palette conversion cache (`IndexedPalette`).
pub struct IndexedPalette {
    pub colors: [u32; Self::SIZE + Self::GUARD],
    pub colors15: [u32; Self::SIZE + Self::GUARD],
    pub uses_15bit: bool,
    source: [u32; Self::SIZE],
    format: PixelFormat,
    built: bool,
    built_15bit: bool,
}

impl IndexedPalette {
    pub const SIZE: usize = 8192;
    pub const GUARD: usize = 64;
    pub const BLOCK: usize = 64;

    pub fn new() -> Self {
        Self {
            colors: [0; Self::SIZE + Self::GUARD],
            colors15: [0; Self::SIZE + Self::GUARD],
            uses_15bit: false,
            source: [0; Self::SIZE],
            format: PixelFormat::Argb8888,
            built: false,
            built_15bit: false,
        }
    }

    /// Rebuild only changed 64-entry blocks; 15-bit words refresh when requested.
    pub fn build(&mut self, palette: &[u32], format: PixelFormat, need_15bit: bool) {
        assert!(
            palette.len() >= Self::SIZE,
            "palette must hold {} entries",
            Self::SIZE
        );
        let rebuild = !self.built || format != self.format;
        if rebuild {
            self.built_15bit = false;
        }
        let refresh_15bit = need_15bit && !self.built_15bit;
        let mut any_changed = false;
        let mut first_changed = false;
        for first in (0..Self::SIZE).step_by(Self::BLOCK) {
            let changed = rebuild
                || self.source[first..first + Self::BLOCK] != palette[first..first + Self::BLOCK];
            if !changed && !refresh_15bit {
                continue;
            }
            if changed {
                for (i, value) in palette[first..first + Self::BLOCK].iter().enumerate() {
                    self.colors[first + i] =
                        format.to_pixel_format(0xff00_0000 | (value & 0x00ff_ffff));
                }
            }
            if need_15bit {
                for (i, value) in palette[first..first + Self::BLOCK].iter().enumerate() {
                    self.colors15[first + i] =
                        format.to_pixel_format(0xff00_0000 | color15(*value));
                }
            }
            if changed {
                self.source[first..first + Self::BLOCK]
                    .copy_from_slice(&palette[first..first + Self::BLOCK]);
                any_changed = true;
                first_changed |= first == 0;
            }
        }
        if first_changed {
            let (head, tail) = self.colors.split_at_mut(Self::SIZE);
            tail[..Self::GUARD].copy_from_slice(&head[..Self::GUARD]);
        }
        if need_15bit && (first_changed || refresh_15bit) {
            let (head, tail) = self.colors15.split_at_mut(Self::SIZE);
            tail[..Self::GUARD].copy_from_slice(&head[..Self::GUARD]);
        }
        self.format = format;
        self.built = true;
        self.built_15bit = need_15bit || (self.built_15bit && !any_changed);
        self.uses_15bit = need_15bit;
    }
}

impl Default for IndexedPalette {
    fn default() -> Self {
        Self::new()
    }
}

/// Tile row visibility masks (`TileRows`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TileRows {
    pub low: u16,
    pub plane4: u16,
    pub plane5: u16,
}

impl TileRows {
    pub fn visible(&self, y: u32, pen_mask: u32) -> bool {
        let rows = u32::from(self.low)
            | (u32::from(self.plane4) & 0u32.wrapping_sub((pen_mask >> 4) & 1))
            | (u32::from(self.plane5) & 0u32.wrapping_sub((pen_mask >> 5) & 1));
        (rows >> y) & 1 != 0
    }
}

/// Per-tile visible-row masks (`TileRowTable`). Pixels travel separately so the table can cache
/// masks without borrowing the asset ROM for the lifetime of `GameVideo`.
#[derive(Clone, Debug, Default)]
pub struct TileRowTable {
    rows: Vec<TileRows>,
}

impl TileRowTable {
    pub fn new(tile_pixels: &[u8]) -> Self {
        let mut rows = Vec::with_capacity(tile_pixels.len() / 256);
        for tile in 0..tile_pixels.len() / 256 {
            let mut low = 0u32;
            let mut plane4 = 0u32;
            let mut plane5 = 0u32;
            for y in 0..16 {
                let at = tile * 256 + y * 16;
                let row = &tile_pixels[at..at + 16];
                let words = [
                    u32::from_le_bytes([row[0], row[1], row[2], row[3]]),
                    u32::from_le_bytes([row[4], row[5], row[6], row[7]]),
                    u32::from_le_bytes([row[8], row[9], row[10], row[11]]),
                    u32::from_le_bytes([row[12], row[13], row[14], row[15]]),
                ];
                let any = words[0] | words[1] | words[2] | words[3];
                low |= u32::from((any & 0x0f0f_0f0f) != 0) << y;
                plane4 |= u32::from((any & 0x1010_1010) != 0) << y;
                plane5 |= u32::from((any & 0x2020_2020) != 0) << y;
            }
            rows.push(TileRows {
                low: low as u16,
                plane4: plane4 as u16,
                plane5: plane5 as u16,
            });
        }
        Self { rows }
    }

    pub fn rows(&self) -> &[TileRows] {
        &self.rows
    }
}

/// Special-pixel counter for one frame (`IndexedFrameStats`).
#[derive(Debug, Default)]
pub struct IndexedFrameStats {
    special_px: AtomicU32,
}

impl IndexedFrameStats {
    pub fn reset(&self) {
        self.special_px.store(0, Ordering::Relaxed);
    }

    pub fn add(&self, count: u32) {
        self.special_px.fetch_add(count, Ordering::Relaxed);
    }

    pub fn special_px(&self) -> u32 {
        self.special_px.load(Ordering::Relaxed)
    }
}

/// Reusable composition buffers (`IndexedScratch`).
pub struct IndexedScratch {
    pub plane: Vec<u16>,
    pub coverage_layers: Vec<u8>,
    pub coverage_bits: Vec<u32>,
    pub bins: SpriteBins,
    pub palette: IndexedPalette,
    pub glyph_rows: [u8; 256],
    pub stats: IndexedFrameStats,
}

impl IndexedScratch {
    pub fn new() -> Self {
        Self {
            plane: Vec::new(),
            coverage_layers: Vec::new(),
            coverage_bits: Vec::new(),
            bins: SpriteBins::default(),
            palette: IndexedPalette::new(),
            glyph_rows: [0; 256],
            stats: IndexedFrameStats::default(),
        }
    }

    pub fn configure(&mut self, options: GameVideoOptions, geometry: &Geometry) {
        let plane_size = sprite_plane_size(options, geometry);
        if self.plane.len() != plane_size {
            self.plane = vec![0; plane_size];
        }
        let rows = if options.expanded() {
            geometry.height() as usize
        } else {
            256
        };
        if self.coverage_layers.len() != rows {
            self.coverage_layers = vec![0; rows];
        }
        let bits = rows * SpriteCoverage::LAYERS * sprite_coverage_words(options, geometry);
        if self.coverage_bits.len() != bits {
            self.coverage_bits = vec![0; bits];
        }
    }

    /// Rebuild palette/glyph tables; returns whether any row uses the 15-bit palette.
    pub fn prepare(&mut self, snapshot: &SceneSnapshotView<'_>, format: PixelFormat) -> bool {
        let need_15bit = uses_15bit_palette(snapshot);
        self.palette.build(snapshot.colors, format, need_15bit);
        glyph_row_masks(snapshot.text, &mut self.glyph_rows);
        need_15bit
    }
}

impl Default for IndexedScratch {
    fn default() -> Self {
        Self::new()
    }
}

fn sprite_plane_size(options: GameVideoOptions, geometry: &Geometry) -> usize {
    if options.expanded() {
        options.width(geometry) as usize * options.height(geometry) as usize
    } else {
        crate::game::sprites::TRAIL_PLANE_PIXELS
    }
}

fn sprite_coverage_words(options: GameVideoOptions, geometry: &Geometry) -> usize {
    crate::game::sprites::sprite_coverage_words(&options, geometry)
}

fn uses_15bit_palette(snapshot: &SceneSnapshotView<'_>) -> bool {
    let first = snapshot_first_line(snapshot);
    let end = snapshot_end_line(snapshot);
    (first..end).any(|y| snapshot.rows[y & 255].palette_15bit)
}

fn snapshot_first_line(snapshot: &SceneSnapshotView<'_>) -> usize {
    snapshot.tiles.video().visible_y as usize
}

fn snapshot_end_line(snapshot: &SceneSnapshotView<'_>) -> usize {
    snapshot.tiles.video().visible_y as usize + snapshot.tiles.video().visible_height as usize
}

fn glyph_row_masks(text: &crate::game::text::GameText, rows: &mut [u8; 256]) {
    let glyphs = text.glyphs();
    for (glyph, used) in rows.iter_mut().enumerate() {
        let data = &glyphs[glyph * 32..glyph * 32 + 32];
        let mut mask = 0u8;
        for y in 0..8 {
            let word = u32::from_le_bytes([
                data[y * 4],
                data[y * 4 + 1],
                data[y * 4 + 2],
                data[y * 4 + 3],
            ]);
            mask |= u8::from(word != 0) << y;
        }
        *used = mask;
    }
}

struct RowWork<'a> {
    job: &'a SharedJob<'a>,
    row: &'a SceneRow,
    width: usize,
    words: usize,
    left_edge: i32,
    border: i32,
    expanded: bool,
    flipped: bool,
    out: &'a mut [u32],
    lut: &'a [u32],
    samples: &'a mut [BlendSamples; MAX_WIDTH],
    unresolved: [u32; MAX_WORDS],
    pending: [u32; MAX_WORDS],
    any_pending: bool,
    setup: &'a RowSetup,
    plane: &'a [u16],
    coverage: SpriteCoverageRow<'a>,
    memo: BlendMemo,
}

impl<'a> RowWork<'a> {
    fn reset(&mut self) {
        self.words = self.width.div_ceil(32);
        self.unresolved.fill(0);
        for word in self.unresolved.iter_mut().take(self.words) {
            *word = u32::MAX;
        }
        if self.width & 31 != 0 {
            self.unresolved[self.words - 1] &= (1u32 << (self.width & 31)) - 1;
        }
        self.pending.fill(0);
        self.any_pending = false;
    }

    fn plan(&self, layer: &SceneLayer, id: LayerId) -> LayerPlan {
        let mode = (layer.blend_mode == 0 || layer.blend_mode == 3)
            && ((self.setup.settle >> (id as u32)) & 1 != 0);
        LayerPlan {
            resolves: [
                mode && u16::from(self.row.blend[0]) + u16::from(self.row.blend[2]) == 8,
                mode && u16::from(self.row.blend[1]) + u16::from(self.row.blend[3]) == 8,
            ],
            layer: id as u32,
        }
    }

    fn blend_result(&mut self, visits: LayerVisits) -> BlendResult {
        let order = &self.setup.order[..self.setup.order_count];
        self.memo
            .resolve(visits, || replay_blend(self.row, order, visits))
    }
}

fn visit(work: &mut RowWork<'_>, x: i32, palette: u16, select: bool, plan: &LayerPlan) {
    let word = (x as u32 >> 5) as usize;
    let bit = 1u32 << ((x as u32) & 31);
    let pass = plan.pass(select);
    if work.unresolved[word] & bit != 0 {
        work.unresolved[word] &= !bit;
        if plan.resolves[usize::from(select)] {
            work.out[x as usize] = work.lut[(palette as usize) & 8191];
            return;
        }
        if !work.any_pending {
            work.any_pending = true;
            work.memo.clear();
        }
        work.pending[word] |= bit;
        let sample = &mut work.samples[x as usize];
        sample.visits = LayerVisits::new(pass);
        sample.index[SLOT_ZERO] = 0;
        sample.index[SLOT_BACKGROUND] = work.row.background;
    } else if work.pending[word] & bit != 0 {
        work.samples[x as usize].visits.add(pass);
    } else {
        return;
    }
    work.samples[x as usize].index[plan.layer as usize] = palette;
}

fn visit_bits(
    work: &mut RowWork<'_>,
    w: usize,
    bits: u32,
    select: bool,
    plan: &LayerPlan,
    palette_at: impl Fn(u32) -> u16,
) {
    let pass = plan.pass(select);
    let again = bits & work.pending[w];
    let fresh = bits & !again;
    if fresh != 0 {
        if !work.any_pending {
            work.any_pending = true;
            work.memo.clear();
        }
        work.unresolved[w] &= !fresh;
        work.pending[w] |= fresh;
        let mut mask = fresh;
        while mask != 0 {
            let x = (w * 32) as u32 + mask.trailing_zeros();
            mask &= mask - 1;
            let sample = &mut work.samples[x as usize];
            sample.visits = LayerVisits::new(pass);
            sample.index[SLOT_ZERO] = 0;
            sample.index[SLOT_BACKGROUND] = work.row.background;
            sample.index[plan.layer as usize] = palette_at(x);
        }
    }
    let mut mask = again;
    while mask != 0 {
        let x = (w * 32) as u32 + mask.trailing_zeros();
        mask &= mask - 1;
        let sample = &mut work.samples[x as usize];
        sample.visits.add(pass);
        sample.index[plan.layer as usize] = palette_at(x);
    }
}

fn visit_run(
    work: &mut RowWork<'_>,
    x: i32,
    bits: u32,
    select: bool,
    plan: &LayerPlan,
    palette_at: impl Fn(u32) -> u16,
) {
    let word = (x as u32 >> 5) as usize;
    let shift = (x as u32) & 31;
    if bits << shift != 0 {
        visit_bits(work, word, bits << shift, select, plan, &palette_at);
    }
    if shift > 16 && bits >> (32 - shift) != 0 {
        visit_bits(
            work,
            word + 1,
            bits >> (32 - shift),
            select,
            plan,
            &palette_at,
        );
    }
}

fn for_each_range_word(left: i32, right: i32, mut body: impl FnMut(usize, u32)) {
    if left >= right {
        return;
    }
    let first_word = (left as u32) >> 5;
    let last_word = ((right - 1) as u32) >> 5;
    for word in first_word..=last_word {
        let mut window = u32::MAX;
        if word == first_word {
            window &= u32::MAX << ((left as u32) & 31);
        }
        if word == last_word && ((right as u32) & 31) != 0 {
            window &= (1u32 << ((right as u32) & 31)) - 1;
        }
        body(word as usize, window);
    }
}

fn open_columns(work: &RowWork<'_>, x: i32, len: u32) -> u32 {
    let open = extract_columns(&work.unresolved, x, len);
    if work.any_pending {
        open | extract_columns(&work.pending, x, len)
    } else {
        open
    }
}

fn next_open_column(work: &RowWork<'_>, x: i32, right: i32) -> i32 {
    let mut word = (x as u32) >> 5;
    let last = ((right - 1) as u32) >> 5;
    let mut bits = (work.unresolved[word as usize] | work.pending[word as usize])
        & (u32::MAX << ((x as u32) & 31));
    while bits == 0 {
        word += 1;
        if word > last {
            return right;
        }
        bits = work.unresolved[word as usize] | work.pending[word as usize];
    }
    right.min((word * 32 + bits.trailing_zeros()) as i32)
}

fn column_ranges(
    row: &SceneRow,
    layer: &SceneLayer,
    left_edge: i32,
    right_edge: i32,
) -> ColumnRanges {
    let clips = clip_ranges_for_row(row, layer, left_edge as i16, right_edge as i16);
    let mut result = ColumnRanges {
        count: clips.count,
        ..ColumnRanges::default()
    };
    for (r, clip) in clips.iter().enumerate() {
        result.range[r] = [
            i32::from(clip.left) - left_edge,
            i32::from(clip.right) - left_edge,
        ];
    }
    result
}

fn playfield_row(work: &RowWork<'_>, id: LayerId) -> ScenePlayfield {
    let pf = work.row.playfields[sub_index(id)];
    if work.expanded {
        presented_playfield(&pf, work.job.video)
    } else {
        pf
    }
}

fn compose_mosaic_layer(
    work: &mut RowWork<'_>,
    id: LayerId,
    layer: &SceneLayer,
    ranges: &ColumnRanges,
) {
    let plan = work.plan(layer, id);
    let layer_kind = kind(id);
    let mut pf = ScenePlayfield::default();
    let mut sampler = None;
    if layer_kind == LayerKind::Playfield {
        pf = playfield_row(work, id);
        sampler = Some(work.job.snapshot.tiles.row_sampler(
            sub_index(id),
            pf.source_y,
            work.flipped,
            work.job.tile_pixels,
            pf.alt_map,
        ));
    }
    let plane = work.plane;
    let covered = if layer_kind == LayerKind::Sprite {
        Some(work.coverage.columns(sub_index(id)))
    } else {
        None
    };
    let job = work.job;
    for r in 0..ranges.count {
        let (left, right) = (ranges.range[r][0], ranges.range[r][1]);
        let mut mask_holder = 0u32;
        for_each_range_word(left, right, |w, window| {
            mask_holder = (work.unresolved[w] | work.pending[w]) & window;
            while mask_holder != 0 {
                let x = (w * 32 + mask_holder.trailing_zeros() as usize) as i32;
                mask_holder &= mask_holder - 1;
                let hardware_x = work.left_edge + x;
                let phase = hardware_x + 68;
                let count = if phase < 0 {
                    phase + 432
                } else if phase >= 432 {
                    phase - 432
                } else {
                    phase
                };
                let sample_x = hardware_x - count % i32::from(work.row.mosaic_period);
                let mut source = ScenePixel::default();
                let mut select = layer.blend_select;
                match layer_kind {
                    LayerKind::Playfield => {
                        let source_x = floor_divide(pf.source_x + (sample_x - 46) * pf.x_step, 256);
                        source = sampler.as_mut().expect("playfield sampler").pixel(source_x);
                        select = (source.flags & 1) != 0;
                        if source.flags & 0x10 == 0 {
                            continue;
                        }
                        source.palette = source.palette.wrapping_add(pf.palette_add);
                    }
                    LayerKind::Sprite => {
                        let source_x = if work.expanded {
                            sample_x - work.left_edge
                        } else {
                            sample_x
                        };
                        let column = if work.expanded {
                            source_x
                        } else {
                            source_x - work.left_edge
                        };
                        let covered = covered.expect("sprite coverage");
                        if column < 0
                            || column >= work.width as i32
                            || (covered[(column >> 5) as usize] >> (column & 31)) & 1 == 0
                        {
                            continue;
                        }
                        source.palette = plane[source_x as usize];
                    }
                    LayerKind::Text => {
                        let source_x = i32::from(work.row.text_x) + sample_x - 46;
                        source = job.snapshot.text.pixel(
                            source_x,
                            i32::from(work.row.text_y),
                            work.flipped,
                            work.row.bitmap,
                        );
                        if source.flags & 0x10 == 0 {
                            continue;
                        }
                    }
                }
                visit(work, x, source.palette, select, &plan);
            }
        });
    }
}

#[derive(Clone, Copy, Default)]
struct PlayfieldCell {
    pens: usize,
    palette: u16,
    pen_mask: u8,
    flip_x: u32,
    select: bool,
    tile: u32,
    tile_y: u8,
}

fn decode_cell(word: u32, texel_y: u32, tile_count: u32, palette_add: u16) -> PlayfieldCell {
    let attributes = (word >> 16) as u16;
    let tile = wrap_tile_index(word as u16 as u32, tile_count);
    let tile_y = texel_y ^ if attributes & 0x8000 != 0 { 15 } else { 0 };
    PlayfieldCell {
        pens: tile as usize * 256 + tile_y as usize * 16,
        palette: ((attributes & 0x1ff) * 16).wrapping_add(palette_add),
        pen_mask: (((attributes >> 10) & 3 & !attributes) << 4 | 15) as u8,
        flip_x: if attributes & 0x4000 != 0 { 15 } else { 0 },
        select: attributes & 0x200 != 0,
        tile,
        tile_y: tile_y as u8,
    }
}

struct ZoomState {
    texel: i32,
    column: u32,
    cell: PlayfieldCell,
    pen: u32,
    color: u32,
    resolves: bool,
}

impl Default for ZoomState {
    fn default() -> Self {
        Self {
            texel: i32::MIN,
            column: u32::MAX,
            cell: PlayfieldCell::default(),
            pen: 0,
            color: 0,
            resolves: false,
        }
    }
}

struct ZoomedPlayfield<'a> {
    state: ZoomState,
    map_row: &'a [u32],
    texel_y: u32,
    tile_pixels: &'a [u8],
    tile_count: u32,
    palette_add: u16,
    plan: LayerPlan,
    screen_flip: u32,
    map_width: u32,
}

impl ZoomedPlayfield<'_> {
    fn sample(
        &mut self,
        work: &mut RowWork<'_>,
        w: usize,
        i: u32,
        source: i32,
        unresolved: u32,
        settled: &mut u32,
    ) {
        if source != self.state.texel {
            self.state.texel = source;
            let wrapped = ((source as u32) & (self.map_width - 1)) ^ self.screen_flip;
            if (wrapped >> 4) != self.state.column {
                self.state.column = wrapped >> 4;
                self.state.cell = decode_cell(
                    self.map_row[self.state.column as usize],
                    self.texel_y,
                    self.tile_count,
                    self.palette_add,
                );
            }
            self.state.pen = u32::from(
                self.tile_pixels
                    [self.state.cell.pens + (((wrapped & 15) ^ self.state.cell.flip_x) as usize)],
            ) & u32::from(self.state.cell.pen_mask);
            self.state.resolves = self.plan.resolves[usize::from(self.state.cell.select)];
            self.state.color =
                work.lut[((self.state.cell.palette + self.state.pen as u16) as usize) & 8191];
        }
        if self.state.pen == 0 {
            return;
        }
        let x = (w * 32) as i32 + i as i32;
        if self.state.resolves && (unresolved >> i) & 1 != 0 {
            work.out[x as usize] = self.state.color;
            *settled |= 1u32 << i;
        } else {
            visit(
                work,
                x,
                self.state.cell.palette.wrapping_add(self.state.pen as u16),
                self.state.cell.select,
                &self.plan,
            );
        }
    }
}

fn compose_zoomed_playfield(
    work: &mut RowWork<'_>,
    id: LayerId,
    layer: &SceneLayer,
    ranges: &ColumnRanges,
    pf: &ScenePlayfield,
) {
    let plan = work.plan(layer, id);
    let cells = work
        .job
        .snapshot
        .tiles
        .cells(GameTiles::physical_map(sub_index(id), pf.alt_map));
    let tile_pixels = work.job.tile_pixels;
    let tile_count = (tile_pixels.len() / 256) as u32;
    let map_width = work.job.video.pf_map_width_px();
    let columns = work.job.video.pf_columns();
    let screen_flip = if work.flipped { map_width - 1 } else { 0 };
    let wrapped_y = (pf.source_y as u32 & 511) ^ if work.flipped { 511 } else { 0 };
    let map_row = &cells[(wrapped_y as usize / 16) * columns..];
    let texel_y = wrapped_y & 15;
    let origin = pf.source_x + (work.left_edge - 46) * pf.x_step;
    let mut zoom = ZoomedPlayfield {
        state: ZoomState::default(),
        map_row,
        texel_y,
        tile_pixels,
        tile_count,
        palette_add: pf.palette_add,
        plan,
        screen_flip,
        map_width,
    };
    for r in 0..ranges.count {
        let (left, right) = (ranges.range[r][0], ranges.range[r][1]);
        for_each_range_word(left, right, |w, window| {
            let unresolved = work.unresolved[w];
            let mut settled = 0u32;
            if (unresolved & window) == window && (work.pending[w] & window) == 0 {
                let first = window.trailing_zeros();
                let end = 32 - window.leading_zeros();
                let mut source = origin + (w * 32) as i32 * pf.x_step + first as i32 * pf.x_step;
                for i in first..end {
                    zoom.sample(work, w, i, source >> 8, unresolved & window, &mut settled);
                    source += pf.x_step;
                }
            } else {
                let mut mask = (unresolved | work.pending[w]) & window;
                while mask != 0 {
                    let i = mask.trailing_zeros();
                    mask &= mask - 1;
                    let source = (origin + ((w * 32) as i32 + i as i32) * pf.x_step) >> 8;
                    zoom.sample(work, w, i, source, unresolved, &mut settled);
                }
            }
            work.unresolved[w] &= !settled;
        });
    }
}

fn write_cell(out: &mut [u32], lut: &[u32], base: usize, pens: &PenRow, mirrored: bool) {
    for group in 0..4 {
        let four = pens.group(group, mirrored);
        let pixel = &mut out[4 * group..4 * group + 4];
        pixel[0] = lut[base + (four & 0xff) as usize];
        pixel[1] = lut[base + ((four >> 8) & 0xff) as usize];
        pixel[2] = lut[base + ((four >> 16) & 0xff) as usize];
        pixel[3] = lut[base + (four >> 24) as usize];
    }
}

fn compose_playfield(
    work: &mut RowWork<'_>,
    id: LayerId,
    layer: &SceneLayer,
    ranges: &ColumnRanges,
) {
    let pf = playfield_row(work, id);
    if pf.x_step != 256 {
        compose_zoomed_playfield(work, id, layer, ranges, &pf);
        return;
    }
    let plan = work.plan(layer, id);
    let cells = work
        .job
        .snapshot
        .tiles
        .cells(GameTiles::physical_map(sub_index(id), pf.alt_map));
    let tile_pixels = work.job.tile_pixels;
    let tile_rows = work.job.tile_rows;
    let tile_count = (tile_pixels.len() / 256) as u32;
    let map_width = work.job.video.pf_map_width_px();
    let columns = work.job.video.pf_columns();
    let screen_flip = if work.flipped { map_width - 1 } else { 0 };
    let base = floor_divide(pf.source_x, 256);
    let wrapped_y = (pf.source_y as u32 & 511) ^ if work.flipped { 511 } else { 0 };
    let map_row = &cells[(wrapped_y as usize / 16) * columns..];
    let texel_y = wrapped_y & 15;
    for r in 0..ranges.count {
        let (left, right) = (ranges.range[r][0], ranges.range[r][1]);
        let mut x = left;
        while x < right {
            let wrapped = ((base + x - work.border) as u32 & (map_width - 1)) ^ screen_flip;
            let texel = wrapped & 15;
            let run = (if work.flipped { texel + 1 } else { 16 - texel }).min((right - x) as u32);
            let unresolved = extract_columns(&work.unresolved, x, run);
            let open = open_columns(work, x, run);
            if open == 0 {
                x += run as i32;
                if x < right {
                    x = next_open_column(work, x, right);
                }
                continue;
            }
            let cell = decode_cell(
                map_row[(wrapped >> 4) as usize],
                texel_y,
                tile_count,
                pf.palette_add,
            );
            if !tile_rows[cell.tile as usize].visible(cell.tile_y as u32, u32::from(cell.pen_mask))
            {
                x += run as i32;
                continue;
            }
            let palette_base = usize::from(cell.palette) & 8191;
            let mirrored = work.flipped != (cell.flip_x != 0);
            let pens = PenRow::new(&tile_pixels[cell.pens..cell.pens + 16], cell.pen_mask);
            if open == 0xffff
                && unresolved == 0xffff
                && plan.resolves[usize::from(cell.select)]
                && run == 16
                && pens.all_opaque()
            {
                write_cell(
                    &mut work.out[x as usize..],
                    work.lut,
                    palette_base,
                    &pens,
                    mirrored,
                );
                clear_columns(&mut work.unresolved, x, 0xffff);
                x += 16;
                continue;
            }
            let live =
                open & (pens.opaque(mirrored) >> if work.flipped { 15 - texel } else { texel });
            if live != 0 {
                let source = cell.pens + (texel ^ cell.flip_x) as usize;
                let step = if mirrored { -1i32 } else { 1 };
                let mut rest = live;
                if plan.resolves[usize::from(cell.select)] {
                    let fast = live & unresolved;
                    for_each_run(fast, |first, length| {
                        for i in 0..length {
                            let at = (source as i32 + (first as i32 + i as i32) * step) as usize;
                            let pen = u32::from(tile_pixels[at]) & u32::from(cell.pen_mask);
                            work.out[(x + first as i32 + i as i32) as usize] =
                                work.lut[palette_base + pen as usize];
                        }
                    });
                    clear_columns(&mut work.unresolved, x, fast);
                    rest &= !fast;
                }
                if rest != 0 {
                    visit_run(work, x, rest, cell.select, &plan, |column| {
                        let at = (source as i32 + (column as i32 - x) * step) as usize;
                        let pen = u32::from(tile_pixels[at]) & u32::from(cell.pen_mask);
                        cell.palette.wrapping_add(pen as u16)
                    });
                }
            }
            x += run as i32;
        }
    }
}

fn compose_text(work: &mut RowWork<'_>, layer: &SceneLayer, ranges: &ColumnRanges) {
    let id = LayerId::Text;
    let text = work.job.snapshot.text;
    let plan = work.plan(layer, id);
    let resolves = plan.resolves[usize::from(layer.blend_select)];
    let bitmap = work.row.bitmap;
    let y_mask = if bitmap { 255 } else { 511 };
    let source_y = i32::from(work.row.text_y) & y_mask;
    let y = (if work.flipped {
        y_mask - source_y
    } else {
        source_y
    }) as u32;
    let map = text.map();
    let ram: &[u8] = if bitmap { text.pivot() } else { text.glyphs() };
    let glyph_rows = work.job.glyph_rows;
    let map_row = if bitmap {
        text.bitmap_map_row(source_y as u32, work.flipped)
    } else {
        y / 8
    };
    let texel_y = y & 7;
    for r in 0..ranges.count {
        let (left, right) = (ranges.range[r][0], ranges.range[r][1]);
        let mut x = left;
        while x < right {
            let u = (i32::from(work.row.text_x) + x - work.border) as u32 & 511;
            let phase = u & 7;
            let len = (8 - phase).min((right - x) as u32);
            let cell = if work.flipped { 63 - (u >> 3) } else { u >> 3 };
            let word = map[(map_row * 64 + cell) as usize];
            let tile = if bitmap {
                cell * 32 + y / 8
            } else {
                u32::from(word & 0xff)
            };
            let flip_y = if word & 0x8000 != 0 { 7 } else { 0 };
            let glyph_y = texel_y ^ flip_y;
            let visible = bitmap || ((glyph_rows[tile as usize] >> glyph_y) & 1 != 0);
            let unresolved = if visible {
                extract_columns(&work.unresolved, x, len)
            } else {
                0
            };
            let open = if work.any_pending && visible {
                unresolved | extract_columns(&work.pending, x, len)
            } else {
                unresolved
            };
            if open != 0 {
                let mirrored = work.flipped != (word & 0x0100 != 0);
                let at = tile as usize * 32 + glyph_y as usize * 4;
                let pens = glyph_row_pens(&ram[at..at + 4], mirrored);
                let live = open & (nonzero_nibbles(pens) >> phase);
                let palette_base = usize::from((word >> 9) & 0x3f) * 16;
                let direct = if resolves { live & unresolved } else { 0 };
                let shift = phase * 4;
                if direct == 0xff {
                    for i in 0..8u32 {
                        work.out[(x + i as i32) as usize] =
                            work.lut[palette_base + ((pens >> (i * 4)) & 0x0f) as usize];
                    }
                } else {
                    let mut bits = direct;
                    while bits != 0 {
                        let i = bits.trailing_zeros();
                        bits &= bits - 1;
                        work.out[(x + i as i32) as usize] =
                            work.lut[palette_base + ((pens >> (shift + i * 4)) & 0x0f) as usize];
                    }
                }
                if direct != 0 {
                    clear_columns(&mut work.unresolved, x, direct);
                }
                let rest = live & !direct;
                if rest != 0 {
                    visit_run(work, x, rest, layer.blend_select, &plan, |column| {
                        (palette_base as u16).wrapping_add(
                            ((pens >> (shift + (column - x as u32) * 4)) & 0x0f) as u16,
                        )
                    });
                }
            }
            x += len as i32;
        }
    }
}

fn compose_sprites(work: &mut RowWork<'_>, id: LayerId, layer: &SceneLayer, ranges: &ColumnRanges) {
    let index = sub_index(id);
    let coverage = work.coverage;
    if !coverage.covers(index) {
        return;
    }
    let covered = coverage.columns(index);
    let plane = work.plane;
    let plane_offset = if work.expanded {
        0
    } else {
        work.left_edge as usize
    };
    let plan = work.plan(layer, id);
    let resolves = plan.resolves[usize::from(layer.blend_select)];
    for r in 0..ranges.count {
        let (left, right) = (ranges.range[r][0], ranges.range[r][1]);
        for_each_range_word(left, right, |w, window| {
            let mut pending = covered[w] & work.pending[w] & window;
            let direct = covered[w] & work.unresolved[w] & window;
            if resolves {
                work.unresolved[w] &= !direct;
                for_each_run(direct, |first, length| {
                    let x = w * 32 + first as usize;
                    for i in 0..length as usize {
                        work.out[x + i] = work.lut[plane[plane_offset + x + i] as usize];
                    }
                });
            } else {
                pending |= direct;
            }
            if pending != 0 {
                visit_bits(work, w, pending, layer.blend_select, &plan, |x| {
                    plane[plane_offset + x as usize]
                });
            }
        });
    }
}

fn blend_argb(source: u32, destination: u32, source_weight: u32, destination_weight: u32) -> u32 {
    let blue_red = ((source & 0x00ff_00ff) * source_weight
        + (destination & 0x00ff_00ff) * destination_weight)
        >> 3
        & 0x01ff_01ff;
    let green = (((source >> 8) & 0xff) * source_weight
        + ((destination >> 8) & 0xff) * destination_weight)
        >> 3;
    let over = (blue_red >> 8) & 0x0001_0001;
    0xff00_0000 | ((blue_red | (over * 0xff)) & 0x00ff_00ff) | green.min(255) << 8
}

fn resolve_pending(work: &mut RowWork<'_>) -> u32 {
    let rgba = work.job.format == PixelFormat::Rgba8888;
    let rotate = if rgba { 8 } else { 0 };
    let low = if rgba { 0xff } else { 0 };
    let mut result = BlendResult::default();
    let mut visits = LayerVisits::default();
    let mut source = u32::MAX;
    let mut destination = u32::MAX;
    let mut color = 0u32;
    let mut count = 0u32;
    for w in 0..work.words {
        let mut mask = work.pending[w];
        while mask != 0 {
            let x = w * 32 + mask.trailing_zeros() as usize;
            mask &= mask - 1;
            count += 1;
            let sample = work.samples[x];
            if sample.visits != visits {
                visits = sample.visits;
                result = work.blend_result(visits);
                source = u32::MAX;
                destination = u32::MAX;
            }
            let source_index = u32::from(sample.index[result.source as usize]) & 8191;
            let destination_index = u32::from(sample.index[result.destination as usize]) & 8191;
            if result.source_weight == 8 && result.destination_weight == 0 {
                work.out[x] = work.lut[source_index as usize];
                continue;
            }
            if result.source_weight == 0 && result.destination_weight == 8 {
                work.out[x] = work.lut[destination_index as usize];
                continue;
            }
            if source_index != source || destination_index != destination {
                source = source_index;
                destination = destination_index;
                color = (blend_argb(
                    work.lut[source as usize] >> rotate,
                    work.lut[destination as usize] >> rotate,
                    u32::from(result.source_weight),
                    u32::from(result.destination_weight),
                ) << rotate)
                    | low;
            }
            work.out[x] = color;
        }
    }
    count
}

fn setup_matches(setup: &RowSetup, row: &SceneRow, active: u32) -> bool {
    if !setup.valid || setup.active != active || setup.clips != row.clips {
        return false;
    }
    for i in 0..LAYER_COUNT {
        if setup.layers[i] != *row.layer(layer_id(i)) {
            return false;
        }
    }
    true
}

fn build_setup(setup: &mut RowSetup, row: &SceneRow, active: u32, left_edge: i32, right_edge: i32) {
    for i in 0..LAYER_COUNT {
        setup.layers[i] = *row.layer(layer_id(i));
    }
    setup.clips = row.clips;
    setup.active = active;
    setup.order_count = 0;
    setup.settle = 0;
    for id in scene_order(row) {
        if (active >> (id as u32)) & 1 == 0 {
            continue;
        }
        setup.order[setup.order_count] = id;
        setup.order_count += 1;
        setup.ranges[id as usize] = column_ranges(row, row.layer(id), left_edge, right_edge);
    }
    for n in 0..setup.order_count {
        let layer = row.layer(setup.order[n]);
        let mut tie = false;
        for m in n + 1..setup.order_count {
            let later = row.layer(setup.order[m]);
            if later.priority != layer.priority {
                break;
            }
            tie |= later.blend_mode != layer.blend_mode;
        }
        if layer.priority != 0 && !tie {
            setup.settle |= 1u32 << (setup.order[n] as u32);
        }
    }
    setup.valid = true;
}

fn compose_row(
    job: &SharedJob<'_>,
    y: usize,
    out: &mut [u32],
    plane_row: &[u16],
    coverage: SpriteCoverageRow<'_>,
    setup: &mut RowSetup,
    argb_scratch: &mut [u32; MAX_WIDTH],
) {
    let scene = job.snapshot;
    let row = &scene.rows[y & 255];
    let width = job.options.width(job.geometry) as usize;
    let flipped = scene.flipped;
    let expanded = job.options.expanded();
    let mut active = 0u32;
    for i in 0..LAYER_COUNT {
        let id = layer_id(i);
        let text_y = if flipped {
            511 - i32::from(row.text_y)
        } else {
            i32::from(row.text_y)
        };
        let empty = match kind(id) {
            LayerKind::Playfield => row.playfields[sub_index(id)].empty_row,
            LayerKind::Text => !row.bitmap && !scene.text.row_used(text_y as u32),
            LayerKind::Sprite => !coverage.covers(sub_index(id)),
        };
        if row.layer(id).enabled && !empty {
            active |= 1u32 << i;
        }
    }
    let left_edge = 46 - job.options.border as i32;
    let right_edge = 366 + job.options.border as i32;
    if !setup_matches(setup, row, active) {
        build_setup(setup, row, active, left_edge, right_edge);
    }

    let mut samples = [BlendSamples::default(); MAX_WIDTH];
    let lut: &[u32] = if row.palette_15bit {
        job.palette15
    } else {
        job.palette
    };
    let mut work = RowWork {
        job,
        row,
        width,
        words: 0,
        left_edge,
        border: job.options.border as i32,
        expanded,
        flipped,
        out,
        lut,
        samples: &mut samples,
        unresolved: [0; MAX_WORDS],
        pending: [0; MAX_WORDS],
        any_pending: false,
        setup,
        plane: plane_row,
        coverage,
        memo: BlendMemo::new(),
    };
    work.reset();

    for n in 0..setup.order_count {
        let id = setup.order[n];
        let layer = *row.layer(id);
        let ranges = setup.ranges[id as usize];
        if layer.mosaic && row.mosaic_period > 1 {
            compose_mosaic_layer(&mut work, id, &layer, &ranges);
            continue;
        }
        match kind(id) {
            LayerKind::Playfield => compose_playfield(&mut work, id, &layer, &ranges),
            LayerKind::Sprite => compose_sprites(&mut work, id, &layer, &ranges),
            LayerKind::Text => compose_text(&mut work, &layer, &ranges),
        }
    }

    let background = work.lut[usize::from(row.background) & 8191];
    for w in 0..work.words {
        let mut mask = work.unresolved[w];
        if mask == u32::MAX {
            work.out[w * 32..w * 32 + 32].fill(background);
            continue;
        }
        while mask != 0 {
            let x = w * 32 + mask.trailing_zeros() as usize;
            mask &= mask - 1;
            work.out[x] = background;
        }
    }
    if work.any_pending {
        let special = resolve_pending(&mut work);
        if special != 0 {
            job.stats.add(special);
        }
    }
    if row.blur {
        for (x, scratch) in argb_scratch.iter_mut().enumerate().take(width) {
            *scratch = job.format.from_pixel_format(work.out[x]);
        }
        for x in (0..width).rev() {
            let color = argb_scratch[x];
            let previous = if x >= 1 { argb_scratch[x - 1] } else { 0 };
            let rb = (((color & 0x00ff_00ff) + (previous & 0x00ff_00ff)) >> 1) & 0x00ff_00ff;
            let g = (((color & 0x0000_ff00) + (previous & 0x0000_ff00)) >> 1) & 0x0000_ff00;
            argb_scratch[x] = 0xff00_0000 | rb | g;
        }
        for (x, scratch) in argb_scratch.iter().enumerate().take(width) {
            work.out[x] = job.format.to_pixel_format(*scratch);
        }
    }
}

struct ChunkBuffers<'a> {
    output: &'a mut [u32],
    plane: &'a mut [u16],
    layers: &'a mut [u8],
    bits: &'a mut [u32],
}

fn compose_indexed_chunk(job: &SharedJob<'_>, chunk: usize, buffers: ChunkBuffers<'_>) {
    let options = job.options;
    let geometry = job.geometry;
    let height = geometry.height() as usize;
    let first = chunk * COMPOSE_CHUNK_ROWS;
    let last = (first + COMPOSE_CHUNK_ROWS).min(height);
    if first >= last {
        return;
    }
    let plane_stride = if options.expanded() {
        options.width(geometry) as usize
    } else {
        crate::game::sprites::SPRITE_PLANE_WIDTH
    };
    {
        let mut coverage = SpriteCoverage::new(&options, geometry, buffers.layers, buffers.bits);
        let raster = SpriteRaster {
            sprites: job.sprites,
            pen_mask: job.snapshot.pen_mask,
            tiles: job.sprite_tiles,
            trail_seed: job.snapshot.sprite_trail_seed,
        };
        raster.draw_chunk(
            job.bins,
            chunk,
            buffers.plane,
            geometry,
            SpriteRasterOptions {
                geometry: options,
                flipped: job.snapshot.sprite_flipped,
            },
            &mut coverage,
        );
    }
    let layers: &[u8] = buffers.layers;
    let bits: &[u32] = buffers.bits;
    let plane: &[u16] = buffers.plane;
    let coverage_view = SpriteCoverageView::new(&options, geometry, layers, bits);
    let mut setup = RowSetup::new();
    let mut argb_scratch = [0u32; MAX_WIDTH];
    for row in first..last {
        let local = row - first;
        let out = &mut buffers.output[local * job.stride..(local + 1) * job.stride];
        let plane_row = &plane[local * plane_stride..(local + 1) * plane_stride];
        compose_row(
            job,
            geometry.first_line() as usize + row,
            out,
            plane_row,
            coverage_view.row(local),
            &mut setup,
            &mut argb_scratch,
        );
    }
}

#[derive(Clone, Copy)]
pub struct IndexedLayout {
    pub stride: usize,
    pub format: PixelFormat,
}

pub struct IndexedOutput<'a> {
    pub pixels: &'a mut [u32],
    pub layout: IndexedLayout,
}

pub struct IndexedTiles<'a> {
    pub playfield_rows: &'a TileRowTable,
    pub playfield_pixels: &'a [u8],
    pub sprite_pixels: &'a [u8],
}

#[derive(Default)]
pub struct RowExecution<'a> {
    pub workers: Option<&'a RowWorkers>,
    pub window: Option<&'a mut dyn WorkWindow>,
}

pub(super) struct IndexedOptions {
    pub raster: GameVideoOptions,
    pub sprites: SpriteList,
}

pub(super) fn compose_indexed(
    snapshot: &SceneSnapshotView<'_>,
    output: IndexedOutput<'_>,
    options: IndexedOptions,
    geometry: &Geometry,
    tiles: IndexedTiles<'_>,
    scratch: &mut IndexedScratch,
    execution: RowExecution<'_>,
) {
    let IndexedOutput {
        pixels: output,
        layout: IndexedLayout { stride, format },
    } = output;
    let IndexedOptions {
        raster: options,
        sprites: list,
    } = options;
    let IndexedTiles {
        playfield_rows: playfield_tiles,
        playfield_pixels,
        sprite_pixels: sprite_tiles,
    } = tiles;
    let RowExecution { workers, window } = execution;
    scratch.configure(options, geometry);
    let palette_15bit = scratch.prepare(snapshot, format);
    scratch.stats.reset();
    let sprite_view = match list {
        SpriteList::Canonical => snapshot.canonical_sprites,
        SpriteList::Presented => snapshot.sprites,
    };
    scratch.bins.bin(
        sprite_view,
        geometry,
        SpriteRasterOptions {
            geometry: options,
            flipped: snapshot.sprite_flipped,
        },
    );

    let IndexedScratch {
        plane,
        coverage_layers,
        coverage_bits,
        bins,
        palette,
        glyph_rows,
        stats,
    } = scratch;

    let job = SharedJob {
        snapshot,
        tile_pixels: playfield_pixels,
        sprite_tiles,
        sprites: sprite_view,
        tile_rows: playfield_tiles.rows(),
        glyph_rows,
        palette: &palette.colors,
        palette15: if palette_15bit {
            &palette.colors15
        } else {
            &[]
        },
        stride,
        format,
        options,
        geometry,
        video: snapshot.tiles.video(),
        stats,
        bins,
    };

    let height = geometry.height() as usize;
    let plane_stride = if options.expanded() {
        options.width(geometry) as usize
    } else {
        crate::game::sprites::SPRITE_PLANE_WIDTH
    };
    let plane_base = if options.expanded() {
        0
    } else {
        geometry.first_line() as usize
    };
    let words = sprite_coverage_words(options, geometry) * SpriteCoverage::LAYERS;
    let chunks = compose_chunk_count(geometry.first_line(), geometry.end_line());

    let mut plane_chunks = plane[plane_base * plane_stride..(plane_base + height) * plane_stride]
        .chunks_mut(COMPOSE_CHUNK_ROWS * plane_stride);
    let mut layer_chunks =
        coverage_layers[plane_base..plane_base + height].chunks_mut(COMPOSE_CHUNK_ROWS);
    let mut bit_chunks = coverage_bits[plane_base * words..(plane_base + height) * words]
        .chunks_mut(COMPOSE_CHUNK_ROWS * words);
    let mut output_chunks = output[..height * stride].chunks_mut(COMPOSE_CHUNK_ROWS * stride);

    let mut items = Vec::with_capacity(chunks);
    for chunk in 0..chunks {
        items.push((
            chunk,
            ChunkBuffers {
                output: output_chunks.next().expect("output chunk"),
                plane: plane_chunks.next().expect("plane chunk"),
                layers: layer_chunks.next().expect("coverage layer chunk"),
                bits: bit_chunks.next().expect("coverage bit chunk"),
            },
        ));
    }

    match workers {
        Some(workers) => workers.run_owned(
            items,
            |chunk, buffers| compose_indexed_chunk(&job, chunk, buffers),
            window,
        ),
        None => {
            for (chunk, buffers) in items {
                compose_indexed_chunk(&job, chunk, buffers);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::SceneSnapshot;
    use crate::game::row_workers::compose_workers;
    use crate::game::scene::SceneSprite;

    const FIRST: usize = 24;

    fn visible_layer(priority: u8, blend_mode: u8) -> SceneLayer {
        SceneLayer {
            enabled: true,
            priority,
            blend_mode,
            // No clip windows selected and inverted => the full scanout window.
            clip_inverse: true,
            ..SceneLayer::default()
        }
    }

    /// A tile ROM with one 16x16 tile whose row-0 pen values are `row`.
    fn tile_rom(row: [u8; 16]) -> Vec<u8> {
        let mut tiles = vec![0u8; 256];
        tiles[..16].copy_from_slice(&row);
        tiles
    }

    fn two_tile_rom(first: [u8; 16], second: [u8; 16]) -> Vec<u8> {
        let mut tiles = vec![0u8; 512];
        tiles[..16].copy_from_slice(&first);
        tiles[256..272].copy_from_slice(&second);
        tiles
    }

    fn scene() -> SceneSnapshot {
        let mut snapshot = SceneSnapshot::new(&VideoConfig::default());
        snapshot.fallback = false;
        snapshot.colors[0] = 0x0000_0000;
        snapshot.colors[1] = 0x00ff_0000;
        snapshot.colors[2] = 0x0000_ff00;
        snapshot.colors[0x1001] = 0x0000_00ff;
        snapshot
    }

    fn cell(attributes: u16, tile: u16) -> u32 {
        (u32::from(attributes) << 16) | u32::from(tile)
    }

    /// Decode text map word 0 (`glyph`) and glyph RAM (row 0 bytes at glyph 1) through the real
    /// decoder so the row-usage cull sees them.
    fn decode_text(snapshot: &mut SceneSnapshot, glyph: u16, row: &[u8; 4]) {
        use crate::memory_map::{GRAPHICS_RAM_SIZE, OFFS_TEXTRAM_GLYPHS, OFFS_TEXTRAM_MAP};
        let mut graphics = vec![0u8; GRAPHICS_RAM_SIZE];
        graphics[OFFS_TEXTRAM_MAP..OFFS_TEXTRAM_MAP + 2].copy_from_slice(&glyph.to_be_bytes());
        graphics[OFFS_TEXTRAM_GLYPHS + 32..OFFS_TEXTRAM_GLYPHS + 36].copy_from_slice(row);
        let control = [0u8; 0x20];
        snapshot
            .text
            .decode(&crate::game::scene::VideoRam::new(&graphics, &control, 1));
    }

    fn run(
        snapshot: &SceneSnapshot,
        border: u32,
        layout: IndexedLayout,
        workers: Option<&RowWorkers>,
        tile_pixels: &[u8],
        sprite_tiles: &[u8],
        list: SpriteList,
    ) -> (Vec<u32>, u32) {
        let height = snapshot.geometry().height() as usize;
        let width = GameVideoOptions { scale: 1, border }.width(&snapshot.geometry()) as usize;
        let stride = layout.stride;
        assert!(stride >= width);
        let mut output = vec![0u32; stride * height];
        let mut scratch = IndexedScratch::new();
        let tile_rows = TileRowTable::new(tile_pixels);
        snapshot
            .compose(
                IndexedOutput {
                    pixels: &mut output,
                    layout,
                },
                border,
                IndexedTiles {
                    playfield_rows: &tile_rows,
                    playfield_pixels: tile_pixels,
                    sprite_pixels: sprite_tiles,
                },
                &mut scratch,
                list,
                RowExecution {
                    workers,
                    window: None,
                },
            )
            .unwrap();
        (output, scratch.stats.special_px())
    }

    proptest::proptest! {
        #![proptest_config(proptest::test_runner::Config::with_cases(24))]
        #[test]
        fn memo_matches_uncached_replay_across_rows(
            visits in proptest::collection::vec(1u16..512, 65..130),
            blend in proptest::array::uniform4(0u8..=8),
            priorities in proptest::array::uniform9(1u8..16),
            modes in proptest::array::uniform9(0u8..4),
        ) {
            use proptest::prelude::*;
            let mut row = SceneRow { blend, ..SceneRow::default() };
            for i in 0..LAYER_COUNT {
                let layer = row.layer_mut(layer_id(i));
                layer.priority = priorities[i];
                layer.blend_mode = modes[i];
            }
            let order = scene_order(&row);
            let mut memo = BlendMemo::new();
            for _ in 0..2 {
                for &mask in visits.iter().chain(visits.iter().rev()) {
                    let mut visits = LayerVisits::default();
                    for i in 0..LAYER_COUNT as u32 {
                        if mask & (1 << i) != 0 {
                            visits.add(LayerVisits::pass(i, i % 2 == 0));
                        }
                    }
                    let actual = memo.resolve(visits, || replay_blend(&row, &order, visits));
                    prop_assert_eq!(actual, replay_blend(&row, &order, visits));
                }
                row.blend = blend.map(|weight| 8 - weight);
                memo.clear();
            }
        }
    }

    #[test]
    fn tile_rows_report_visible_pen_rows() {
        let mut tiles = vec![0u8; 256];
        tiles[0] = 0x01; // low plane, row 0
        tiles[17] = 0x10; // plane 4, row 1
        tiles[34] = 0x20; // plane 5, row 2
        let table = TileRowTable::new(&tiles);
        assert!(table.rows()[0].visible(0, 15));
        assert!(table.rows()[0].visible(1, 0x1f));
        assert!(table.rows()[0].visible(2, 0x2f));
        assert!(!table.rows()[0].visible(1, 0x0f));
        assert!(!table.rows()[0].visible(2, 0x1f));
        assert!(!table.rows()[0].visible(3, 0x1f));
    }

    #[test]
    fn playfield_draws_pen_colours() {
        let mut snapshot = scene();
        snapshot.rows[FIRST].playfields[0].layer = visible_layer(1, 0);
        snapshot.rows[FIRST].blend = [8, 0, 0, 0];
        snapshot.tiles.cells_mut(0)[0] = cell(0, 0);
        let tiles = tile_rom([1; 16]);
        let (pixels, special) = run(
            &snapshot,
            0,
            IndexedLayout {
                stride: 320,
                format: PixelFormat::Argb8888,
            },
            None,
            &tiles,
            &[],
            SpriteList::Presented,
        );
        assert_eq!(pixels[0], 0xffff_0000);
        assert_eq!(pixels[15], 0xffff_0000);
        assert_eq!(pixels[16], 0xffff_0000, "map column 1 repeats tile 0");
        assert_eq!(special, 0, "priority-0 blend mode resolves directly");
    }

    #[test]
    fn border_columns_shift_the_picture_symmetrically() {
        let mut snapshot = scene();
        snapshot.rows[FIRST].playfields[0].layer = visible_layer(1, 0);
        snapshot.rows[FIRST].blend = [8, 0, 0, 0];
        snapshot.tiles.cells_mut(0)[0] = cell(0, 0);
        let tiles = tile_rom([1; 16]);
        let (plain, _) = run(
            &snapshot,
            0,
            IndexedLayout {
                stride: 320,
                format: PixelFormat::Argb8888,
            },
            None,
            &tiles,
            &[],
            SpriteList::Presented,
        );
        let (bordered, _) = run(
            &snapshot,
            4,
            IndexedLayout {
                stride: 328,
                format: PixelFormat::Argb8888,
            },
            None,
            &tiles,
            &[],
            SpriteList::Presented,
        );
        for y in 0..snapshot.geometry().height() as usize {
            assert_eq!(
                &bordered[y * 328 + 4..y * 328 + 324],
                &plain[y * 320..y * 320 + 320]
            );
        }
    }

    #[test]
    fn sprites_and_trails_seed_the_plane() {
        let mut snapshot = scene();
        snapshot.rows[FIRST].sprites[0] = visible_layer(1, 0);
        snapshot.rows[FIRST].blend = [8, 0, 0, 0];
        snapshot.sprites[0] = SceneSprite {
            x: (46 << 8) - 128,
            y: (FIRST as i32) * 256 - 255,
            tile: 0,
            ..SceneSprite::default()
        };
        snapshot.sprite_count = 1;
        let tiles = tile_rom([1; 16]);
        let (pixels, _) = run(
            &snapshot,
            0,
            IndexedLayout {
                stride: 320,
                format: PixelFormat::Argb8888,
            },
            None,
            &[],
            &tiles,
            SpriteList::Presented,
        );
        assert_eq!(pixels[0], 0xff00_00ff);

        let mut trails = scene();
        trails.rows[FIRST].sprites[0] = visible_layer(1, 0);
        trails.rows[FIRST].blend = [8, 0, 0, 0];
        trails.sprite_trail_seed = vec![0u16; crate::game::sprites::TRAIL_PLANE_PIXELS];
        trails.sprite_trail_seed[FIRST * 432 + 46] = 0x1001;
        let (pixels, _) = run(
            &trails,
            0,
            IndexedLayout {
                stride: 320,
                format: PixelFormat::Argb8888,
            },
            None,
            &[],
            &[],
            SpriteList::Canonical,
        );
        assert_eq!(pixels[0], 0xff00_00ff);
    }

    #[test]
    fn text_layer_draws_glyph_pens() {
        let mut snapshot = scene();
        snapshot.rows[FIRST].text = visible_layer(1, 0);
        snapshot.rows[FIRST].blend = [8, 0, 0, 0];
        snapshot.rows[FIRST].text_y = 0;
        decode_text(&mut snapshot, 1, &[0x11; 4]);
        let (pixels, _) = run(
            &snapshot,
            0,
            IndexedLayout {
                stride: 320,
                format: PixelFormat::Argb8888,
            },
            None,
            &[],
            &[],
            SpriteList::Presented,
        );
        assert_eq!(pixels[0], 0xffff_0000);
        assert_eq!(pixels[7], 0xffff_0000);
        assert_eq!(pixels[8], 0xff00_0000);
    }

    #[test]
    fn clip_windows_limit_the_playfield() {
        let mut snapshot = scene();
        let mut layer = visible_layer(1, 0);
        layer.clip_enabled = 1;
        layer.clip_inverted = 1;
        layer.clip_inverse = false;
        snapshot.rows[FIRST].playfields[0].layer = layer;
        snapshot.rows[FIRST].clips[0] = SceneClip {
            left: 52,
            right: 56,
        };
        snapshot.rows[FIRST].blend = [8, 0, 0, 0];
        snapshot.tiles.cells_mut(0)[0] = cell(0, 0);
        let tiles = tile_rom([1; 16]);
        let (pixels, _) = run(
            &snapshot,
            0,
            IndexedLayout {
                stride: 320,
                format: PixelFormat::Argb8888,
            },
            None,
            &tiles,
            &[],
            SpriteList::Presented,
        );
        assert_eq!(pixels[0], 0xff00_0000);
        for (x, pixel) in pixels.iter().enumerate().take(10).skip(6) {
            assert_eq!(*pixel, 0xffff_0000, "window column {x}");
        }
        assert_eq!(pixels[10], 0xff00_0000);
    }

    #[test]
    fn mosaic_samples_the_quantised_column() {
        let mut snapshot = scene();
        let mut layer = visible_layer(1, 0);
        layer.mosaic = true;
        snapshot.rows[FIRST].playfields[0].layer = layer;
        snapshot.rows[FIRST].mosaic_period = 16;
        snapshot.rows[FIRST].blend = [8, 0, 0, 0];
        snapshot.tiles.cells_mut(0)[0] = cell(0, 0);
        // Column 63 is the wrap target for the first phase sample (columns 44/45).
        snapshot.tiles.cells_mut(0)[63] = cell(0, 1);
        let tiles = two_tile_rom([1; 16], [0; 16]);
        let (mosaic, _) = run(
            &snapshot,
            0,
            IndexedLayout {
                stride: 320,
                format: PixelFormat::Argb8888,
            },
            None,
            &tiles,
            &[],
            SpriteList::Presented,
        );
        assert_eq!(mosaic[0], 0xff00_0000, "sampled the empty wrapped column");
        assert_eq!(mosaic[0], mosaic[5]);

        snapshot.rows[FIRST].playfields[0].layer.mosaic = false;
        let (plain, _) = run(
            &snapshot,
            0,
            IndexedLayout {
                stride: 320,
                format: PixelFormat::Argb8888,
            },
            None,
            &tiles,
            &[],
            SpriteList::Presented,
        );
        assert_eq!(plain[0], 0xffff_0000);
    }

    #[test]
    fn pending_blend_matches_the_argb_formula() {
        let mut snapshot = scene();
        snapshot.colors[1] = 0x0010_2030;
        snapshot.colors[2] = 0x0040_5060;
        snapshot.rows[FIRST].blend = [4, 0, 8, 0];
        snapshot.rows[FIRST].playfields[0].layer = visible_layer(1, 1);
        snapshot.rows[FIRST].playfields[1].layer = visible_layer(1, 2);
        snapshot.tiles.cells_mut(0)[0] = cell(0, 0);
        snapshot.tiles.cells_mut(1)[0] = cell(0, 1);
        let tiles = two_tile_rom([1; 16], [2; 16]);
        let (pixels, special) = run(
            &snapshot,
            0,
            IndexedLayout {
                stride: 320,
                format: PixelFormat::Argb8888,
            },
            None,
            &tiles,
            &[],
            SpriteList::Presented,
        );
        // (source * 8 + destination * 4) >> 3, source = PF0 pen 1, destination = PF1 pen 2.
        let argb = (0x10 * 8 + 0x40 * 4) >> 3;
        let green = (0x20 * 8 + 0x50 * 4) >> 3;
        let blue = (0x30 * 8 + 0x60 * 4) >> 3;
        assert_eq!(pixels[0], 0xff00_0000 | argb << 16 | green << 8 | blue);
        assert!(special > 0);
    }

    #[test]
    fn serial_and_parallel_composition_agree() {
        let mut snapshot = scene();
        snapshot.rows[FIRST].blend = [4, 0, 8, 0];
        snapshot.rows[FIRST].playfields[0].layer = visible_layer(1, 1);
        snapshot.rows[FIRST].playfields[1].layer = visible_layer(1, 2);
        snapshot.rows[FIRST].sprites[0] = visible_layer(1, 0);
        snapshot.rows[FIRST].text = visible_layer(1, 0);
        snapshot.rows[FIRST].text_y = 0;
        snapshot.tiles.cells_mut(0)[0] = cell(0, 0);
        snapshot.tiles.cells_mut(1)[0] = cell(0, 1);
        decode_text(&mut snapshot, 1, &[0x11; 4]);
        snapshot.sprites[0] = SceneSprite {
            x: (46 << 8) - 128,
            y: (FIRST as i32) * 256 - 255,
            tile: 0,
            ..SceneSprite::default()
        };
        snapshot.sprite_count = 1;
        let tiles = two_tile_rom([1; 16], [2; 16]);
        for border in [0u32, 4, 160] {
            for format in [PixelFormat::Argb8888, PixelFormat::Rgba8888] {
                let width =
                    GameVideoOptions { scale: 1, border }.width(&snapshot.geometry()) as usize;
                let stride = width + 3;
                let (serial, serial_special) = run(
                    &snapshot,
                    border,
                    IndexedLayout { stride, format },
                    None,
                    &tiles,
                    &tiles,
                    SpriteList::Presented,
                );
                let (parallel, parallel_special) = run(
                    &snapshot,
                    border,
                    IndexedLayout { stride, format },
                    Some(compose_workers()),
                    &tiles,
                    &tiles,
                    SpriteList::Presented,
                );
                assert_eq!(serial, parallel, "border {border} {format:?}");
                assert_eq!(serial_special, parallel_special);
            }
        }
    }
}
