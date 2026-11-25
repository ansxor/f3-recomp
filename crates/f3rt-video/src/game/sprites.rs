//! Sprite display-list decode, lagged latch and raster.
//! The old sprite plane seeds the next draw when the incoming frame enables trails.

use crate::config::{GameVideoOptions, Geometry, NATIVE_WIDTH, VideoConfig};
use crate::decode::{DecodedSpriteEntry, SpritePresentation, SpriteRamState, decode_sprite_list};
use crate::game::row_bits::{PenRow, extract_columns, for_each_run, set_columns};
use crate::game::row_workers::COMPOSE_CHUNK_ROWS;
use crate::game::scene::{
    LayerId, MAX_HARDWARE_SPRITES, SceneSprite, VideoRam, sprite_layer, wrap_tile_index,
};
use crate::memory_map::OFFS_SPRITERAM;
use crate::state_io::{CanonicalRecord, CanonicalSceneSprite, StateReader, StateWriter};

/// Native sprite-plane width (`36 + 366` scanout columns).
pub const SPRITE_PLANE_WIDTH: usize = 432;
/// Native sprite-plane rows.
pub const SPRITE_PLANE_ROWS: usize = 256;
/// Native pixels in one sprite plane.
pub const TRAIL_PLANE_PIXELS: usize = SPRITE_PLANE_WIDTH * SPRITE_PLANE_ROWS;
/// Serialized trail-plane bytes (`GameSprites::TRAIL_STATE_SIZE`).
pub const TRAIL_STATE_SIZE: usize = TRAIL_PLANE_PIXELS * 2;

/// `GameSprites::state_size()`: three canonical sprite lists plus the six flag bytes.
pub const SPRITE_STATE_SIZE: usize =
    CanonicalSceneSprite::SIZE * MAX_HARDWARE_SPRITES * 3 + 4 * 3 + 3 + 4;

/// Flash-buffered plane size for a raster request: expanded presentations span the scaled window.
pub fn sprite_plane_size(options: &GameVideoOptions, geometry: &Geometry) -> usize {
    if options.expanded() {
        (options.width(geometry) * options.height(geometry)) as usize
    } else {
        TRAIL_PLANE_PIXELS
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SpriteTrails {
    Off,
    On,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SpriteLatch {
    Unchanged,
    Latched,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SpriteRasterOptions {
    pub geometry: GameVideoOptions,
    pub flipped: bool,
}

/// `scene_sprite`: the serialized-canonical view of one decoded display-list entry.
pub fn scene_sprite(entry: &DecodedSpriteEntry) -> SceneSprite {
    SceneSprite {
        x: entry.x,
        y: entry.y,
        scale_x: entry.scale_x as u16,
        scale_y: entry.scale_y as u16,
        tile: entry.tile,
        palette: entry.color,
        flip_x: entry.flip_x,
        flip_y: entry.flip_y,
        group: entry.group,
        group_x: entry.group_x,
        group_y: entry.group_y,
        group_pc: entry.group_pc,
        blend_weight: 1.0,
    }
}

/// Layers a sprite plane can use (`SpriteCoverage::LAYERS`).
pub const SPRITE_COVERAGE_LAYERS: usize = 4;

/// `SpriteCoverage::words_for`: packed columns per layer row, including the span guard word.
pub fn sprite_coverage_words(options: &GameVideoOptions, geometry: &Geometry) -> usize {
    (options.width(geometry) as usize).div_ceil(32) + 1
}

/// Bit coverage of a 16-pixel pen row (`SpriteCoverage::Row`).
#[derive(Clone, Copy, Debug)]
pub struct SpriteCoverageRow<'a> {
    layers: &'a u8,
    bits: &'a [u32],
    words: usize,
}

impl<'a> SpriteCoverageRow<'a> {
    pub fn layers(&self) -> u8 {
        *self.layers
    }

    pub fn covers(&self, layer: usize) -> bool {
        (*self.layers >> layer) & 1 != 0
    }

    pub fn word_count(&self) -> usize {
        self.words
    }

    pub fn columns(&self, layer: usize) -> &'a [u32] {
        &self.bits[layer * self.words..(layer + 1) * self.words]
    }

    pub fn covered_word(&self, word: usize) -> u32 {
        let mut held = 0;
        let mut mask = *self.layers;
        while mask != 0 {
            let layer = mask.trailing_zeros() as usize;
            held |= self.bits[layer * self.words + word];
            mask &= mask - 1;
        }
        held
    }

    pub fn covered(&self, column: u32, len: u32) -> u32 {
        let mut held = 0;
        let mut mask = *self.layers;
        while mask != 0 {
            let layer = mask.trailing_zeros() as usize;
            held |= extract_columns(self.columns(layer), column as i32, len);
            mask &= mask - 1;
        }
        held
    }
}

/// Mutable coverage row used while rasterizing (`SpriteCoverage::Row`).
pub struct SpriteCoverageRowMut<'a> {
    layers: &'a mut u8,
    bits: &'a mut [u32],
    words: usize,
}

impl<'a> SpriteCoverageRowMut<'a> {
    pub fn layers(&self) -> u8 {
        *self.layers
    }

    pub fn covers(&self, layer: usize) -> bool {
        (*self.layers >> layer) & 1 != 0
    }

    pub fn columns(&self, layer: usize) -> &[u32] {
        &self.bits[layer * self.words..(layer + 1) * self.words]
    }

    pub fn covered_word(&self, word: usize) -> u32 {
        let mut held = 0;
        let mut mask = *self.layers;
        while mask != 0 {
            let layer = mask.trailing_zeros() as usize;
            held |= self.bits[layer * self.words + word];
            mask &= mask - 1;
        }
        held
    }

    pub fn covered(&self, column: u32, len: u32) -> u32 {
        let mut held = 0;
        let mut mask = *self.layers;
        while mask != 0 {
            let layer = mask.trailing_zeros() as usize;
            held |= extract_columns(self.columns(layer), column as i32, len);
            mask &= mask - 1;
        }
        held
    }

    pub fn mark(&mut self, layer: usize, column: u32, bits: u32) {
        *self.layers |= 1u8 << layer;
        set_columns(
            &mut self.bits[layer * self.words..(layer + 1) * self.words],
            column as i32,
            bits,
        );
    }

    pub fn mark_word(&mut self, layer: usize, word: usize, bits: u32) {
        *self.layers |= 1u8 << layer;
        self.bits[layer * self.words + word] |= bits;
    }

    pub fn clear(&mut self) {
        *self.layers = 0;
        self.bits[..self.words * SPRITE_COVERAGE_LAYERS].fill(0);
    }
}

/// Packed sprite-plane coverage for one raster pass (`SpriteCoverage`).
pub struct SpriteCoverage<'a> {
    words: usize,
    layers: &'a mut [u8],
    bits: &'a mut [u32],
}

impl<'a> SpriteCoverage<'a> {
    pub const LAYERS: usize = SPRITE_COVERAGE_LAYERS;

    pub fn new(
        options: &GameVideoOptions,
        geometry: &Geometry,
        layers: &'a mut [u8],
        bits: &'a mut [u32],
    ) -> Self {
        let words = sprite_coverage_words(options, geometry);
        assert!(
            bits.len() >= layers.len() * Self::LAYERS * words,
            "sprite coverage bit storage too small"
        );
        Self {
            words,
            layers,
            bits,
        }
    }

    pub fn words_for(options: &GameVideoOptions, geometry: &Geometry) -> usize {
        sprite_coverage_words(options, geometry)
    }

    pub fn rows(&self) -> usize {
        self.layers.len()
    }

    pub fn row(&self, plane_row: usize) -> SpriteCoverageRow<'_> {
        let words = self.words;
        SpriteCoverageRow {
            layers: &self.layers[plane_row],
            bits: &self.bits[plane_row * Self::LAYERS * words..],
            words,
        }
    }

    pub fn row_mut(&mut self, plane_row: usize) -> SpriteCoverageRowMut<'_> {
        let words = self.words;
        SpriteCoverageRowMut {
            layers: &mut self.layers[plane_row],
            bits: &mut self.bits[plane_row * Self::LAYERS * words..],
            words,
        }
    }
}

/// Read-only coverage view for composition (`SpriteCoverage` rows after rasterizing).
#[derive(Clone, Copy, Debug)]
pub struct SpriteCoverageView<'a> {
    words: usize,
    layers: &'a [u8],
    bits: &'a [u32],
}

impl<'a> SpriteCoverageView<'a> {
    pub fn new(
        options: &GameVideoOptions,
        geometry: &Geometry,
        layers: &'a [u8],
        bits: &'a [u32],
    ) -> Self {
        let words = sprite_coverage_words(options, geometry);
        assert!(
            bits.len() >= layers.len() * SPRITE_COVERAGE_LAYERS * words,
            "sprite coverage bit storage too small"
        );
        Self {
            words,
            layers,
            bits,
        }
    }

    pub fn row(&self, plane_row: usize) -> SpriteCoverageRow<'_> {
        SpriteCoverageRow {
            layers: &self.layers[plane_row],
            bits: &self.bits[plane_row * SPRITE_COVERAGE_LAYERS * self.words..],
            words: self.words,
        }
    }
}

#[derive(Clone, Copy)]
struct RasterGeometry {
    options: GameVideoOptions,
    tile_count: u32,
    scale: i32,
    width: i32,
    origin_x: i32,
    origin_y: i32,
    left: i32,
    right: i32,
    top: i32,
    bottom: i32,
    y_bias: i32,
    first_line: i32,
    expanded: bool,
    /// Absolute plane row the `plane` slice starts at (chunked raster); 0 for a full plane.
    row_base: i32,
}

fn make_geometry(
    assets: usize,
    geometry: &Geometry,
    raster: SpriteRasterOptions,
) -> RasterGeometry {
    let options = raster.geometry;
    let expanded = options.expanded();
    let scale = options.scale as i32;
    let width = if expanded {
        options.width(geometry) as i32
    } else {
        SPRITE_PLANE_WIDTH as i32
    };
    RasterGeometry {
        options,
        tile_count: (assets / 256) as u32,
        scale,
        width,
        origin_x: if expanded {
            46 - options.border as i32
        } else {
            0
        },
        origin_y: if expanded {
            geometry.first_line() as i32
        } else {
            0
        },
        left: if expanded { 0 } else { 46 },
        right: if expanded { width } else { 366 },
        top: if expanded {
            0
        } else {
            geometry.first_line() as i32
        },
        bottom: if expanded {
            options.height(geometry) as i32
        } else {
            geometry.end_line() as i32
        },
        y_bias: if raster.flipped { 0 } else { 255 },
        first_line: geometry.first_line() as i32,
        expanded,
        row_base: 0,
    }
}

/// First and last (exclusive) destination row of one texel row; empty when `first >= last`.
fn texel_rows(sprite: &SceneSprite, g: &RasterGeometry, y: i32) -> (i32, i32) {
    let position_y = (sprite.y + y * i32::from(sprite.scale_y)) * g.scale + g.y_bias;
    let start_y = (position_y >> 8) - g.origin_y * g.scale;
    let end_y = std::cmp::max(
        start_y + 1,
        ((position_y + i32::from(sprite.scale_y) * g.scale) >> 8) - g.origin_y * g.scale,
    );
    (
        std::cmp::max(g.top, start_y),
        std::cmp::min(g.bottom, end_y),
    )
}

fn draw_aligned_sprite<const UNIT_ROWS: bool>(
    sprite: &SceneSprite,
    pixels: &[u8],
    plane: &mut [u16],
    g: &RasterGeometry,
    mask: u8,
    mut coverage: Option<&mut SpriteCoverage<'_>>,
) {
    let base_x = ((sprite.x + 128) >> 8) - g.origin_x;
    let first_x = std::cmp::max(0, g.left - base_x);
    let last_x = std::cmp::min(16, g.right - base_x);
    if first_x >= last_x {
        return;
    }
    let color_base = 0x1000 + (u16::from(sprite.palette) << 4);
    let layer = (sprite_layer(color_base) as u32 - LayerId::Sp0 as u32) as usize;
    let window_width = (last_x - first_x) as u32;
    let window = (1u32 << window_width) - 1;
    let column = (base_x + first_x - g.left) as u32;
    let base_y = if UNIT_ROWS {
        ((sprite.y + g.y_bias) >> 8) - g.origin_y
    } else {
        0
    };
    let first_y = if UNIT_ROWS {
        std::cmp::max(0, g.top - base_y)
    } else {
        0
    };
    let last_y = if UNIT_ROWS {
        std::cmp::min(16, g.bottom - base_y)
    } else {
        16
    };
    for y in first_y..last_y {
        let rows = if UNIT_ROWS {
            (base_y + y, base_y + y + 1)
        } else {
            texel_rows(sprite, g, y)
        };
        if rows.0 >= rows.1 {
            continue;
        }
        let texel_base = (y ^ if sprite.flip_y { 15 } else { 0 }) as usize * 16;
        let texels = &pixels[texel_base..texel_base + 16];
        let mut pens: Option<PenRow> = None;
        let mut opaque = 0u32;
        for row in rows.0..rows.1 {
            let local_row = (row - g.row_base) as usize;
            let line_base = (row - g.row_base) * g.width + base_x + first_x;
            let claimed = if let Some(coverage) = coverage.as_deref() {
                coverage.row(local_row).covered(column, window_width)
            } else {
                let mut claimed = 0u32;
                for i in 0..window_width {
                    let destination = plane[(line_base + i as i32) as usize];
                    claimed |= u32::from(destination != 0) << i;
                }
                claimed
            };
            if claimed == window {
                continue;
            }
            if pens.is_none() {
                let pen_row = PenRow::new(texels, mask);
                opaque = if pen_row.blank() {
                    0
                } else {
                    (pen_row.opaque(sprite.flip_x) >> first_x) & window
                };
                pens = Some(pen_row);
            }
            if opaque == 0 {
                break;
            }
            let fresh = opaque & !claimed;
            if fresh == 0 {
                continue;
            }
            if let Some(coverage) = coverage.as_deref_mut() {
                coverage.row_mut(local_row).mark(layer, column, fresh);
            }
            let columns = fresh << first_x;
            let pens = pens.as_ref().expect("bound above");
            for group in 0..4usize {
                let nibble = columns >> (4 * group) & 0xf;
                if nibble == 0 {
                    continue;
                }
                let four = pens.group(group, sprite.flip_x);
                let offset = (4 * group) as i32 - first_x;
                if nibble == 0xf {
                    for (i, shift) in (0..4u32).enumerate() {
                        let value = color_base | ((four >> (8 * shift)) & 0xff) as u16;
                        plane[(line_base + offset + i as i32) as usize] = value;
                    }
                } else {
                    let mut rest = nibble;
                    while rest != 0 {
                        let i = rest.trailing_zeros();
                        let value = color_base | ((four >> (8 * i)) & 0xff) as u16;
                        plane[(line_base + offset + i as i32) as usize] = value;
                        rest &= rest - 1;
                    }
                }
            }
        }
    }
}

fn draw_scaled_sprite_columns(
    sprite: &SceneSprite,
    pixels: &[u8],
    plane: &mut [u16],
    g: &RasterGeometry,
    mask: u8,
    columns: &[(i32, i32, u32)],
) {
    for y in 0..16i32 {
        let rows = texel_rows(sprite, g, y);
        if rows.0 >= rows.1 {
            continue;
        }
        let row_base = (y ^ if sprite.flip_y { 15 } else { 0 }) as usize * 16;
        let row = &pixels[row_base..row_base + 16];
        for &(column_left, column_right, x) in columns {
            let pen = row[(x ^ if sprite.flip_x { 15 } else { 0 }) as usize] & mask;
            if pen == 0 {
                continue;
            }
            let color = 0x1000 | (u16::from(sprite.palette) << 4) | u16::from(pen);
            for dy in rows.0..rows.1 {
                let line = (dy - g.row_base) * g.width;
                for dx in column_left + g.left..column_right + g.left {
                    let destination = &mut plane[(line + dx) as usize];
                    if *destination == 0 {
                        *destination = color;
                    }
                }
            }
        }
    }
}

fn bit_range(lo: u32, hi: u32) -> u32 {
    (if hi == 32 { u32::MAX } else { (1u32 << hi) - 1 }) & !((1u32 << lo) - 1)
}

/// `SpriteCoverage::words_for(GameVideoOptions{1, MAX_BORDER})`: the widest chunked coverage row.
const MAX_COVERAGE_WORDS: usize =
    (NATIVE_WIDTH as usize + GameVideoOptions::MAX_BORDER as usize * 2).div_ceil(32) + 1;

fn draw_scaled_sprite_covered(
    sprite: &SceneSprite,
    pixels: &[u8],
    plane: &mut [u16],
    g: &RasterGeometry,
    mask: u8,
    coverage: &mut SpriteCoverage<'_>,
    columns: &[(i32, i32, u32)],
) {
    let color_base = 0x1000 + (u16::from(sprite.palette) << 4);
    let span_left = columns.first().expect("non-empty columns").0 as u32;
    let span_right = columns.last().expect("non-empty columns").1 as u32;
    let first_word = span_left / 32;
    let last_word = (span_right - 1) / 32;
    let mut held = [0u32; MAX_COVERAGE_WORDS];
    for y in 0..16i32 {
        let rows = texel_rows(sprite, g, y);
        if rows.0 >= rows.1 {
            continue;
        }
        let texel_base = (y ^ if sprite.flip_y { 15 } else { 0 }) as usize * 16;
        let pens = PenRow::new(&pixels[texel_base..texel_base + 16], mask);
        if pens.blank() {
            continue;
        }
        let opaque = pens.opaque(sprite.flip_x);
        for row in rows.0..rows.1 {
            let local_row = (row - g.row_base) as usize;
            let cover = coverage.row(local_row);
            let mut open = 0u32;
            for word in first_word..=last_word {
                let any = cover.covered_word(word as usize);
                held[(word - first_word) as usize] = any;
                open |= bit_range(
                    if word == first_word {
                        span_left % 32
                    } else {
                        0
                    },
                    if word == last_word {
                        (span_right - 1) % 32 + 1
                    } else {
                        32
                    },
                ) & !any;
            }
            if open == 0 {
                continue;
            }
            let line_base = (row - g.row_base) * g.width + g.left;
            for &(column_left, column_right, x) in columns {
                if (opaque >> x) & 1 == 0 {
                    continue;
                }
                let color = color_base
                    | ((pens.group((x / 4) as usize, sprite.flip_x) >> (8 * (x % 4))) & 0xff)
                        as u16;
                let layer = (sprite_layer(color) as u32 - LayerId::Sp0 as u32) as usize;
                let first = column_left as u32;
                let last = column_right as u32;
                for word in first / 32..=(last - 1) / 32 {
                    let fresh = bit_range(
                        if word == first / 32 { first % 32 } else { 0 },
                        if word == (last - 1) / 32 {
                            (last - 1) % 32 + 1
                        } else {
                            32
                        },
                    ) & !held[(word - first_word) as usize];
                    if fresh == 0 {
                        continue;
                    }
                    let mut cover = coverage.row_mut(local_row);
                    cover.mark_word(layer, word as usize, fresh);
                    for_each_run(fresh, |begin, length| {
                        let at = (line_base + (word * 32 + begin) as i32) as usize;
                        plane[at..at + length as usize].fill(color);
                    });
                }
            }
        }
    }
}

fn draw_sprite(
    sprite: &SceneSprite,
    assets: &[u8],
    plane: &mut [u16],
    g: &RasterGeometry,
    mask: u8,
    coverage: Option<&mut SpriteCoverage<'_>>,
) {
    if sprite.x + i32::from(sprite.scale_x) * 16 <= (46 - g.options.border as i32) * 256
        || sprite.x > (365 + g.options.border as i32) * 256
        || sprite.y + i32::from(sprite.scale_y) * 16 <= g.first_line * 256
        || sprite.y > 255 * 256
    {
        return;
    }
    let tile = wrap_tile_index(sprite.tile, g.tile_count) as usize;
    let pixels = &assets[tile * 256..tile * 256 + 256];
    if g.scale == 1 && sprite.scale_x == 256 {
        if sprite.scale_y == 256 {
            draw_aligned_sprite::<true>(sprite, pixels, plane, g, mask, coverage);
        } else {
            draw_aligned_sprite::<false>(sprite, pixels, plane, g, mask, coverage);
        }
        return;
    }
    let mut columns = [(0i32, 0i32, 0u32); 16];
    let column_count = sprite_columns(sprite, g, &mut columns);
    if column_count == 0 {
        return;
    }
    if let Some(coverage) = coverage {
        draw_scaled_sprite_covered(
            sprite,
            pixels,
            plane,
            g,
            mask,
            coverage,
            &columns[..column_count],
        );
        return;
    }
    draw_scaled_sprite_columns(sprite, pixels, plane, g, mask, &columns[..column_count]);
}

/// Per-column destination spans for a scaled sprite.
fn sprite_columns(
    sprite: &SceneSprite,
    g: &RasterGeometry,
    columns: &mut [(i32, i32, u32)],
) -> usize {
    let mut column_count = 0usize;
    for x in 0..16i32 {
        let position_x = (sprite.x + x * i32::from(sprite.scale_x)) * g.scale + 128;
        let start_x = (position_x >> 8) - g.origin_x * g.scale;
        let end_x =
            ((position_x + i32::from(sprite.scale_x) * g.scale) >> 8) - g.origin_x * g.scale;
        if start_x == end_x || end_x <= g.left || start_x >= g.right {
            continue;
        }
        columns[column_count] = (
            std::cmp::max(g.left, start_x) - g.left,
            std::cmp::min(g.right, end_x) - g.left,
            x as u32,
        );
        column_count += 1;
    }
    column_count
}

/// Rasterize `sprites` back-to-front into `output`; with `trails` off the plane is cleared first.
pub fn raster_sprites(
    sprites: &[SceneSprite],
    pen_mask: u8,
    assets: &[u8],
    output: &mut [u16],
    geometry: &Geometry,
    raster: SpriteRasterOptions,
    trails: SpriteTrails,
) {
    if trails == SpriteTrails::Off {
        output.fill(0);
    }
    if assets.is_empty() {
        return;
    }
    let g = make_geometry(assets.len(), geometry, raster);
    for sprite in sprites.iter().rev() {
        draw_sprite(sprite, assets, output, &g, pen_mask, None);
    }
}

/// Sprites bucketed per composition chunk (`SpriteBins`).
#[derive(Clone, Debug, Default)]
pub struct SpriteBins {
    chunks: Vec<Vec<u16>>,
}

impl SpriteBins {
    pub fn bin(
        &mut self,
        sprites: &[SceneSprite],
        geometry: &Geometry,
        raster: SpriteRasterOptions,
    ) {
        let g = make_geometry(0, geometry, raster);
        let chunks =
            (g.bottom.max(0) as usize - g.top.max(0) as usize).div_ceil(COMPOSE_CHUNK_ROWS);
        if self.chunks.len() != chunks {
            self.chunks.resize(chunks, Vec::new());
        }
        for bucket in &mut self.chunks {
            bucket.clear();
        }
        for (i, sprite) in sprites.iter().enumerate() {
            let start = ((sprite.y * g.scale + g.y_bias) >> 8) - g.origin_y * g.scale - 1;
            let end = (((sprite.y + 16 * i32::from(sprite.scale_y)) * g.scale + g.y_bias) >> 8)
                - g.origin_y * g.scale
                + 1;
            let first = std::cmp::max(g.top, start);
            let last = std::cmp::min(g.bottom, end);
            if first >= last {
                continue;
            }
            let begin_chunk = (first as usize - g.top as usize) / COMPOSE_CHUNK_ROWS;
            let end_chunk = (last as usize - g.top as usize) / COMPOSE_CHUNK_ROWS;
            for chunk in begin_chunk..=end_chunk {
                if chunk < chunks {
                    self.chunks[chunk].push(i as u16);
                }
            }
        }
    }

    pub fn chunk_count(&self) -> usize {
        self.chunks.len()
    }

    pub fn chunk(&self, chunk: usize) -> &[u16] {
        &self.chunks[chunk]
    }
}

/// Rasterize one chunk into a chunk-local plane and coverage (`raster_sprite_chunk`).
///
/// `plane` starts at absolute plane row `g.top + chunk * COMPOSE_CHUNK_ROWS`; coverage is the
/// matching row range. Row arithmetic stays absolute so the formulas match the full-plane path.
pub(super) struct SpriteRaster<'a> {
    pub sprites: &'a [SceneSprite],
    pub pen_mask: u8,
    pub tiles: &'a [u8],
    pub trail_seed: &'a [u16],
}

impl SpriteRaster<'_> {
    pub fn draw_chunk(
        &self,
        bins: &SpriteBins,
        chunk: usize,
        plane: &mut [u16],
        geometry: &Geometry,
        raster: SpriteRasterOptions,
        coverage: &mut SpriteCoverage<'_>,
    ) {
        let mut g = make_geometry(self.tiles.len(), geometry, raster);
        let first = (g.top.max(0) as usize) + chunk * COMPOSE_CHUNK_ROWS;
        let last = std::cmp::min(
            (g.top.max(0) as usize) + (chunk + 1) * COMPOSE_CHUNK_ROWS,
            g.bottom.max(0) as usize,
        );
        if first >= last {
            return;
        }
        g.row_base = first as i32;
        for row in first..last {
            let local_row = row - first;
            coverage.row_mut(local_row).clear();
            if self.trail_seed.is_empty() {
                continue;
            }
            let native_y = if g.expanded {
                row + geometry.first_line() as usize
            } else {
                row
            };
            for x in g.left..g.right {
                let native_x = x + g.origin_x;
                let color = if (46..366).contains(&native_x) {
                    self.trail_seed[native_y * SPRITE_PLANE_WIDTH + native_x as usize]
                } else {
                    0
                };
                plane[local_row * g.width as usize + x as usize] = color;
                if color != 0 {
                    coverage.row_mut(local_row).mark(
                        (sprite_layer(color) as u32 - LayerId::Sp0 as u32) as usize,
                        (x - g.left) as u32,
                        1,
                    );
                }
            }
        }
        if self.tiles.is_empty() {
            return;
        }
        let bucket = bins.chunk(chunk);
        let windowed = RasterGeometry {
            top: std::cmp::max(g.top, first as i32),
            bottom: std::cmp::min(g.bottom, last as i32),
            ..g
        };
        for index in bucket.iter().rev() {
            draw_sprite(
                &self.sprites[*index as usize],
                self.tiles,
                plane,
                &windowed,
                self.pen_mask,
                Some(coverage),
            );
        }
    }
}

pub struct GameSprites {
    video: VideoConfig,
    geometry: Geometry,
    staging: Vec<SceneSprite>,
    staging_count: usize,
    submitted: Vec<SceneSprite>,
    submitted_count: usize,
    current: Vec<SceneSprite>,
    current_count: usize,
    current_flipped: bool,
    current_pen_mask: u8,
    current_trails: bool,
    /// The framebuffer drawn with the previous list, seeding the next trails draw.
    trail_seed: Vec<u16>,
    reg_flipped: bool,
    reg_pen_mask: u8,
    reg_trails: bool,
    reg_bank: bool,
    decoded: Vec<DecodedSpriteEntry>,
}

impl GameSprites {
    pub fn new(video: &VideoConfig) -> Self {
        let mut sprites = Self {
            video: *video,
            geometry: Geometry::from_video(video),
            staging: vec![SceneSprite::default(); MAX_HARDWARE_SPRITES],
            staging_count: 0,
            submitted: vec![SceneSprite::default(); MAX_HARDWARE_SPRITES],
            submitted_count: 0,
            current: vec![SceneSprite::default(); MAX_HARDWARE_SPRITES],
            current_count: 0,
            current_flipped: false,
            current_pen_mask: 15,
            current_trails: false,
            trail_seed: vec![0; TRAIL_PLANE_PIXELS],
            reg_flipped: false,
            reg_pen_mask: 15,
            reg_trails: false,
            reg_bank: false,
            decoded: vec![DecodedSpriteEntry::default(); MAX_HARDWARE_SPRITES],
        };
        sprites.reset();
        sprites
    }

    pub fn reset(&mut self) {
        self.staging_count = 0;
        self.submitted_count = 0;
        self.current_count = 0;
        self.current_flipped = false;
        self.current_pen_mask = 15;
        self.current_trails = false;
        self.trail_seed.fill(0);

        self.reg_flipped = false;
        self.reg_pen_mask = 15;
        self.reg_trails = false;
        self.reg_bank = false;
    }

    /// Decode the sprite display list from VRAM into the next submission.
    pub fn decode(&mut self, vram: &VideoRam<'_>, presentation: SpritePresentation<'_>) {
        let mut state = SpriteRamState {
            flipscreen: self.reg_flipped,
            bank: self.reg_bank,
            trails: self.reg_trails,
            extra_planes: self.reg_pen_mask >> 4,
            pen_mask: self.reg_pen_mask,
        };

        let count = decode_sprite_list(
            &vram.graphics[OFFS_SPRITERAM..],
            self.geometry.first_line() as i32..self.geometry.end_line() as i32,
            &mut self.decoded,
            &mut state,
            true,
            (!presentation.empty()).then_some(&presentation),
        );

        for ((staged, submitted), entry) in self
            .staging
            .iter_mut()
            .zip(&mut self.submitted)
            .zip(&self.decoded[..count])
        {
            let sprite = scene_sprite(entry);
            *staged = sprite;
            *submitted = sprite;
        }
        self.submitted_count = count;
        self.staging_count = 0;

        self.reg_flipped = state.flipscreen;
        self.reg_pen_mask = state.pen_mask;
        self.reg_trails = state.trails;
        self.reg_bank = state.bank;
    }

    pub fn latch(&mut self, assets: &[u8]) {
        // The framebuffer drawn with the old list and flags seeds the next draw.
        // Reconstruct it only when the incoming draw will retain its pixels.
        if self.reg_trails {
            let count = self.current_count;
            let trails = if self.current_trails {
                SpriteTrails::On
            } else {
                SpriteTrails::Off
            };
            let geometry = self.geometry;
            raster_sprites(
                &self.current[..count],
                self.current_pen_mask,
                assets,
                &mut self.trail_seed,
                &geometry,
                SpriteRasterOptions {
                    geometry: GameVideoOptions::default(),
                    flipped: self.current_flipped,
                },
                trails,
            );
        } else if self.current_trails {
            self.trail_seed.fill(0);
        }
        self.current_flipped = self.reg_flipped;
        self.current_pen_mask = self.reg_pen_mask;
        self.current_trails = self.reg_trails;
        self.current_count = self.submitted_count;

        self.current[..self.current_count].copy_from_slice(&self.submitted[..self.current_count]);
    }

    pub fn raster(&self, assets: &[u8], output: &mut [u16], options: GameVideoOptions) {
        raster_sprites(
            self.sprites(),
            self.pen_mask(),
            assets,
            output,
            &self.geometry,
            SpriteRasterOptions {
                geometry: options,
                flipped: self.flipped(),
            },
            if self.trails() {
                SpriteTrails::On
            } else {
                SpriteTrails::Off
            },
        );
    }

    /// The trail plane, empty unless trails are active.
    pub fn trail_seed(&self) -> &[u16] {
        if self.trails() { &self.trail_seed } else { &[] }
    }

    pub fn sprites(&self) -> &[SceneSprite] {
        &self.current[..self.current_count]
    }

    pub fn presented_sprites(&self) -> &[SceneSprite] {
        self.sprites()
    }

    pub fn flipped(&self) -> bool {
        self.current_flipped
    }

    pub fn scanout_flipped(&self) -> bool {
        if self.video.sprite_lag == 2 {
            self.reg_flipped
        } else {
            self.current_flipped
        }
    }

    pub fn before_scanout(
        &mut self,
        vram: &VideoRam<'_>,
        presentation: SpritePresentation<'_>,
        assets: &[u8],
    ) -> SpriteLatch {
        if self.video.sprite_lag != 2 {
            self.decode(vram, presentation);
        }
        if self.video.sprite_lag == 0 {
            self.latch(assets);
            return SpriteLatch::Latched;
        }
        SpriteLatch::Unchanged
    }

    pub fn after_scanout(
        &mut self,
        vram: &VideoRam<'_>,
        presentation: SpritePresentation<'_>,
        assets: &[u8],
    ) -> SpriteLatch {
        if self.video.sprite_lag != 0 {
            self.latch(assets);
        }
        if self.video.sprite_lag == 2 {
            self.decode(vram, presentation);
        }
        if self.video.sprite_lag != 0 {
            SpriteLatch::Latched
        } else {
            SpriteLatch::Unchanged
        }
    }

    /// Which list scanout presents: trails keep the canonical list, enhancements (when present)
    /// supply their own presented list.
    pub fn presented_for<'a>(
        &'a self,
        enhancements: Option<&'a [SceneSprite]>,
    ) -> &'a [SceneSprite] {
        if self.trails() {
            return self.sprites();
        }
        match enhancements {
            Some(sprites) if !sprites.is_empty() => sprites,
            _ => self.presented_sprites(),
        }
    }

    pub fn presented_valid(&self) -> bool {
        false
    }

    pub fn presented_overflow(&self) -> bool {
        false
    }

    pub fn pen_mask(&self) -> u8 {
        self.current_pen_mask
    }

    pub fn trails(&self) -> bool {
        self.current_trails
    }

    pub fn state_size(&self) -> usize {
        SPRITE_STATE_SIZE
    }

    pub fn save_state(&self, writer: &mut StateWriter<'_>) -> Result<(), String> {
        writer.write_u32(self.staging_count as u32)?;
        for sprite in &self.staging {
            writer.write_canonical(&pack_sprite(sprite))?;
        }
        writer.write_u32(self.submitted_count as u32)?;
        for sprite in &self.submitted {
            writer.write_canonical(&pack_sprite(sprite))?;
        }
        writer.write_u32(self.current_count as u32)?;
        for sprite in &self.current {
            writer.write_canonical(&pack_sprite(sprite))?;
        }
        writer.write_u8(u8::from(self.current_flipped))?;
        writer.write_u8(self.current_pen_mask)?;
        writer.write_u8(u8::from(self.current_trails))?;
        writer.write_u8(u8::from(self.reg_flipped))?;
        writer.write_u8(self.reg_pen_mask)?;
        writer.write_u8(u8::from(self.reg_trails))?;
        writer.write_u8(u8::from(self.reg_bank))
    }

    pub fn load_state(&mut self, reader: &mut StateReader<'_>) -> Result<(), String> {
        let count = reader.read_u32()?;
        if count as usize > MAX_HARDWARE_SPRITES {
            return Err("Snapshot staging sprite count out of range".to_string());
        }
        self.staging_count = count as usize;
        for sprite in &mut self.staging {
            *sprite = unpack_sprite(&reader.read_canonical::<CanonicalSceneSprite>()?);
        }
        let count = reader.read_u32()?;
        if count as usize > MAX_HARDWARE_SPRITES {
            return Err("Snapshot submitted sprite count out of range".to_string());
        }
        self.submitted_count = count as usize;
        for sprite in &mut self.submitted {
            *sprite = unpack_sprite(&reader.read_canonical::<CanonicalSceneSprite>()?);
        }
        let count = reader.read_u32()?;
        if count as usize > MAX_HARDWARE_SPRITES {
            return Err("Snapshot current sprite count out of range".to_string());
        }
        self.current_count = count as usize;
        for sprite in &mut self.current {
            *sprite = unpack_sprite(&reader.read_canonical::<CanonicalSceneSprite>()?);
        }
        self.current_flipped = reader.read_u8()? != 0;
        self.current_pen_mask = reader.read_u8()?;
        self.current_trails = reader.read_u8()? != 0;
        self.reg_flipped = reader.read_u8()? != 0;
        self.reg_pen_mask = reader.read_u8()?;
        self.reg_trails = reader.read_u8()? != 0;
        self.reg_bank = reader.read_u8()? != 0;
        Ok(())
    }

    pub fn save_trails(&self, writer: &mut StateWriter<'_>) -> Result<(), String> {
        for value in &self.trail_seed {
            writer.write_u16(*value)?;
        }
        Ok(())
    }

    pub fn load_trails(&mut self, reader: &mut StateReader<'_>) -> Result<(), String> {
        for value in &mut self.trail_seed {
            *value = reader.read_u16()?;
        }
        Ok(())
    }
}

fn pack_sprite(sprite: &SceneSprite) -> CanonicalSceneSprite {
    CanonicalSceneSprite {
        x: sprite.x,
        y: sprite.y,
        scale_x: sprite.scale_x,
        scale_y: sprite.scale_y,
        tile: sprite.tile,
        palette: sprite.palette,
        flip_x: u8::from(sprite.flip_x),
        flip_y: u8::from(sprite.flip_y),
    }
}

fn unpack_sprite(canonical: &CanonicalSceneSprite) -> SceneSprite {
    let canonical = *canonical;
    SceneSprite {
        x: canonical.x,
        y: canonical.y,
        scale_x: canonical.scale_x,
        scale_y: canonical.scale_y,
        tile: canonical.tile,
        palette: canonical.palette,
        flip_x: canonical.flip_x != 0,
        flip_y: canonical.flip_y != 0,
        group: 0,
        group_x: 0,
        group_y: 0,
        group_pc: 0,
        blend_weight: 1.0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::memory_map::GRAPHICS_RAM_SIZE;

    const ENTRY_BYTES: usize = 16;
    const SPRITE_X: usize = 4;
    const SPRITE_Y: usize = 6;
    const SPRITE_COLOR: usize = 8;
    const SPRITE_COMMAND: usize = 10;
    const SPRITE_TERMINATOR: usize = 12;

    struct Sources {
        graphics: Vec<u8>,
        control: [u8; 0x20],
    }

    impl Sources {
        fn new() -> Self {
            Self {
                graphics: vec![0; GRAPHICS_RAM_SIZE],
                control: [0; 0x20],
            }
        }

        fn vram(&self) -> VideoRam<'_> {
            VideoRam::new(&self.graphics, &self.control, 0)
        }

        fn word(&mut self, offset: usize, value: u16) {
            self.graphics[offset] = (value >> 8) as u8;
            self.graphics[offset + 1] = value as u8;
        }
    }

    fn tile_assets() -> Vec<u8> {
        let mut assets = vec![0u8; 512];
        assets[256..].fill(0x1f);
        assets
    }

    fn sprite_list(sources: &mut Sources, y: u16) {
        sources.word(SPRITE_Y, 0x8000);
        sources.word(ENTRY_BYTES, 1);
        sources.word(ENTRY_BYTES + SPRITE_X, 46);
        sources.word(ENTRY_BYTES + SPRITE_Y, y);
        sources.word(ENTRY_BYTES + SPRITE_COLOR, 1);
        // A self-jump ends the list (jump target == its own slot).
        sources.word(2 * ENTRY_BYTES + SPRITE_TERMINATOR, 0x8002);
    }

    #[test]
    fn block_chaining_decodes_two_sprites() {
        let mut sources = Sources::new();
        sources.word(0, 1);
        sources.word(2, 0);
        sources.word(4, 50);
        sources.word(6, 30);
        sources.word(8, 0x0005);
        sources.word(ENTRY_BYTES, 2);
        sources.word(ENTRY_BYTES + 2, 0);
        sources.word(ENTRY_BYTES + 8, 0xf005);
        let mut sprites = GameSprites::new(&VideoConfig::default());
        sprites.decode(&sources.vram(), SpritePresentation::default());
        assert!(
            sprites.sprites().is_empty(),
            "decoded sprites are submitted, not latched"
        );
        sprites.latch(&[]);
        let list = sprites.sprites();
        assert_eq!(list.len(), 2);
        assert_eq!(list[0].x, 50 * 256);
        assert_eq!(list[0].y, 30 * 256);
        assert_eq!(list[1].x, (50 + 16) * 256);
        assert_eq!(list[1].y, (30 + 16) * 256);
        assert_eq!(list[0].tile, 1);
        assert_eq!(list[1].tile, 2);
        assert_eq!(list[0].palette, 5);
        assert_eq!(list[0].scale_x, 256);
        assert_eq!(list[0].scale_y, 256);
    }

    #[test]
    fn latch_timing_follows_sprite_lag() {
        for (lag, before, after, visible_before) in [
            (0u32, SpriteLatch::Latched, SpriteLatch::Unchanged, true),
            (1, SpriteLatch::Unchanged, SpriteLatch::Latched, false),
            (2, SpriteLatch::Unchanged, SpriteLatch::Latched, false),
        ] {
            let video = VideoConfig {
                sprite_lag: lag,
                ..VideoConfig::default()
            };
            let mut sources = Sources::new();
            sources.word(0, 1);
            sources.word(4, 50);
            sources.word(6, 30);
            let mut sprites = GameSprites::new(&video);
            assert_eq!(
                sprites.before_scanout(&sources.vram(), SpritePresentation::default(), &[]),
                before,
                "lag {lag} before_scanout"
            );
            assert_eq!(
                sprites.sprites().len(),
                usize::from(visible_before),
                "lag {lag} visibility after before_scanout"
            );
            assert_eq!(
                sprites.after_scanout(&sources.vram(), SpritePresentation::default(), &[]),
                after,
                "lag {lag} after_scanout"
            );
            if lag == 2 {
                // Lag 2 latches the previous submission, then decodes the new one.
                assert!(sprites.sprites().is_empty());
                sprites.after_scanout(&sources.vram(), SpritePresentation::default(), &[]);
                assert_eq!(sprites.sprites().len(), 1);
            } else {
                assert_eq!(
                    sprites.sprites().len(),
                    1,
                    "lag {lag} list is latched after scanout"
                );
            }
        }
    }

    #[test]
    fn trails_retain_the_previous_plane_and_clear_when_disabled() {
        let video = VideoConfig::default();
        let geometry = Geometry::from_video(&video);
        let y = geometry.first_line();
        let mut sources = Sources::new();
        sprite_list(&mut sources, y as u16);
        let assets = tile_assets();
        let mut sprites = GameSprites::new(&video);

        let submit = |sprites: &mut GameSprites, sources: &mut Sources, x: u16, trails: bool| {
            sources.word(SPRITE_COMMAND, if trails { 0x0102 } else { 0x0100 });
            sources.word(ENTRY_BYTES + SPRITE_X, x);
            sprites.decode(&sources.vram(), SpritePresentation::default());
            sprites.latch(&assets);
        };

        submit(&mut sprites, &mut sources, 46, false);
        assert!(!sprites.trails());
        assert!(sprites.trail_seed().is_empty(), "no trails, no seed");
        let mut previous = vec![0u16; TRAIL_PLANE_PIXELS];
        sprites.raster(&assets, &mut previous, GameVideoOptions::default());
        let row = y as usize * SPRITE_PLANE_WIDTH;
        assert_ne!(
            previous[row + 46],
            0,
            "the frame raster leaves the sprite visible"
        );

        submit(&mut sprites, &mut sources, 54, true);
        assert!(sprites.trails());
        let seed = sprites.trail_seed();
        assert_eq!(seed.len(), TRAIL_PLANE_PIXELS);
        assert!(
            seed == &previous[..],
            "the previous raster seeds the trail plane"
        );
        assert_eq!(
            seed[row + 100],
            0,
            "columns outside the previous sprite stay clear"
        );

        // Further trailing frames retain the plane (and draw the current list on top).
        submit(&mut sprites, &mut sources, 54, true);
        assert_eq!(
            sprites.trail_seed()[row + 46],
            previous[row + 46],
            "retained pixels survive another trailing latch"
        );
        assert_eq!(sprites.trail_seed()[row + 100], 0);

        submit(&mut sprites, &mut sources, 78, false);
        assert!(!sprites.trails());
        assert!(sprites.trail_seed().is_empty());
    }

    #[test]
    fn state_round_trips_with_exact_sizes() {
        let mut sources = Sources::new();
        sprite_list(&mut sources, 40);
        let assets = tile_assets();
        let mut source = GameSprites::new(&VideoConfig::default());
        source.decode(&sources.vram(), SpritePresentation::default());
        source.latch(&assets);
        assert_eq!(source.state_size(), 58387);
        assert_eq!(TRAIL_STATE_SIZE, 221184);

        let mut bytes = vec![0u8; source.state_size() + TRAIL_STATE_SIZE];
        let mut writer = StateWriter::new(&mut bytes);
        source.save_state(&mut writer).unwrap();
        source.save_trails(&mut writer).unwrap();
        assert_eq!(writer.remaining(), 0);

        let mut restored = GameSprites::new(&VideoConfig::default());
        let mut reader = StateReader::new(&bytes);
        restored.load_state(&mut reader).unwrap();
        restored.load_trails(&mut reader).unwrap();
        assert_eq!(reader.remaining(), 0);

        let mut copy = vec![0u8; source.state_size()];
        let mut writer = StateWriter::new(&mut copy);
        source.save_state(&mut writer).unwrap();
        let mut after = vec![0u8; restored.state_size()];
        let mut writer = StateWriter::new(&mut after);
        restored.save_state(&mut writer).unwrap();
        assert_eq!(after, copy);
        assert_eq!(restored.sprites().len(), source.sprites().len());
        assert_eq!(restored.sprites()[0], source.sprites()[0]);
    }

    #[test]
    fn trailing_plane_round_trips() {
        let mut seed = vec![0u16; TRAIL_PLANE_PIXELS];
        for (i, value) in seed.iter_mut().enumerate() {
            *value = (i % 0x1000) as u16;
        }
        let mut sprites = GameSprites::new(&VideoConfig::default());
        sprites.trail_seed.copy_from_slice(&seed);

        let mut bytes = vec![0u8; TRAIL_STATE_SIZE];
        let mut writer = StateWriter::new(&mut bytes);
        sprites.save_trails(&mut writer).unwrap();
        assert_eq!(writer.remaining(), 0);

        sprites.trail_seed.fill(0);
        let mut reader = StateReader::new(&bytes);
        sprites.load_trails(&mut reader).unwrap();
        assert_eq!(reader.remaining(), 0);
        assert_eq!(sprites.trail_seed, seed);
    }

    #[test]
    fn load_state_rejects_out_of_range_counts() {
        let mut sprites = GameSprites::new(&VideoConfig::default());
        let mut bytes = vec![0u8; sprites.state_size()];
        let mut writer = StateWriter::new(&mut bytes);
        sprites.save_state(&mut writer).unwrap();
        bytes[3] = 0xff; // staging count, little-endian u32
        bytes[2] = 0xff;
        let mut reader = StateReader::new(&bytes);
        assert_eq!(
            sprites.load_state(&mut reader).unwrap_err(),
            "Snapshot staging sprite count out of range"
        );
    }

    #[test]
    fn presented_list_keeps_group_tags_without_changing_serialized_state() {
        let mut sources = Sources::new();
        sources.graphics[1] = 1;
        sources.graphics[5] = 50;
        sources.graphics[7] = 30;

        let mut canonical = GameSprites::new(&VideoConfig::default());
        canonical.decode(&sources.vram(), SpritePresentation::default());
        canonical.latch(&[]);
        assert_eq!(canonical.sprites().len(), 1);
        assert_eq!(canonical.presented_sprites(), canonical.sprites());

        let groups = [crate::decode::SpriteGroupSlot {
            key: 42,
            x: 1,
            y: 2,
            pc: 3,
        }];
        let mut grouped = GameSprites::new(&VideoConfig::default());
        grouped.decode(&sources.vram(), SpritePresentation { groups: &groups });
        grouped.latch(&[]);

        assert_eq!(grouped.sprites().len(), 1);
        assert_eq!(grouped.sprites()[0].group, 42);

        let mut before = vec![0u8; canonical.state_size()];
        let mut writer = StateWriter::new(&mut before);
        canonical.save_state(&mut writer).unwrap();
        let mut after = vec![0u8; grouped.state_size()];
        let mut writer = StateWriter::new(&mut after);
        grouped.save_state(&mut writer).unwrap();
        assert_eq!(
            after, before,
            "presentation never reaches the canonical snapshot"
        );
    }

    #[test]
    fn presented_for_prefers_trails_then_enhancements() {
        let mut sources = Sources::new();
        sprite_list(&mut sources, 40);
        let mut sprites = GameSprites::new(&VideoConfig::default());
        sprites.decode(&sources.vram(), SpritePresentation::default());
        sprites.latch(&[]);
        assert_eq!(sprites.sprites().len(), 1);
        assert_eq!(sprites.presented_for(None).len(), 1);

        let extra = [SceneSprite::default(), SceneSprite::default()];
        assert_eq!(sprites.presented_for(Some(&extra)).len(), 2);
        assert_eq!(sprites.presented_for(Some(&[])).len(), 1);

        sources.word(SPRITE_COMMAND, 0x0102);
        sprites.decode(&sources.vram(), SpritePresentation::default());
        sprites.latch(&[]);
        assert!(sprites.trails());
        assert_eq!(
            sprites.presented_for(Some(&extra)).len(),
            1,
            "trails always present the canonical list"
        );
    }

    #[test]
    fn raster_draws_tiles_and_keeps_existing_pixels_with_trails() {
        let video = VideoConfig::default();
        let geometry = Geometry::from_video(&video);
        let mut sources = Sources::new();
        sprite_list(&mut sources, geometry.first_line() as u16);
        sources.word(ENTRY_BYTES + 4, 46);
        sources.word(ENTRY_BYTES + 8, 1);
        let mut sprites = GameSprites::new(&video);
        sprites.decode(&sources.vram(), SpritePresentation::default());
        sprites.latch(&[]);

        let assets = tile_assets();
        let mut plane = vec![0u16; TRAIL_PLANE_PIXELS];
        sprites.raster(&assets, &mut plane, GameVideoOptions::default());
        let at = geometry.first_line() as usize * SPRITE_PLANE_WIDTH + 46;
        assert_eq!(plane[at], 0x101f);
        assert_eq!(plane[at + 15], 0x101f);
        assert_eq!(plane[at + 16], 0);

        // Expanded raster geometry draws at the scaled origin.
        let options = GameVideoOptions {
            scale: 2,
            border: 4,
        };
        let size = sprite_plane_size(&options, &geometry);
        assert_eq!(size, (320 + 8) * 2 * (232 * 2));
        let mut expanded = vec![0u16; size];
        sprites.raster(&assets, &mut expanded, options);
        // Scaled geometry: x maps through (sprite.x * scale + 128) >> 8 - origin_x * scale.
        let width = options.width(&geometry) as usize;
        let base_x = (((46 * 256) * options.scale as i32 + 128) >> 8)
            - (46 - options.border as i32) * options.scale as i32;
        let base_y = ((geometry.first_line() as i32 * 256) * options.scale as i32 + 255) >> 8;
        let base_y = base_y - geometry.first_line() as i32 * options.scale as i32;
        assert_eq!(base_x, 8);
        assert_eq!(base_y, 0);
        assert_eq!(
            expanded[base_y as usize * width + base_x as usize],
            0x101f,
            "expanded raster places the sprite at the scaled origin"
        );
        assert_eq!(
            expanded[base_y as usize * width + base_x as usize + 1],
            0x101f
        );
    }
}
