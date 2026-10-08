// GPU scene buffer layout: the single source of truth shared by the C++ encoder
// (runtime/renderer/gpu/encode.cpp) and the GLSL shaders (#include-d from
// runtime/renderer/shaders/scene.glsl). Plain macros with `u` suffixes are valid
// in both languages. All offsets are 32-bit word indices into one storage buffer.
#ifndef F3_SCENE_LAYOUT_H
#define F3_SCENE_LAYOUT_H

// Region bases.
// PF cells: 4 layers * 2048 raw cells, two words per slot. Word 0 is the raw
// big-endian video-RAM cell (attributes<<16 | tile code). Word 1 of the PF2/PF3
// slots holds the cell of that layer's alternate map (physical maps 4/5) when the
// game has extended_alt_maps; it is read only on rows carrying F3_LAYER_ALT_MAP.
#define F3_SCENE_PF_CELLS 0u
#define F3_SCENE_PF_CELL_STRIDE 2u
#define F3_SCENE_PF_LAYER_CELLS 2048u
// Text cells: 4096 raw big-endian text-map words.
#define F3_SCENE_TEXT_CELLS 16384u
// Glyph RAM: 0x2000 raw bytes packed four per word, so 2048 of the 4096 words
// of the region are used.
#define F3_SCENE_GLYPHS 20480u
#define F3_SCENE_GLYPH_WORDS 2048u
// Palette: 8192 words, 0x00RRGGBB.
#define F3_SCENE_PALETTE 24576u
#define F3_SCENE_PALETTE_WORDS 8192u
// Scanline rows: 256 rows of ROW_STRIDE words.
#define F3_SCENE_ROWS 32768u
#define F3_SCENE_ROW_COUNT 256u
#define F3_SCENE_ROW_STRIDE 352u
// Sprites: up to 4096 descriptors (max_presented_sprites) of SPRITE_STRIDE words.
#define F3_SCENE_SPRITES 122880u
#define F3_SCENE_SPRITE_COUNT 4096u
#define F3_SCENE_SPRITE_STRIDE 8u
// Canonical scene size, and the opt-in interpolation coefficient region that
// follows it.
#define F3_SCENE_WORD_COUNT 155648u
#define F3_SCENE_INTERP 155648u
#define F3_SCENE_INTERP_ROW_STRIDE 52u
#define F3_SCENE_INTERP_WORD_COUNT 168960u

// Row header fields (relative to a row base).
#define F3_ROW_BACKGROUND 0u
#define F3_ROW_MOSAIC 1u    // bits 0-7 mosaic period, then the row colour flags below
#define F3_ROW_PALETTE15 256u // palette words are FDA 15-bit RRRRGGGGBBBBRGBx
#define F3_ROW_BLUR 512u      // two-pixel horizontal blur of the finished row
#define F3_ROW_TEXT_X 2u
#define F3_ROW_TEXT_Y 3u
#define F3_ROW_BLEND 4u     // four 8-bit weights, weight 0 in the low byte
#define F3_ROW_ORDER 5u     // nine layer ids, front to back
#define F3_ROW_MOTION_TEXT_X 14u // 24.8 text scroll; read only with the motion layer-mask bit
#define F3_ROW_MOTION_TEXT_Y 15u

// Layer blocks: nine per row, indexed by LayerId (0..3 PF, 4..7 SP, 8 text).
#define F3_ROW_LAYERS 16u
#define F3_LAYER_STRIDE 34u
#define F3_LAYER_FLAGS 0u
#define F3_LAYER_CLIP_COUNT 1u
#define F3_LAYER_CLIPS 2u   // (left, right) pairs, up to 16
// Layer flag bits.
#define F3_LAYER_PRIORITY_MASK 15u
#define F3_LAYER_MODE_SHIFT 4u
#define F3_LAYER_MODE_MASK 3u
#define F3_LAYER_ENABLED 64u
#define F3_LAYER_SELECT_SHIFT 7u
#define F3_LAYER_MOSAIC 256u
#define F3_LAYER_ALT_MAP 512u // playfield rows sampling the alternate map (word 1 of the cell slot)

// Playfield geometry blocks: four per row, after the layer blocks.
#define F3_ROW_PF 322u
#define F3_PF_STRIDE 6u
#define F3_PF_SOURCE_X 0u
#define F3_PF_SOURCE_Y 1u
#define F3_PF_X_STEP 2u
#define F3_PF_Y_STEP 3u
#define F3_PF_Y_FRACTION 4u
#define F3_PF_PALETTE_ADD 5u

// Sprite descriptor fields.
#define F3_SPRITE_X 0u
#define F3_SPRITE_Y 1u
#define F3_SPRITE_SCALE_X 2u
#define F3_SPRITE_SCALE_Y 3u
#define F3_SPRITE_TILE 4u
#define F3_SPRITE_PALETTE 5u
#define F3_SPRITE_FLIP 6u   // bit 0 flip X, bit 1 flip Y
#define F3_SPRITE_FLIP_X 1u
#define F3_SPRITE_FLIP_Y 2u
#define F3_SPRITE_SHADOW 4u // flicker-shadow tag (SceneSprite::shadow): drawn into sprite plane A only

// Presenter bits in the layer-mask uniform (controls.w), above the nine layer bits; bit 31 is the
// motion marker (renderer/gpu/motion.hpp).
#define F3_MASK_HIDE_SHADOW 0x10000000u  // sprite pass: skip descriptors tagged F3_SPRITE_SHADOW (plane B)
#define F3_MASK_BLEND_SHADOW 0x20000000u // scene pass: sprite plane B is valid; average shade(A) and shade(B) in linear light

// Interpolation coefficients: per row, four playfield blocks of PF_STRIDE words:
// [flags][3 source][3 zoom][3 vertical][3 palette], each triple being
// native-anchored cubic increments c1, c2, c3 as float bits.
#define F3_INTERP_PF_STRIDE 13u
#define F3_INTERP_SOURCE 1u
#define F3_INTERP_ZOOM 4u
#define F3_INTERP_VERTICAL 7u
#define F3_INTERP_PALETTE 10u
#define F3_INTERP_FLAG_SOURCE 1u
#define F3_INTERP_FLAG_ZOOM 2u
#define F3_INTERP_FLAG_VERTICAL 4u
#define F3_INTERP_FLAG_PALETTE 8u
#define F3_INTERP_PALETTE_STRIDE_SHIFT 16u

#endif
