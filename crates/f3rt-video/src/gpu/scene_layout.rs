//! GPU scene buffer layout shared with the GLSL shaders. Offsets are in 32-bit words.

// Region bases.
// PF cells: 4 layers * 2048 raw cells, two words per slot. Word 0 is the raw big-endian
// video-RAM cell (attributes<<16 | tile code). Word 1 of the PF2/PF3 slots holds the cell of
// that layer's alternate map (physical maps 4/5) when the game has extended_alt_maps; it is read
// only on rows carrying [`F3_LAYER_ALT_MAP`].
pub const F3_SCENE_PF_CELLS: usize = 0;
pub const F3_SCENE_PF_CELL_STRIDE: usize = 2;
pub const F3_SCENE_PF_LAYER_CELLS: usize = 2048;
// Text cells: 4096 raw big-endian text-map words.
pub const F3_SCENE_TEXT_CELLS: usize = 16384;
// Glyph RAM: 0x2000 raw bytes packed four per word, so 2048 of the 4096 words of the region are
// used.
pub const F3_SCENE_GLYPHS: usize = 20480;
pub const F3_SCENE_GLYPH_WORDS: usize = 2048;
// Palette: 8192 words, 0x00RRGGBB.
pub const F3_SCENE_PALETTE: usize = 24576;
pub const F3_SCENE_PALETTE_WORDS: usize = 8192;
// Scanline rows: 256 rows of ROW_STRIDE words.
pub const F3_SCENE_ROWS: usize = 32768;
pub const F3_SCENE_ROW_COUNT: usize = 256;
pub const F3_SCENE_ROW_STRIDE: usize = 352;
// Sprites: up to 4096 descriptors (max_presented_sprites) of SPRITE_STRIDE words.
pub const F3_SCENE_SPRITES: usize = 122880;
pub const F3_SCENE_SPRITE_COUNT: usize = 4096;
pub const F3_SCENE_SPRITE_STRIDE: usize = 8;
// Pivot RAM: 64 KiB of raw bitmap tiles, packed four bytes per word.
pub const F3_SCENE_PIVOT: usize = F3_SCENE_SPRITES + F3_SCENE_SPRITE_COUNT * F3_SCENE_SPRITE_STRIDE;
pub const F3_SCENE_PIVOT_WORDS: usize = 16384;
// Canonical scene size, and the opt-in interpolation coefficient region that follows it.
pub const F3_SCENE_WORD_COUNT: usize = F3_SCENE_PIVOT + F3_SCENE_PIVOT_WORDS;
pub const F3_SCENE_INTERP: usize = F3_SCENE_WORD_COUNT;
pub const F3_SCENE_INTERP_ROW_STRIDE: usize = 52;
pub const F3_SCENE_INTERP_WORD_COUNT: usize =
    F3_SCENE_INTERP + F3_SCENE_ROW_COUNT * F3_SCENE_INTERP_ROW_STRIDE;

// Row header fields (relative to a row base).
pub const F3_ROW_BACKGROUND: usize = 0;
/// bits 0-7 mosaic period, then the row colour flags below.
pub const F3_ROW_MOSAIC: usize = 1;
/// palette words are FDA 15-bit RRRRGGGGBBBBRGBx
pub const F3_ROW_PALETTE15: u32 = 256;
/// two-pixel horizontal blur of the finished row
pub const F3_ROW_BLUR: u32 = 512;
pub const F3_ROW_TEXT_X: usize = 2;
pub const F3_ROW_TEXT_Y: usize = 3;
/// four 8-bit weights, weight 0 in the low byte
pub const F3_ROW_BLEND: usize = 4;
/// nine layer ids, front to back
pub const F3_ROW_ORDER: usize = 5;
/// 24.8 text scroll; read only with the motion layer-mask bit
pub const F3_ROW_MOTION_TEXT_X: usize = 14;
pub const F3_ROW_MOTION_TEXT_Y: usize = 15;

// Layer numbering of LayerId. scene.glsl takes LAYER_COUNT, LAYER_SP0 and LAYER_TEXT from these;
// the encoder asserts them against `LayerId`.
pub const F3_SCENE_LAYER_COUNT: usize = 9;
pub const F3_SCENE_LAYER_SP0: usize = 4;
pub const F3_SCENE_LAYER_TEXT: usize = 8;
// Layer blocks: nine per row, indexed by LayerId (0..3 PF, 4..7 SP, 8 text).
pub const F3_ROW_LAYERS: usize = 16;
pub const F3_LAYER_STRIDE: usize = 34;
pub const F3_LAYER_FLAGS: usize = 0;
pub const F3_LAYER_CLIP_COUNT: usize = 1;
/// (left, right) pairs, up to 16
pub const F3_LAYER_CLIPS: usize = 2;
// Layer flag bits.
pub const F3_LAYER_MODE_SHIFT: u32 = 4;
pub const F3_LAYER_ENABLED: u32 = 64;
pub const F3_LAYER_SELECT_SHIFT: u32 = 7;
pub const F3_LAYER_MOSAIC: u32 = 256;
/// playfield rows sampling the alternate map (word 1 of the cell slot)
pub const F3_LAYER_ALT_MAP: u32 = 512;
/// playfield 64 columns (extend=true) vs 32 columns (extend=false)
pub const F3_LAYER_EXTEND: u32 = 1024;
/// text rows sampling bitmap tiles
pub const F3_LAYER_BITMAP: u32 = 2048;

// Playfield geometry blocks: four per row, after the layer blocks.
pub const F3_ROW_PF: usize = 322;
pub const F3_PF_STRIDE: usize = 6;
pub const F3_PF_SOURCE_X: usize = 0;
pub const F3_PF_SOURCE_Y: usize = 1;
pub const F3_PF_X_STEP: usize = 2;
pub const F3_PF_Y_STEP: usize = 3;
pub const F3_PF_Y_FRACTION: usize = 4;
pub const F3_PF_PALETTE_ADD: usize = 5;
pub const F3_ROW_BITMAP_MAP_ROW: usize = 346;

// Sprite descriptor fields.
pub const F3_SPRITE_X: usize = 0;
pub const F3_SPRITE_Y: usize = 1;
pub const F3_SPRITE_SCALE_X: usize = 2;
pub const F3_SPRITE_SCALE_Y: usize = 3;
pub const F3_SPRITE_TILE: usize = 4;
pub const F3_SPRITE_PALETTE: usize = 5;
/// bit 0 flip X, bit 1 flip Y
pub const F3_SPRITE_FLIP: usize = 6;
pub const F3_SPRITE_WEIGHT: usize = 7;
pub const F3_SPRITE_WEIGHT_SCALE: u32 = 65536;
pub const F3_MASK_SPRITE_TRAILS: u32 = 0x0400_0000;
pub const F3_MASK_SCREEN_FLIPPED: u32 = 0x0800_0000;
#[cfg(feature = "gpu")]
pub const F3_MASK_HIDE_WEIGHTED: u32 = 0x1000_0000;
#[cfg(feature = "gpu")]
pub const F3_MASK_BLEND_WEIGHTED: u32 = 0x2000_0000;
pub const F3_SPRITE_FLIP_X: u32 = 1;
pub const F3_SPRITE_FLIP_Y: u32 = 2;
pub const F3_SPRITE_SCREEN_FLIPPED: u32 = 4;

// Interpolation coefficients: per row, four playfield blocks of PF_STRIDE words:
// [flags][3 source][3 zoom][3 vertical][3 palette], each triple being native-anchored cubic
// increments c1, c2, c3 as float bits.
pub const F3_INTERP_PF_STRIDE: usize = 13;
pub const F3_INTERP_SOURCE: usize = 1;
pub const F3_INTERP_ZOOM: usize = 4;
pub const F3_INTERP_VERTICAL: usize = 7;
pub const F3_INTERP_PALETTE: usize = 10;
pub const F3_INTERP_FLAG_SOURCE: u32 = 1;
pub const F3_INTERP_FLAG_ZOOM: u32 = 2;
pub const F3_INTERP_FLAG_VERTICAL: u32 = 4;
pub const F3_INTERP_FLAG_PALETTE: u32 = 8;
pub const F3_INTERP_PALETTE_STRIDE_SHIFT: u32 = 16;

/// Shader uniforms serialized as three tightly packed uvec4s.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SceneUniforms {
    pub scale: u32,
    pub border: u32,
    pub width: u32,
    pub height: u32,
    pub sprite_count: u32,
    pub pen_mask: u32,
    pub fallback: u32,
    pub layer_mask: u32,
    pub first_line: u32,
    pub native_height: u32,
    pub pf_tile_count: u32,
    pub sp_tile_count: u32,
}

#[cfg(any(feature = "gpu", test))]
impl SceneUniforms {
    pub fn to_bytes(self) -> [u8; 48] {
        uniform_words([
            self.scale,
            self.border,
            self.width,
            self.height,
            self.sprite_count,
            self.pen_mask,
            self.fallback,
            self.layer_mask,
            self.first_line,
            self.native_height,
            self.pf_tile_count,
            self.sp_tile_count,
        ])
    }
}

#[cfg(any(feature = "gpu", test))]
pub(super) fn uniform_words<const WORDS: usize, const BYTES: usize>(
    words: [u32; WORDS],
) -> [u8; BYTES] {
    const { assert!(BYTES == WORDS * 4) };
    let mut bytes = [0; BYTES];
    for (chunk, word) in bytes.chunks_exact_mut(4).zip(words) {
        chunk.copy_from_slice(&word.to_le_bytes());
    }
    bytes
}

const _: () = assert!(
    F3_SCENE_PF_CELLS + 4 * F3_SCENE_PF_LAYER_CELLS * F3_SCENE_PF_CELL_STRIDE
        == F3_SCENE_TEXT_CELLS
);
const _: () = assert!(F3_SCENE_TEXT_CELLS + 4096 == F3_SCENE_GLYPHS);
const _: () = assert!(F3_SCENE_GLYPHS + 4096 == F3_SCENE_PALETTE);
const _: () = assert!(F3_SCENE_PALETTE + F3_SCENE_PALETTE_WORDS <= F3_SCENE_ROWS);
const _: () = assert!(F3_SCENE_ROWS + F3_SCENE_ROW_COUNT * F3_SCENE_ROW_STRIDE == F3_SCENE_SPRITES);
const _: () = assert!(4 * F3_INTERP_PF_STRIDE == F3_SCENE_INTERP_ROW_STRIDE);
const _: () = assert!(F3_SCENE_LAYER_COUNT == 9);
const _: () = assert!(F3_SCENE_LAYER_SP0 == 4);
const _: () = assert!(F3_SCENE_LAYER_TEXT == 8);
const _: () = assert!(
    F3_ROW_ORDER + F3_SCENE_LAYER_COUNT <= F3_ROW_MOTION_TEXT_X
        && F3_ROW_MOTION_TEXT_Y < F3_ROW_LAYERS
);
const _: () = assert!(F3_LAYER_CLIPS + 2 * 5 <= F3_LAYER_STRIDE);
const _: () = assert!(F3_ROW_LAYERS + F3_SCENE_LAYER_COUNT * F3_LAYER_STRIDE == F3_ROW_PF);
const _: () = assert!(F3_ROW_PF + 4 * F3_PF_STRIDE == F3_ROW_BITMAP_MAP_ROW);
const _: () = assert!(F3_ROW_BITMAP_MAP_ROW < F3_SCENE_ROW_STRIDE);
const _: () = assert!(F3_SCENE_SPRITE_COUNT == 4096);
const _: () = assert!(F3_SCENE_PALETTE_WORDS == 8192);
const _: () = assert!(F3_SCENE_WORD_COUNT == 172032);
const _: () = assert!(F3_SCENE_INTERP_WORD_COUNT == 185344);
#[cfg(test)]
mod tests {
    use super::SceneUniforms;

    #[test]
    fn scene_uniforms_match_shader_word_order_and_endianness() {
        let uniforms = SceneUniforms {
            scale: 0x1234_5601,
            border: 2,
            width: 3,
            height: 4,
            sprite_count: 5,
            pen_mask: 6,
            fallback: 7,
            layer_mask: 8,
            first_line: 9,
            native_height: 10,
            pf_tile_count: 11,
            sp_tile_count: 12,
        };
        assert_eq!(
            uniforms.to_bytes(),
            [
                1, 0x56, 0x34, 0x12, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0, 5, 0, 0, 0, 6, 0, 0, 0, 7,
                0, 0, 0, 8, 0, 0, 0, 9, 0, 0, 0, 10, 0, 0, 0, 11, 0, 0, 0, 12, 0, 0, 0,
            ]
        );
    }
}
