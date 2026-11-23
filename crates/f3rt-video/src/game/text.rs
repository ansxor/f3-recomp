//! Text and pivot RAM snapshots.
//! Three regions are snapshotted raw because emulation keeps writing VRAM while the frame is
//! composited or materialized lazily:
//!   `map`:       4096 big-endian 16-bit words at graphics offset 0x1c000 (64x64 cells, 8x8 px);
//!   `glyphs`:    0x2000 bytes at graphics offset 0x1e000 (256 glyphs x 32 bytes, 8x8 4bpp);
//!   `pivot_ram`: 0x10000 bytes of column-major bitmap tiles (2048 x 32 bytes).
//! `pixel` decodes them on demand, bit-identical to the FDP renderer.

use crate::game::ram_shadow::{RamShadow, fresh_content_version};
use crate::game::scene::{ScenePixel, VideoRam};
use crate::memory_map::{
    CONTROL_TEXT_SCROLL_Y_WORD, OFFS_PIVOT, OFFS_TEXTRAM_GLYPHS, OFFS_TEXTRAM_MAP,
    TEXTRAM_GLYPH_SIZE, TEXTRAM_TILES,
};

const PIVOT_RAM_SIZE: usize = 0x10000;

#[derive(Clone)]
pub struct GameText {
    map: [u16; TEXTRAM_TILES],
    glyph_ram: [u8; TEXTRAM_GLYPH_SIZE],
    pivot_ram: [u8; PIVOT_RAM_SIZE],
    scroll_y: u16,
    /// bit r: text row r has a nonzero tile code.
    row_used: u64,
    shadow_map: RamShadow<{ TEXTRAM_TILES * 2 }>,
    shadow_glyphs: RamShadow<TEXTRAM_GLYPH_SIZE>,
    shadow_pivot: RamShadow<PIVOT_RAM_SIZE>,
    version: u64,
}

impl GameText {
    pub fn new() -> Self {
        let mut text = Self {
            map: [0; TEXTRAM_TILES],
            glyph_ram: [0; TEXTRAM_GLYPH_SIZE],
            pivot_ram: [0; PIVOT_RAM_SIZE],
            scroll_y: 0,
            row_used: 0,
            shadow_map: RamShadow::new(),
            shadow_glyphs: RamShadow::new(),
            shadow_pivot: RamShadow::new(),
            version: fresh_content_version(),
        };
        text.reset();
        text
    }

    pub fn reset(&mut self) {
        self.map.fill(0);
        self.glyph_ram.fill(0);
        self.pivot_ram.fill(0);
        self.scroll_y = 0;
        self.row_used = 0;
        self.touch();
    }

    /// Copy the raw text map, glyph RAM and pivot RAM from VRAM at VBSTART.
    pub fn decode(&mut self, vram: &VideoRam<'_>) {
        let graphics = vram.graphics;
        let map_span: &[u8; TEXTRAM_TILES * 2] = graphics
            [OFFS_TEXTRAM_MAP..OFFS_TEXTRAM_MAP + TEXTRAM_TILES * 2]
            .try_into()
            .expect("4 KiB text map");
        let map_changed = self.shadow_map.changed(map_span);
        let glyph_span: &[u8; TEXTRAM_GLYPH_SIZE] = graphics
            [OFFS_TEXTRAM_GLYPHS..OFFS_TEXTRAM_GLYPHS + TEXTRAM_GLYPH_SIZE]
            .try_into()
            .expect("8 KiB glyph RAM");
        let glyphs_changed = self.shadow_glyphs.changed(glyph_span);
        if map_changed {
            self.row_used = 0;
            for (i, (word, raw)) in self
                .map
                .iter_mut()
                .zip(map_span.as_chunks::<2>().0)
                .enumerate()
            {
                *word = u16::from_be_bytes(*raw);
                if *word & 0xff != 0 {
                    self.row_used |= 1u64 << (i >> 6);
                }
            }
        }
        if glyphs_changed {
            self.glyph_ram.copy_from_slice(glyph_span);
        }
        let pivot_span: &[u8; PIVOT_RAM_SIZE] = graphics[OFFS_PIVOT..OFFS_PIVOT + PIVOT_RAM_SIZE]
            .try_into()
            .expect("64 KiB pivot RAM");
        let pivot_changed = self.shadow_pivot.changed(pivot_span);
        if pivot_changed {
            self.pivot_ram.copy_from_slice(pivot_span);
        }
        let scroll_y = vram.control_u16(CONTROL_TEXT_SCROLL_Y_WORD);
        if map_changed || glyphs_changed || pivot_changed || scroll_y != self.scroll_y {
            self.version = fresh_content_version();
        }
        self.scroll_y = scroll_y;
    }

    pub fn version(&self) -> u64 {
        self.version
    }

    /// Raw text-map word bit layout (shared with the GPU decode in `scene_body.glsl`):
    ///   bits 0-7 glyph number (0..255); bit 8 flip X; bits 9-14 palette code; bit 15 flip Y.
    pub fn bitmap_map_row(&self, y: u32, flipped: bool) -> u32 {
        let tile_row = (y & 255) / 8;
        // FDP uses the global text Y register to select the attribute bank.
        tile_row
            + if (tile_row * 8 + u32::from(self.scroll_y) + if flipped { 256 } else { 0 }) & 256
                != 0
            {
                32
            } else {
                0
            }
    }

    /// Decode one text pixel. `bitmap` selects the pivot-RAM bitmap path.
    pub fn pixel(&self, x: i32, y: i32, flipped: bool, bitmap: bool) -> ScenePixel {
        let mut x = x & 511;
        let mut y = y & if bitmap { 255 } else { 511 };
        let bitmap_row = if bitmap {
            self.bitmap_map_row(y as u32, flipped)
        } else {
            0
        };
        if flipped {
            x = 511 - x;
            y = (if bitmap { 255 } else { 511 }) - y;
        }
        let word = self.map
            [(if bitmap { bitmap_row } else { (y / 8) as u32 }) as usize * 64 + (x / 8) as usize];
        let tile = if bitmap {
            (x / 8 * 32 + y / 8) as usize
        } else {
            usize::from(word & 0xff)
        };
        let ram: &[u8] = if bitmap {
            &self.pivot_ram
        } else {
            &self.glyph_ram
        };
        let tx = ((x & 7) ^ if word & 0x0100 != 0 { 7 } else { 0 }) as usize;
        let ty = ((y & 7) ^ if word & 0x8000 != 0 { 7 } else { 0 }) as usize;
        let byte = ram[tile * 32 + ty * 4 + (3 - tx / 2)];
        let pen = (byte >> ((tx & 1) * 4)) & 0x0f;
        ScenePixel {
            palette: (((word >> 9) & 0x3f) * 16).wrapping_add(u16::from(pen)),
            flags: if pen != 0 { 0x10 } else { 0 },
        }
    }

    /// Hardware row-usage cull (FDP `textram_row_usage`): a text row whose 64 tile codes are all
    /// zero is not drawn at all, even when attributes select visible pens.
    pub fn row_used(&self, y: u32) -> bool {
        (self.row_used >> ((y >> 3) & 63)) & 1 != 0
    }

    pub fn map(&self) -> &[u16; TEXTRAM_TILES] {
        &self.map
    }

    pub fn map_mut(&mut self) -> &mut [u16; TEXTRAM_TILES] {
        self.touch();
        &mut self.map
    }

    pub fn glyphs(&self) -> &[u8; TEXTRAM_GLYPH_SIZE] {
        &self.glyph_ram
    }

    pub fn glyphs_mut(&mut self) -> &mut [u8; TEXTRAM_GLYPH_SIZE] {
        self.touch();
        &mut self.glyph_ram
    }

    pub fn pivot(&self) -> &[u8; PIVOT_RAM_SIZE] {
        &self.pivot_ram
    }

    pub fn scroll_y(&self) -> u16 {
        self.scroll_y
    }

    fn touch(&mut self) {
        self.shadow_map.invalidate();
        self.shadow_glyphs.invalidate();
        self.shadow_pivot.invalidate();
        self.version = fresh_content_version();
    }
}

impl Default for GameText {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::memory_map::GRAPHICS_RAM_SIZE;

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

    #[test]
    fn decodes_text_map_and_glyph_ram() {
        let mut sources = Sources::new();
        sources.word(OFFS_TEXTRAM_MAP, (3 << 9) | 0x12);
        sources.graphics[OFFS_TEXTRAM_GLYPHS + 0x12 * 32 + 3] = 0x05;
        let mut text = GameText::new();
        text.decode(&sources.vram());
        let pixel = text.pixel(0, 0, false, false);
        assert_eq!(pixel.palette, 3 * 16 + 5);
        assert_eq!(pixel.flags, 0x10);
    }

    #[test]
    fn flip_bits_mirror_texel_coordinates() {
        let mut sources = Sources::new();
        sources.word(OFFS_TEXTRAM_MAP + 2, (2 << 9) | 0x0100 | 0x8000 | 0x21);
        sources.graphics[OFFS_TEXTRAM_GLYPHS + 0x21 * 32 + 24] = 0x9a;
        let mut text = GameText::new();
        text.decode(&sources.vram());
        let flipped = text.pixel(8, 1, false, false);
        assert_eq!(flipped.palette, 2 * 16 + 9);
        assert_eq!(flipped.flags, 0x10);
    }

    #[test]
    fn dirty_detection_covers_map_glyphs_pivot_and_scroll() {
        let mut sources = Sources::new();
        let mut text = GameText::new();
        text.decode(&sources.vram());
        let mut version = text.version();

        text.decode(&sources.vram());
        assert_eq!(
            text.version(),
            version,
            "unchanged VRAM must not bump the version"
        );

        sources.graphics[OFFS_TEXTRAM_MAP + 0x1fff] ^= 1;
        text.decode(&sources.vram());
        assert_ne!(text.version(), version);
        version = text.version();

        sources.graphics[OFFS_TEXTRAM_GLYPHS + TEXTRAM_GLYPH_SIZE - 1] ^= 1;
        text.decode(&sources.vram());
        assert_ne!(text.version(), version);
        version = text.version();

        sources.graphics[OFFS_PIVOT + PIVOT_RAM_SIZE - 1] ^= 1;
        text.decode(&sources.vram());
        assert_ne!(text.version(), version);
        version = text.version();

        sources.control[CONTROL_TEXT_SCROLL_Y_WORD * 2] = 0x01;
        text.decode(&sources.vram());
        assert_ne!(
            text.version(),
            version,
            "text Y scroll selects the attribute bank"
        );
        assert_eq!(text.scroll_y(), 0x0100);
    }

    #[test]
    fn row_used_tracks_nonzero_tile_codes() {
        let mut sources = Sources::new();
        let mut text = GameText::new();
        text.decode(&sources.vram());
        assert!(!text.row_used(0));

        sources.word(OFFS_TEXTRAM_MAP + 63 * 64 * 2, 0x0001);
        sources.word(OFFS_TEXTRAM_MAP + 64 * 2, 0xff00);
        text.decode(&sources.vram());
        assert!(text.row_used(63 * 8));
        assert!(text.row_used(63 * 8 + 7));
        assert!(!text.row_used(0), "attributes alone never mark a row used");
        assert!(!text.row_used(8), "row 1 has no tile code");

        sources.word(OFFS_TEXTRAM_MAP + 64 * 2, 0);
        text.decode(&sources.vram());
        assert!(!text.row_used(1));
    }

    #[test]
    fn bitmap_map_row_selects_the_attribute_bank_from_scroll_y() {
        let mut sources = Sources::new();
        let mut text = GameText::new();
        sources.control[CONTROL_TEXT_SCROLL_Y_WORD * 2 + 1] = 0x08; // scroll_y = 8
        text.decode(&sources.vram());
        // tile_row 30: (30*8 + 8) & 256 == 0 -> base bank.
        assert_eq!(text.bitmap_map_row(30 * 8, false), 30);
        // tile_row 31: (31*8 + 8) & 256 == 256 -> bank 32 + 31.
        assert_eq!(text.bitmap_map_row(31 * 8, false), 32 + 31);
        // Flipped adds 256 to the scroll: (30*8 + 8 + 256) & 256 == 256.
        assert_eq!(text.bitmap_map_row(30 * 8, true), 32 + 30);
    }

    #[test]
    fn bitmap_pixels_come_from_pivot_ram_tiles() {
        let mut sources = Sources::new();
        // Column 1 of the map holds the attributes for bitmap texels 8..15.
        sources.word(OFFS_TEXTRAM_MAP + 2, 1 << 9);
        // Bitmap tile (x/8, y/8) = (1, 0) is tile 1, whose first row starts at 32 bytes.
        sources.graphics[OFFS_PIVOT + 32 * 32 + 3] = 0x0f;
        let mut text = GameText::new();
        text.decode(&sources.vram());
        let pixel = text.pixel(8, 0, false, true);
        assert_eq!(pixel.palette, 31);
        assert_eq!(pixel.flags, 0x10);
        let plain = text.pixel(8, 0, false, false);
        assert_eq!(
            plain.flags, 0,
            "the glyph path reads glyph 0, which is blank"
        );
    }

    #[test]
    fn mutable_access_invalidates_the_shadow() {
        let mut sources = Sources::new();
        sources.word(OFFS_TEXTRAM_MAP, 0x0007);
        let mut text = GameText::new();
        text.decode(&sources.vram());
        let version = text.version();
        text.map_mut()[0] = 0;
        assert_ne!(text.version(), version);
        assert_eq!(text.map()[0], 0);
        text.decode(&sources.vram());
        assert_eq!(text.map()[0], 0x0007);
        assert!(text.row_used(0));
    }

    #[test]
    fn reset_clears_regions_and_bumps_version() {
        let mut sources = Sources::new();
        sources.word(OFFS_TEXTRAM_MAP, 0x0001);
        let mut text = GameText::new();
        text.decode(&sources.vram());
        let version = text.version();
        text.reset();
        assert_ne!(text.version(), version);
        assert_eq!(text.map()[0], 0);
        assert_eq!(text.glyphs()[0], 0);
        assert_eq!(text.pivot()[0], 0);
        assert_eq!(text.scroll_y(), 0);
        assert!(!text.row_used(0));
    }
}
