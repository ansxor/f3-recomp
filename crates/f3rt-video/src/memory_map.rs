//! Video-relevant constants of the F3 bus map, ported from `include/f3rt/memory_map.hpp`.
//!
//! Only the graphics/palette regions the renderer decodes live here; the rest of the machine
//! map stays in the C++ runtime.

/// Palette RAM (0x440000-0x448000, 8 KiB).
pub const PALETTE_RAM_SIZE: usize = 0x8000;

/// Graphics RAM (0x600000-0x640000, 256 KiB).
pub const GRAPHICS_RAM_SIZE: usize = 0x40000;

/// Sprite display list (`OFFS_SPRITERAM`).
pub const OFFS_SPRITERAM: usize = 0x0000;
pub const SPRITE_BANK_STRIDE: usize = 0x8000;
/// Playfield tilemaps (`OFFS_PLAYFIELDS`).
pub const OFFS_PLAYFIELDS: usize = 0x10000;
/// 8 KiB per playfield layer (extended mode, 64 columns x 32 rows).
pub const PLAYFIELD_LAYER_SIZE: usize = 0x2000;
/// 4 KiB per playfield layer (standard mode, 32 columns x 32 rows).
pub const PLAYFIELD_STANDARD_LAYER_SIZE: usize = 0x1000;
/// Text tilemap (`OFFS_TEXTRAM_MAP`).
pub const OFFS_TEXTRAM_MAP: usize = 0x1c000;
/// Text glyph/font patterns (`OFFS_TEXTRAM_GLYPHS`), 8 KiB.
pub const OFFS_TEXTRAM_GLYPHS: usize = 0x1e000;
pub const TEXTRAM_GLYPH_SIZE: usize = 0x2000;
/// 64x64 = 4096 text tiles.
pub const TEXTRAM_TILES: usize = 0x1000;
/// Line RAM (`OFFS_LINERAM`).
pub const OFFS_LINERAM: usize = 0x20000;
pub const LINERAM_SIZE: usize = 0x10000;
/// Pivot RAM (`OFFS_PIVOT`).
pub const OFFS_PIVOT: usize = 0x30000;

/// Base of the section line tables inside line RAM (0x624000).
pub const LINERAM_SECTIONS_OFFSET: usize = 0x4000;
/// 4 KiB per section.
pub const LINERAM_SECTION_STRIDE: usize = 0x1000;
/// 512 bytes per subsection (256 scanline words).
pub const LINERAM_SUBSECTION_STRIDE: usize = 0x200;
/// 512 bytes per section latch table.
pub const LINERAM_LATCH_STRIDE: usize = 0x200;
/// Bank offset for the alternate line table.
pub const LINERAM_BANK_OFFSET: usize = 0x800;

/// Control register words are held at 0x660000; the text Y scroll is `control_1[5]`.
pub const CONTROL_TEXT_SCROLL_Y_WORD: usize = 8 + 5;
