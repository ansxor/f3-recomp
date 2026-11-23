//! Playfield tile decode, mechanically ported from `runtime/renderer/game/tiles.{hpp,cpp}`.
//!
//! Each physical map is 2048 cells (64 columns x 32 rows of 16x16 texels) at graphics offset
//! 0x10000 + layer * 0x2000 (2048 cells in extended mode, 32x32 at stride 0x1000 otherwise).
//! A cell is the raw 4-byte big-endian map entry; the snapshot is required because emulation
//! keeps writing VRAM while a frame is composited or materialized lazily.
//!
//! Physical maps 0..3 are PF0..PF3; maps 4/5 are the alternates of PF2/PF3 at 0x18000/0x1a000,
//! selected per scanline (`ScenePlayfield::alt_map`).
//!
//! Raw cell bit layout (identical in the GPU shader and `GameTiles::RowSampler`):
//!   bytes 0-1, attributes (big-endian u16):
//!     bits 0-8   palette code (palette base = code * 16)
//!     bit  9     blend selector
//!     bits 10-11 extra pen planes
//!     bit  14    flip X
//!     bit  15    flip Y
//!     pen mask   = (((attributes >> 10) & 3 & ~attributes) << 4) | 15
//!   bytes 2-3, tile code (big-endian u16): the full 16-bit word, wrapped to the PF ROM tile
//!   count (`wrap_tile_index`).

use crate::config::VideoConfig;
use crate::game::ram_shadow::{RamShadowSlice, fresh_content_version};
use crate::game::scene::{ScenePixel, SceneSource, VideoRam, wrap_tile_index};
use crate::memory_map::OFFS_PLAYFIELDS;

/// Map columns of the widest configuration (extended mode).
pub const MAX_PF_COLUMNS: usize = 64;
/// Cells per map in the widest configuration.
pub const MAX_PF_CELLS: usize = MAX_PF_COLUMNS * 32;
/// Physical maps in the widest configuration (alternates included).
pub const MAX_PF_MAPS: usize = 6;
/// Playfield map width in texels of the widest configuration.
const MAX_PF_MAP_PX: usize = MAX_PF_COLUMNS * 16;
/// Tile rows per map.
const MAP_ROWS: usize = 32;

#[derive(Clone)]
pub struct GameTiles {
    video: VideoConfig,
    columns: usize,
    cells_per_map: usize,
    maps: [[u32; MAX_PF_CELLS]; MAX_PF_MAPS],
    /// bit r: map row r has a nonzero tile code.
    row_used: [u32; MAX_PF_MAPS],
    /// Raw RAM behind each decoded map.
    shadow: [RamShadowSlice; MAX_PF_MAPS],
    version: u64,
}

/// A compose layer/subrow owns this sampler; it decodes raw cells on demand.
pub struct RowSampler<'a> {
    cells: &'a [u32],
    tiles: &'a [u8],
    tile_count: u32,
    map_width_px: u32,
    y: u32,
    screen_flip: u32,
    column: u32,
    palette: u16,
    pen_mask: u8,
    flip_x: u8,
    blend: u8,
    /// Texel offset of the bound tile's row inside `tiles`.
    tile_base: usize,
}

impl RowSampler<'_> {
    pub fn pixel(&mut self, x: i32) -> ScenePixel {
        let wrapped_x = (x as u32 & (self.map_width_px - 1)) ^ self.screen_flip;
        let column = wrapped_x / 16;
        if column != self.column {
            let cell = self.cells[column as usize];
            let attributes = (cell >> 16) as u16;
            let palette_code = attributes & 0x1ff;
            let ty = self.y ^ if attributes & 0x8000 != 0 { 15 } else { 0 };
            let tile = wrap_tile_index(cell as u16 as u32, self.tile_count);
            self.tile_base = tile as usize * 256 + ty as usize * 16;
            self.palette = palette_code * 16;
            self.pen_mask = ((((attributes >> 10) & 3 & !attributes) << 4) | 15) as u8;
            self.flip_x = if attributes & 0x4000 != 0 { 15 } else { 0 };
            self.blend = ((attributes >> 9) & 1) as u8;
            self.column = column;
        }
        // `column` is at most columns - 1, so the branch above always binds the tile first;
        // `column` starts at `columns`, which no column matches.
        let pen = self.tiles
            [self.tile_base + (((wrapped_x & 15) ^ u32::from(self.flip_x)) as usize)]
            & self.pen_mask;
        ScenePixel {
            palette: self.palette + u16::from(pen),
            flags: (if pen != 0 { 0x10 } else { 0 }) | self.blend,
        }
    }
}

impl GameTiles {
    pub fn new(video: &VideoConfig) -> Self {
        Self {
            video: *video,
            columns: video.pf_columns(),
            cells_per_map: video.pf_columns() * MAP_ROWS,
            maps: [[0; MAX_PF_CELLS]; MAX_PF_MAPS],
            row_used: [0; MAX_PF_MAPS],
            shadow: std::array::from_fn(|_| RamShadowSlice::new()),
            version: fresh_content_version(),
        }
    }

    pub fn reset(&mut self) {
        for map in &mut self.maps {
            map.fill(0);
        }
        self.row_used.fill(0);
        self.touch();
    }

    /// Copy the raw PF layers from video RAM at VBSTART.
    pub fn decode(&mut self, vram: &VideoRam<'_>) {
        let mut any = false;
        let stride = self.video.pf_layer_stride();
        let columns = self.columns;
        let cells = self.cells_per_map;
        for layer in 0..self.video.map_count() {
            let base = OFFS_PLAYFIELDS + layer * stride;
            if !self.shadow[layer].changed(&vram.graphics[base..base + stride]) {
                continue;
            }
            any = true;
            for (i, cell) in self.maps[layer][..cells].iter_mut().enumerate() {
                let at = base + i * 4;
                *cell = u32::from(vram.u16(at)) << 16 | u32::from(vram.u16(at + 2));
            }
            let mut used = 0u32;
            for row in 0..MAP_ROWS {
                if self.maps[layer][row * columns..(row + 1) * columns]
                    .iter()
                    .any(|cell| *cell as u16 != 0)
                {
                    used |= 1 << row;
                }
            }
            self.row_used[layer] = used;
        }
        if any {
            self.version = fresh_content_version();
        }
    }

    pub fn version(&self) -> u64 {
        self.version
    }

    pub fn columns(&self) -> usize {
        self.columns
    }

    pub fn cells_per_map(&self) -> usize {
        self.cells_per_map
    }

    pub fn map_width_px(&self) -> u32 {
        self.video.pf_map_width_px()
    }

    pub fn map_stride(&self) -> usize {
        self.video.pf_layer_stride()
    }

    /// Physical map count: 4, or 6 with the PF2/PF3 alternate maps.
    pub fn map_count(&self) -> usize {
        self.video.map_count()
    }

    pub fn video(&self) -> &VideoConfig {
        &self.video
    }

    pub fn physical_map(layer: usize, alt: bool) -> usize {
        layer + if alt { 2 } else { 0 }
    }

    pub fn row_sampler<'a>(
        &'a self,
        layer: usize,
        y: i32,
        flipped: bool,
        tiles: &'a [u8],
        alt: bool,
    ) -> RowSampler<'a> {
        let map = Self::physical_map(layer, alt);
        let wrapped_y = (y as u32 & 511) ^ if flipped { 511 } else { 0 };
        RowSampler {
            cells: &self.maps[map][(wrapped_y as usize / 16) * self.columns..],
            tiles,
            tile_count: (tiles.len() / 256) as u32,
            map_width_px: self.video.pf_map_width_px(),
            y: wrapped_y & 15,
            screen_flip: if flipped {
                self.video.pf_map_width_px() - 1
            } else {
                0
            },
            column: self.columns as u32,
            palette: 0,
            pen_mask: 0,
            flip_x: 0,
            blend: 0,
            tile_base: 0,
        }
    }

    /// Hardware row-usage cull (FDP `update_row_usages`): a map row whose 32+ tile code words
    /// are all zero is not drawn at all, even when its attributes select visible pens.
    /// `source_y` is the playfield source row (`ScenePlayfield::source_y`, 0..511), before
    /// global flip.
    pub fn row_used(&self, map: usize, source_y: u32) -> bool {
        (self.row_used[map] >> ((source_y >> 4) & 31)) & 1 != 0
    }

    /// Raw cells of one layer (row-major columns x 32), for snapshot consumers.
    pub fn cells(&self, map: usize) -> &[u32] {
        &self.maps[map][..self.cells_per_map]
    }

    pub fn cells_mut(&mut self, map: usize) -> &mut [u32] {
        self.touch();
        let cells = self.cells_per_map;
        &mut self.maps[map][..cells]
    }

    pub fn playfield_pixel(
        &self,
        layer: usize,
        x: i32,
        y: i32,
        flipped: bool,
        tiles: &[u8],
        alt: bool,
    ) -> ScenePixel {
        self.row_sampler(layer, y, flipped, tiles, alt).pixel(x)
    }

    fn touch(&mut self) {
        for shadow in &mut self.shadow {
            shadow.invalidate();
        }
        self.version = fresh_content_version();
    }
}

impl SceneSource for GameTiles {
    fn reset(&mut self) {
        Self::reset(self);
    }

    fn decode(&mut self, vram: &VideoRam<'_>) {
        Self::decode(self, vram);
    }
}

/// Render-only (never serialized, never feeds emulation): which PF2/PF3 alternate-map tile rows
/// are an exact X2/Y1 alias of the same row of the full-resolution main map.
///
/// For tile row r of PF `layer` (2/3) the solved offsets are the c in `[0, pf_map_width_px)` such
/// that for every x and all 16 lines y of the row
///     alt(x, y) == main((2x + c) mod pf_map_width_px, y)
/// comparing the final decoded sample (`RowSampler::pixel`: palette+pen and flags), so tile codes,
/// attributes and flips may differ between the maps. Rows whose alternate row holds no tile code
/// (hardware row cull) and rows without any exact c have an empty set. Results are cached per row
/// keyed on the raw cells of both maps, so only rows changed by an upload are solved again.
pub struct FullResolutionAltMaps {
    video: VideoConfig,
    rows: [[AltRow; MAP_ROWS]; 2],
    /// Identity of the tile ROM the cache was solved against (address + size).
    tile_data: Option<(usize, usize)>,
    solved_rows: u64,
}

struct AltRow {
    /// Cache key: raw cells.
    main: [u32; MAX_PF_COLUMNS],
    alt: [u32; MAX_PF_COLUMNS],
    /// bit c: offset c is exact.
    offsets: [u64; MAX_PF_COLUMNS],
    /// Key and offsets describe a solved row.
    valid: bool,
}

impl AltRow {
    fn new() -> Self {
        Self {
            main: [0; MAX_PF_COLUMNS],
            alt: [0; MAX_PF_COLUMNS],
            offsets: [0; MAX_PF_COLUMNS],
            valid: false,
        }
    }
}

impl FullResolutionAltMaps {
    pub const ROWS: usize = MAP_ROWS;

    pub fn new(video: &VideoConfig) -> Self {
        Self {
            video: *video,
            rows: std::array::from_fn(|_| std::array::from_fn(|_| AltRow::new())),
            tile_data: None,
            solved_rows: 0,
        }
    }

    pub fn reset(&mut self) {
        let video = self.video;
        *self = Self::new(&video);
    }

    /// Solve every PF2/PF3 tile row whose cells (or the tile ROM) changed since the previous
    /// update. No-op unless `VideoConfig::full_resolution_alt_maps`.
    pub fn update(&mut self, tiles: &GameTiles, tile_pixels: &[u8]) {
        if !self.video.full_resolution_alt_maps {
            return;
        }
        let identity = (tile_pixels.as_ptr() as usize, tile_pixels.len());
        if self.tile_data != Some(identity) {
            for layer in &mut self.rows {
                for row in layer {
                    row.valid = false;
                }
            }
            self.tile_data = Some(identity);
        }
        let columns = tiles.columns();
        for l in 0..2 {
            let main = tiles.cells(GameTiles::physical_map(2 + l, false));
            let alt = tiles.cells(GameTiles::physical_map(2 + l, true));
            for r in 0..Self::ROWS {
                let row = &mut self.rows[l][r];
                let m = &main[r * columns..(r + 1) * columns];
                let a = &alt[r * columns..(r + 1) * columns];
                if row.valid && row.main[..columns] == *m && row.alt[..columns] == *a {
                    continue;
                }
                row.main[..columns].copy_from_slice(m);
                row.alt[..columns].copy_from_slice(a);
                Self::solve(row, &self.video, tiles, tile_pixels, 2 + l, r);
                row.valid = true;
                self.solved_rows += 1;
            }
        }
    }

    /// Smallest offset valid for every tile row from `first_row` through `last_row` (cyclic
    /// mod 32) of PF `layer` (2 or 3); -1 when some row has none.
    pub fn offset(&self, layer: usize, first_row: usize, last_row: usize) -> i32 {
        if !self.video.full_resolution_alt_maps {
            return -1;
        }
        let layer_rows = &self.rows[layer - 2];
        let mut common = layer_rows[first_row % Self::ROWS].offsets;
        let span = last_row.wrapping_sub(first_row) % Self::ROWS;
        for row in first_row + 1..=first_row + span {
            let other = layer_rows[row % Self::ROWS].offsets;
            for (word, other) in common.iter_mut().zip(other.iter()) {
                *word &= *other;
            }
        }
        for (i, word) in common.iter().enumerate() {
            if *word != 0 {
                return (i * 64 + word.trailing_zeros() as usize) as i32;
            }
        }
        -1
    }

    /// Tile rows solved by `update` since reset (cache misses).
    pub fn solved_rows(&self) -> u64 {
        self.solved_rows
    }

    fn solve(
        row: &mut AltRow,
        video: &VideoConfig,
        tiles: &GameTiles,
        tile_pixels: &[u8],
        layer: usize,
        tile_row: usize,
    ) {
        const LINES: usize = 16;
        let map_width_px = video.pf_map_width_px() as usize;
        let half = map_width_px / 2;
        row.offsets = [0; MAX_PF_COLUMNS];
        // The hardware never draws a row without tile codes (GameTiles::row_used); nothing to
        // alias.
        if !tiles.row_used(
            GameTiles::physical_map(layer, true),
            (tile_row * LINES) as u32,
        ) {
            return;
        }
        // Decoded samples (palette and flags) per line: the alt row, and the main row split by x
        // parity. A twin of alt(x) = main(2x + c) forces the alt row to repeat every half map.
        let mut alt = [[0u32; MAX_PF_MAP_PX / 2]; LINES];
        let mut even = [[0u32; MAX_PF_MAP_PX / 2]; LINES];
        let mut odd = [[0u32; MAX_PF_MAP_PX / 2]; LINES];
        let mut samples = [0u32; MAX_PF_MAP_PX];
        let decode = |alternate: bool, line: usize, samples: &mut [u32; MAX_PF_MAP_PX]| {
            let mut sampler = tiles.row_sampler(
                layer,
                (tile_row * LINES + line) as i32,
                false,
                tile_pixels,
                alternate,
            );
            for (x, sample) in samples[..map_width_px].iter_mut().enumerate() {
                let pixel = sampler.pixel(x as i32);
                *sample = u32::from(pixel.palette) | u32::from(pixel.flags) << 16;
            }
        };
        for line in 0..LINES {
            decode(true, line, &mut samples);
            if samples[..half] != samples[half..map_width_px] {
                return;
            }
            alt[line][..half].copy_from_slice(&samples[..half]);
            decode(false, line, &mut samples);
            for i in 0..half {
                even[line][i] = samples[2 * i];
                odd[line][i] = samples[2 * i + 1];
            }
        }
        // c = 2m + parity: alt(x) == main(2x + c) <=> alt[x] == (parity ? odd : even)[(x + m) mod
        // half].
        for parity in 0..2 {
            let source = if parity != 0 { &odd } else { &even };
            for m in 0..half {
                let mut exact = true;
                for line in 0..LINES {
                    // Rotation by m: the two memcmp halves of the C++ solver.
                    let tail_matches = m == 0 || alt[line][half - m..half] == source[line][..m];
                    exact = tail_matches && alt[line][..half - m] == source[line][m..half];
                    if !exact {
                        break;
                    }
                }
                if exact {
                    let c = 2 * m + parity;
                    row.offsets[c / 64] |= 1u64 << (c % 64);
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::memory_map::{GRAPHICS_RAM_SIZE, PLAYFIELD_STANDARD_LAYER_SIZE};

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

        fn cell(&mut self, base: usize, index: usize, attributes: u16, code: u16) {
            let at = base + index * 4;
            self.graphics[at..at + 4].copy_from_slice(&[
                (attributes >> 8) as u8,
                attributes as u8,
                (code >> 8) as u8,
                code as u8,
            ]);
        }
    }

    fn extended() -> VideoConfig {
        VideoConfig {
            extend: true,
            extended_alt_maps: true,
            ..VideoConfig::default()
        }
    }

    #[test]
    fn decodes_cells_row_usage_and_version() {
        let mut sources = Sources::new();
        let mut tiles = GameTiles::new(&extended());
        let version = tiles.version();
        tiles.decode(&sources.vram());
        assert_ne!(
            tiles.version(),
            version,
            "first decode always counts as a change"
        );
        let version = tiles.version();
        tiles.decode(&sources.vram());
        assert_eq!(
            tiles.version(),
            version,
            "unchanged VRAM must not bump the version"
        );
        assert!(!tiles.row_used(0, 0));

        sources.cell(OFFS_PLAYFIELDS, 5 * 64 + 3, 0x1234, 0x0042);
        tiles.decode(&sources.vram());
        assert_ne!(tiles.version(), version);
        assert_eq!(tiles.cells(0)[5 * 64 + 3], 0x1234_0042);
        assert!(tiles.row_used(0, 5 * 16));
        assert!(tiles.row_used(0, 5 * 16 + 15));
        assert!(!tiles.row_used(0, 4 * 16));

        // Attributes alone never mark a row used.
        sources.cell(OFFS_PLAYFIELDS, 6 * 64, 0xffff, 0);
        tiles.decode(&sources.vram());
        assert!(!tiles.row_used(0, 6 * 16));
    }

    #[test]
    fn each_layer_has_its_own_shadow_and_stride() {
        let mut sources = Sources::new();
        let mut tiles = GameTiles::new(&extended());
        tiles.decode(&sources.vram());
        let version = tiles.version();

        // Last cell of PF1's 8 KiB layer.
        let pf1_last = OFFS_PLAYFIELDS + 0x2000 + (MAX_PF_CELLS - 1) * 4;
        sources.graphics[pf1_last + 3] = 1;
        tiles.decode(&sources.vram());
        assert_ne!(tiles.version(), version);
        assert_eq!(tiles.cells(1)[MAX_PF_CELLS - 1], 1);
        assert!(tiles.row_used(1, 31 * 16));
        assert!(!tiles.row_used(0, 31 * 16));

        // Alternate maps 4/5 sit at 0x18000/0x1a000.
        let alt0 = OFFS_PLAYFIELDS + 4 * 0x2000;
        sources.cell(alt0, 0, 0, 7);
        tiles.decode(&sources.vram());
        assert_eq!(tiles.cells(GameTiles::physical_map(2, true))[0], 7);
    }

    #[test]
    fn standard_mode_uses_32_columns_and_4k_strides() {
        let video = VideoConfig {
            extend: false,
            extended_alt_maps: false,
            ..VideoConfig::default()
        };
        let mut sources = Sources::new();
        let mut tiles = GameTiles::new(&video);
        assert_eq!(tiles.columns(), 32);
        assert_eq!(tiles.cells_per_map(), 1024);
        assert_eq!(tiles.map_width_px(), 512);
        assert_eq!(tiles.map_stride(), PLAYFIELD_STANDARD_LAYER_SIZE);
        assert_eq!(
            tiles.map_count(),
            6,
            "standard mode always exposes the alternates"
        );

        sources.cell(OFFS_PLAYFIELDS + 0x1000, 0, 0, 3);
        tiles.decode(&sources.vram());
        assert_eq!(tiles.cells(1)[0], 3);
        assert!(tiles.row_used(1, 0));
        // 4 KiB stride: PF2 starts at +0x2000, which is also map 4's start.
        assert_eq!(tiles.cells(GameTiles::physical_map(2, false))[0], 0);
    }

    #[test]
    fn reset_clears_cells_and_bumps_version() {
        let mut sources = Sources::new();
        let mut tiles = GameTiles::new(&extended());
        sources.cell(OFFS_PLAYFIELDS, 0, 0, 1);
        tiles.decode(&sources.vram());
        let version = tiles.version();
        tiles.reset();
        assert_ne!(tiles.version(), version);
        assert_eq!(tiles.cells(0)[0], 0);
        assert!(!tiles.row_used(0, 0));
        tiles.decode(&sources.vram());
        assert_eq!(tiles.cells(0)[0], 1);
    }

    #[test]
    fn sampler_matches_an_independent_cell_decode() {
        let mut sources = Sources::new();
        let mut assets = vec![0u8; 64 * 256];
        for (i, byte) in assets.iter_mut().enumerate() {
            *byte = (i * 37 + i / 16) as u8;
        }
        let columns = MAX_PF_COLUMNS;
        for layer in 0..4 {
            for i in 0..MAX_PF_CELLS {
                let attributes = ((i & 0x1ff)
                    | ((i >> 9) & 1) << 9
                    | ((i >> 2) & 1) << 10
                    | (i & 1) << 14
                    | ((i >> 1) & 1) << 15) as u16;
                let code = (0x8000 | (i & 63)) as u16;
                sources.cell(OFFS_PLAYFIELDS + layer * 0x2000, i, attributes, code);
            }
        }
        let mut tiles = GameTiles::new(&extended());
        tiles.decode(&sources.vram());

        for layer in 0..4 {
            for flipped in [false, true] {
                for y in [-513i32, -512, -1, 0, 15, 16, 511, 512] {
                    let mut sampler = tiles.row_sampler(layer, y, flipped, &assets, false);
                    for x in [
                        0i32, 15, 16, 17, 31, 32, 1023, 1024, -1, -16, -17, -1024, -1025, 511, 256,
                    ] {
                        let sx = (x as u32 & (1024 - 1)) ^ if flipped { 1023 } else { 0 };
                        let sy = (y as u32 & 511) ^ if flipped { 511 } else { 0 };
                        let cell =
                            tiles.cells(layer)[(sy as usize / 16) * columns + sx as usize / 16];
                        let attributes = (cell >> 16) as u16;
                        let code = cell as u16 as u32;
                        let palette_code = attributes & 0x1ff;
                        let pen_mask = (((attributes >> 10) & 3 & !attributes) << 4) | 15;
                        let tx = (sx & 15) ^ if attributes & 0x4000 != 0 { 15 } else { 0 };
                        let ty = (sy & 15) ^ if attributes & 0x8000 != 0 { 15 } else { 0 };
                        let pen = assets
                            [(code & 0x7fff) as usize * 256 + ty as usize * 16 + tx as usize]
                            & pen_mask as u8;
                        let pixel = sampler.pixel(x);
                        assert_eq!(pixel.palette, palette_code * 16 + u16::from(pen));
                        assert_eq!(
                            pixel.flags,
                            (if pen != 0 { 0x10 } else { 0 }) | ((attributes >> 9) & 1) as u8
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn playfield_pixel_applies_palette_mask_and_flips() {
        let mut sources = Sources::new();
        sources.cell(OFFS_PLAYFIELDS + 0x2000, 0, 3 | 0x200, 5);
        sources.cell(OFFS_PLAYFIELDS + 0x2000, 1, 4 | (1 << 10) | 0x4000, 6);
        sources.cell(OFFS_PLAYFIELDS + 0x2000, 2, 5 | (1 << 10), 7);
        sources.cell(OFFS_PLAYFIELDS + 0x2000, 3, 6 | 0x8000, 8);
        let mut assets = vec![0u8; 16 * 256];
        for (i, byte) in assets.iter_mut().enumerate() {
            *byte = (i * 5 + 1) as u8;
        }
        let mut tiles = GameTiles::new(&extended());
        tiles.decode(&sources.vram());

        let plain = tiles.playfield_pixel(1, 0, 0, false, &assets, false);
        assert_eq!(plain.palette, 48 + u16::from(assets[5 * 256] & 15));
        assert_eq!(plain.flags, 0x11);

        let flip0 = tiles.playfield_pixel(1, 16, 0, false, &assets, false);
        let flip15 = tiles.playfield_pixel(1, 31, 0, false, &assets, false);
        assert_eq!(flip0.palette, 64 + u16::from(assets[6 * 256 + 15] & 31));
        assert_eq!(flip15.palette, 64 + u16::from(assets[6 * 256] & 31));

        let masked = tiles.playfield_pixel(1, 32, 0, false, &assets, false);
        assert_eq!(masked.palette, 80 + u16::from(assets[7 * 256] & 15));

        let flip_y = tiles.playfield_pixel(1, 48, 0, false, &assets, false);
        assert_eq!(
            flip_y.palette,
            96 + u16::from(assets[8 * 256 + 15 * 16] & 15)
        );
        assert_eq!(flip_y.flags, 0x10);
    }

    #[test]
    fn full_resolution_alt_maps_solve_exact_aliases() {
        let video = VideoConfig {
            extend: true,
            extended_alt_maps: true,
            full_resolution_alt_maps: true,
            ..VideoConfig::default()
        };
        const COLUMNS: usize = MAX_PF_COLUMNS;
        // Tiles 0..63: main map. 64..127: alternate row 0 (main(2x)). 128..191: alternate row 1
        // (main(2x + 1)).
        const TILES: usize = 3 * COLUMNS;

        let main_pen = |tile: usize, tx: usize, ty: usize| -> u8 {
            ((tile * 5 + tx * 3 + ty * 7) & 0x0f) as u8
        };
        let mut tile_pixels = vec![0u8; TILES * 256];
        for tile in 0..COLUMNS {
            for ty in 0..16 {
                for tx in 0..16 {
                    tile_pixels[tile * 256 + ty * 16 + tx] = main_pen(tile, tx, ty);
                }
            }
        }
        // Each alternate tile copies the source texels its 16 columns sample through the alias.
        for (row, c) in [(0usize, 0usize), (1, 1)] {
            for j in 0..COLUMNS {
                let alt_tile = COLUMNS + row * COLUMNS + j;
                for ty in 0..16 {
                    for t in 0..16 {
                        let x = 16 * j + t;
                        let source = 2 * x + c;
                        let tile = (source % 1024) / 16;
                        let tx = source % 16;
                        tile_pixels[alt_tile * 256 + ty * 16 + t] = main_pen(tile, tx, ty);
                    }
                }
            }
        }

        let mut sources = Sources::new();
        let main_base = OFFS_PLAYFIELDS + 2 * 0x2000;
        let alt_base = OFFS_PLAYFIELDS + 4 * 0x2000;
        for row in 0..2 {
            for column in 0..COLUMNS {
                sources.cell(main_base, row * COLUMNS + column, 4, column as u16);
                sources.cell(
                    alt_base,
                    row * COLUMNS + column,
                    4,
                    (COLUMNS + row * COLUMNS + column) as u16,
                );
            }
        }
        let mut tiles = GameTiles::new(&video);
        tiles.decode(&sources.vram());
        let mut maps = FullResolutionAltMaps::new(&video);
        maps.update(&tiles, &tile_pixels);
        assert_eq!(maps.solved_rows(), 2 * MAP_ROWS as u64);

        let cached = maps.solved_rows();
        maps.update(&tiles, &tile_pixels);
        assert_eq!(
            maps.solved_rows(),
            cached,
            "unchanged rows are not re-solved"
        );

        assert_eq!(maps.offset(2, 0, 0), 0);
        assert_eq!(maps.offset(2, 1, 1), 1);
        assert_eq!(maps.offset(2, 0, 1), -1, "rows 0 and 1 share no offset");
        assert_eq!(maps.offset(3, 0, 0), -1, "PF3 has no alternate tile codes");

        // Changing the tile ROM identity invalidates the cache.
        let replacement = vec![0u8; TILES * 256];
        maps.update(&tiles, &replacement);
        assert_eq!(maps.solved_rows(), cached + 2 * MAP_ROWS as u64);
    }

    #[test]
    fn full_resolution_alt_maps_are_inert_when_disabled() {
        let video = VideoConfig {
            extend: true,
            extended_alt_maps: true,
            full_resolution_alt_maps: false,
            ..VideoConfig::default()
        };
        let mut sources = Sources::new();
        sources.cell(OFFS_PLAYFIELDS + 4 * 0x2000, 0, 0, 1);
        let mut tiles = GameTiles::new(&video);
        tiles.decode(&sources.vram());
        let mut maps = FullResolutionAltMaps::new(&video);
        let tile_pixels = vec![0u8; 256];
        maps.update(&tiles, &tile_pixels);
        assert_eq!(maps.solved_rows(), 0);
        assert_eq!(maps.offset(2, 0, 0), -1);
    }
}
