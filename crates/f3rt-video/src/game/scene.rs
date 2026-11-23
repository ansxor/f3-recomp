//! Scene model and video-RAM access, mechanically ported from
//! `runtime/renderer/game/scene.hpp`.
//!
//! The C++ header bakes `game_config::video` into constants; here the same values travel in
//! [`VideoConfig`] so one binary describes any ROM set.

use crate::config::VideoConfig;
use crate::memory_map::GRAPHICS_RAM_SIZE;

/// Hardware display-list limit: canonical (serialized) sprite lists never exceed it.
pub const MAX_HARDWARE_SPRITES: usize = 1024;
/// Render-only presented list (unit splices): full-detail splices expand spliced objects ~17x
/// (measured peak 1300 presented sprites in Command War attract), so it needs its own capacity.
/// Overflow is counted (`GameVideo::report`), never silent.
pub const MAX_PRESENTED_SPRITES: usize = 4096;

/// Video RAM as seen at VBSTART, plus the palette when the caller has it.
///
/// `graphics` is the 0x40000-byte graphics RAM, `control` the 0x20-byte control register file,
/// and `palette` optionally the 0x8000-byte palette RAM (empty = none). Data is big-endian as
/// stored by the 68020.
#[derive(Clone, Copy, Debug)]
pub struct VideoRam<'a> {
    pub graphics: &'a [u8],
    pub control: &'a [u8],
    /// `Machine::frame + 1`; diagnostics only.
    pub frame: u64,
    pub palette: &'a [u8],
}

impl<'a> VideoRam<'a> {
    pub const fn new(graphics: &'a [u8], control: &'a [u8], frame: u64) -> Self {
        Self {
            graphics,
            control,
            frame,
            palette: &[],
        }
    }

    pub const fn with_palette(
        graphics: &'a [u8],
        control: &'a [u8],
        frame: u64,
        palette: &'a [u8],
    ) -> Self {
        Self {
            graphics,
            control,
            frame,
            palette,
        }
    }

    pub fn u16(&self, offset: usize) -> u16 {
        u16::from(self.graphics[offset]) << 8 | u16::from(self.graphics[offset + 1])
    }

    pub fn control_u16(&self, index: usize) -> u16 {
        u16::from(self.control[index * 2]) << 8 | u16::from(self.control[index * 2 + 1])
    }

    /// True when the full graphics RAM is present; every decode below indexes it.
    pub fn has_graphics(&self) -> bool {
        self.graphics.len() >= GRAPHICS_RAM_SIZE
    }
}

/// A scene component decoded from video RAM at VBSTART.
pub trait SceneSource {
    fn reset(&mut self);
    fn decode(&mut self, vram: &VideoRam<'_>);
}

/// Tile index within an asset ROM of `count` 16x16 tiles (the ROM decides the wrap, not a fixed
/// 15-bit field): power-of-two ROMs mask, others take the remainder.
/// Mirrors the FDP's `wrap_tile`; GLSL `wrap_tile` in scene.glsl is the GPU twin.
pub const fn wrap_tile_index(code: u32, count: u32) -> u32 {
    if count & count.wrapping_sub(1) == 0 {
        code & count.wrapping_sub(1)
    } else {
        code % count
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ScenePixel {
    pub palette: u16,
    /// bit 4: nontransparent texel; bit 0: blend selector.
    pub flags: u8,
}

#[derive(Clone, Copy, Debug)]
pub struct SceneSprite {
    /// 24.8 coordinates in the 432x256 scanout space.
    pub x: i32,
    pub y: i32,
    pub scale_x: u16,
    pub scale_y: u16,
    pub tile: u32,
    pub palette: u8,
    pub flip_x: bool,
    pub flip_y: bool,
    pub group: u64,
    pub group_x: i32,
    pub group_y: i32,
    pub group_pc: u32,
    pub blend_weight: f32,
}

impl SceneSprite {
    pub fn weighted(&self) -> bool {
        self.blend_weight < 1.0
    }
}

impl PartialEq for SceneSprite {
    /// C++ `operator==` compares geometry and tile selection only (not group or weight).
    fn eq(&self, o: &Self) -> bool {
        self.x == o.x
            && self.y == o.y
            && self.scale_x == o.scale_x
            && self.scale_y == o.scale_y
            && self.tile == o.tile
            && self.palette == o.palette
            && self.flip_x == o.flip_x
            && self.flip_y == o.flip_y
    }
}

impl Default for SceneSprite {
    fn default() -> Self {
        Self {
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
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SceneLayer {
    pub priority: u8,
    pub blend_mode: u8,
    /// Raw mix-word fields. A set `c` bit enables that window, and a set `i` bit selects inside.
    pub clip_enabled: u8,
    pub clip_inverted: u8,
    pub clip_inverse: bool,
    pub enabled: bool,
    pub blend_select: bool,
    pub mosaic: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ScenePlayfield {
    pub layer: SceneLayer,
    /// 24.8 source coordinate at native screen column zero.
    pub source_x: i32,
    /// Integer source row before global screen flipping.
    pub source_y: i32,
    pub x_step: i32,
    pub y_step: i32,
    /// Native subpixel phase retained for high-resolution sampling.
    pub y_fraction: u8,
    pub palette_add: u16,
    /// PF2/PF3 alternate physical map (maps 4/5) selected by line RAM bit 0x200; only ever set
    /// when the game's `extended_alt_maps` layout is configured.
    pub alt_map: bool,
    /// The sampled map row has no nonzero tile code, so the hardware skips the layer on this
    /// scanline (`GameLines::cull_empty_rows`). Derived each frame, never serialized.
    pub empty_row: bool,
    /// Render-only presentation hint, derived each frame (`GameLines::resolve_alt_maps`), never
    /// serialized and never compared with the oracle: for an `alt_map` row, the offset `c` in
    /// `[0, pf_map_width_px)` with `alt(x, y) == main((2x + c) mod pf_map_width_px, y)` for every
    /// tile row the scanline samples; -1 = none.
    pub full_res_offset: i16,
}

impl Default for ScenePlayfield {
    fn default() -> Self {
        Self {
            layer: SceneLayer::default(),
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
}

/// The presented (expanded GPU/CPU output) view of one playfield row.
///
/// With `VideoConfig::full_resolution_alt_maps`, an `alt_map` row that has a solved
/// full-resolution twin samples the main map at twice the step: `x' = 2x + c`, so `alt_map`
/// clears, `x_step` doubles and `source_x` becomes `2*source_x + (c << 8)` wrapped to the map
/// period (24.8). Vertical geometry is unchanged. Canonical rows, native composition and
/// snapshots never call this.
pub fn presented_playfield(playfield: &ScenePlayfield, video: &VideoConfig) -> ScenePlayfield {
    if video.full_resolution_alt_maps && playfield.alt_map && playfield.full_res_offset >= 0 {
        let mut presented = *playfield;
        presented.alt_map = false;
        presented.x_step = playfield.x_step * 2;
        let period = video.pf_map_width_px() * 256 - 1;
        presented.source_x = (((playfield.source_x as u32)
            .wrapping_mul(2)
            .wrapping_add(u32::from(playfield.full_res_offset as u16) << 8))
            & period) as i32;
        return presented;
    }
    *playfield
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SceneClip {
    /// Half-open scanout coordinates, calibration applied.
    pub left: i16,
    pub right: i16,
}

/// Scene layer numbering shared by the GPU scene encoding, sprite-plane bits, shaders and
/// `--video-layer-mask`: 0..3 playfields, 4..7 sprites, 8 text. Values are fixed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum LayerId {
    Pf0 = 0,
    Pf1,
    Pf2,
    Pf3,
    Sp0,
    Sp1,
    Sp2,
    Sp3,
    Text,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LayerKind {
    Playfield,
    Sprite,
    Text,
}

pub const LAYER_COUNT: usize = 9;
pub const ALL_LAYERS: u32 = (1 << LAYER_COUNT) - 1;

pub const fn layer_bit(id: LayerId) -> u32 {
    1u32 << id as u32
}

pub const fn kind(id: LayerId) -> LayerKind {
    match id as u8 {
        0..=3 => LayerKind::Playfield,
        8 => LayerKind::Text,
        _ => LayerKind::Sprite,
    }
}

pub const fn sub_index(id: LayerId) -> usize {
    match kind(id) {
        LayerKind::Playfield => id as usize,
        LayerKind::Sprite => id as usize - LayerId::Sp0 as usize,
        LayerKind::Text => 0,
    }
}

pub const fn playfield(i: u32) -> LayerId {
    match i {
        0 => LayerId::Pf0,
        1 => LayerId::Pf1,
        2 => LayerId::Pf2,
        _ => LayerId::Pf3,
    }
}

pub const fn sprite(i: u32) -> LayerId {
    match i {
        0 => LayerId::Sp0,
        1 => LayerId::Sp1,
        2 => LayerId::Sp2,
        _ => LayerId::Sp3,
    }
}

/// An indexed sprite-plane value carries its sprite layer in bits 10-11.
pub const fn sprite_layer(plane_value: u16) -> LayerId {
    sprite(((plane_value >> 10) & 3) as u32)
}

pub struct LayerInfo {
    pub name: &'static str,
    pub width: u16,
    pub height: u16,
    /// Sampling-domain suffix; reports print `<width>x<height>-<domain>`.
    pub domain: &'static str,
}

/// Per-layer sampling domains in `LayerId` order.
pub fn layer_info(video: &VideoConfig) -> [LayerInfo; LAYER_COUNT] {
    let pf_width = video.pf_map_width_px() as u16;
    let sprite_width = crate::config::NATIVE_WIDTH as u16;
    let sprite_height = crate::config::FRAME_LINES as u16;
    [
        LayerInfo {
            name: "pf0",
            width: pf_width,
            height: 512,
            domain: "indexed-texture",
        },
        LayerInfo {
            name: "pf1",
            width: pf_width,
            height: 512,
            domain: "indexed-texture",
        },
        LayerInfo {
            name: "pf2",
            width: pf_width,
            height: 512,
            domain: "indexed-texture",
        },
        LayerInfo {
            name: "pf3",
            width: pf_width,
            height: 512,
            domain: "indexed-texture",
        },
        LayerInfo {
            name: "sp0",
            width: sprite_width,
            height: sprite_height,
            domain: "next-sprite-plane",
        },
        LayerInfo {
            name: "sp1",
            width: sprite_width,
            height: sprite_height,
            domain: "next-sprite-plane",
        },
        LayerInfo {
            name: "sp2",
            width: sprite_width,
            height: sprite_height,
            domain: "next-sprite-plane",
        },
        LayerInfo {
            name: "sp3",
            width: sprite_width,
            height: sprite_height,
            domain: "next-sprite-plane",
        },
        LayerInfo {
            name: "text",
            width: 512,
            height: 512,
            domain: "indexed-texture",
        },
    ]
}

/// Layers with scroll geometry (playfields and text).
pub const SCROLLED_LAYERS: [LayerId; 5] = [
    LayerId::Pf0,
    LayerId::Pf1,
    LayerId::Pf2,
    LayerId::Pf3,
    LayerId::Text,
];

/// One scanout row of decoded scene state.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SceneRow {
    pub playfields: [ScenePlayfield; 4],
    pub sprites: [SceneLayer; 4],
    pub text: SceneLayer,
    pub clips: [SceneClip; 4],
    /// Saturated weights, 0..8.
    pub blend: [u8; 4],
    pub background: u16,
    pub text_x: i16,
    pub text_y: i16,
    pub mosaic_period: u8,
    pub bitmap: bool,
    /// Line RAM 0x6000 section word 2 bits 14/13 (MAME y-ack/fdp-collapse): 15-bit palette words
    /// and a horizontal two-pixel blur. Not serialized: rows are re-decoded every VBSTART.
    pub palette_15bit: bool,
    pub blur: bool,
}

impl SceneRow {
    pub fn layer(&self, id: LayerId) -> &SceneLayer {
        match kind(id) {
            LayerKind::Playfield => &self.playfields[sub_index(id)].layer,
            LayerKind::Sprite => &self.sprites[sub_index(id)],
            LayerKind::Text => &self.text,
        }
    }

    pub fn layer_mut(&mut self, id: LayerId) -> &mut SceneLayer {
        match kind(id) {
            LayerKind::Playfield => &mut self.playfields[sub_index(id)].layer,
            LayerKind::Sprite => &mut self.sprites[sub_index(id)],
            LayerKind::Text => &mut self.text,
        }
    }
}

impl Default for SceneRow {
    fn default() -> Self {
        Self {
            playfields: [ScenePlayfield::default(); 4],
            sprites: [SceneLayer::default(); 4],
            text: SceneLayer::default(),
            clips: [SceneClip::default(); 4],
            blend: [0; 4],
            background: 0,
            text_x: 0,
            text_y: 0,
            mosaic_period: 16,
            bitmap: false,
            palette_15bit: false,
            blur: false,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layer_numbering_is_fixed() {
        assert_eq!(LayerId::Text as u8, 8);
        assert_eq!(LAYER_COUNT, 9);
        assert_eq!(ALL_LAYERS, 0x1ff);
        assert_eq!(kind(LayerId::Pf3), LayerKind::Playfield);
        assert_eq!(kind(LayerId::Sp0), LayerKind::Sprite);
        assert_eq!(kind(LayerId::Text), LayerKind::Text);
        assert_eq!(sub_index(LayerId::Sp2), 2);
        assert_eq!(layer_bit(LayerId::Sp1), 0x20);
        assert_eq!(sprite_layer(0x0c00), LayerId::Sp3);
        assert_eq!(sprite_layer(0x0400), LayerId::Sp1);
    }

    #[test]
    fn tile_wrap_masks_power_of_two_roms_and_remainders_others() {
        assert_eq!(wrap_tile_index(0x1234, 1024), 0x234);
        assert_eq!(wrap_tile_index(10, 3), 1);
        assert_eq!(wrap_tile_index(0x8001, 0), 0x8001);
        assert_eq!(wrap_tile_index(5, 1), 0);
    }

    #[test]
    fn video_ram_reads_big_endian_words() {
        let mut graphics = [0u8; 4];
        graphics[0] = 0x12;
        graphics[1] = 0x34;
        graphics[2] = 0xab;
        graphics[3] = 0xcd;
        let control = [0x11, 0x22, 0x33, 0x44];
        let vram = VideoRam::new(&graphics, &control, 7);
        assert_eq!(vram.u16(0), 0x1234);
        assert_eq!(vram.u16(2), 0xabcd);
        assert_eq!(vram.control_u16(0), 0x1122);
        assert_eq!(vram.control_u16(1), 0x3344);
        assert_eq!(vram.frame, 7);
        assert!(vram.palette.is_empty());
        assert!(!vram.has_graphics());
    }

    #[test]
    fn presented_playfield_remaps_only_solved_alt_rows() {
        let video = VideoConfig {
            full_resolution_alt_maps: true,
            extended_alt_maps: true,
            ..VideoConfig::default()
        };
        let source = ScenePlayfield {
            alt_map: true,
            x_step: 128,
            source_x: 1000,
            full_res_offset: 5,
            ..ScenePlayfield::default()
        };

        let presented = presented_playfield(&source, &video);
        assert!(!presented.alt_map);
        assert_eq!(presented.x_step, 256);
        assert_eq!(
            presented.source_x,
            ((1000u32 * 2 + (5u32 << 8)) & (1024 * 256 - 1)) as i32
        );

        // Unset offset or disabled presentation keeps the canonical row.
        let unsolved = ScenePlayfield {
            full_res_offset: -1,
            ..source
        };
        assert_eq!(presented_playfield(&unsolved, &video), unsolved);

        let disabled = VideoConfig {
            full_resolution_alt_maps: false,
            ..video
        };
        assert_eq!(presented_playfield(&source, &disabled), source);

        // Wrapping to the map period.
        let wrapping = ScenePlayfield {
            source_x: 131_000,
            ..source
        };
        let presented = presented_playfield(&wrapping, &video);
        assert!((0..1024 * 256).contains(&presented.source_x));
    }

    #[test]
    fn playfield_layers_follow_the_configured_width() {
        let extended = layer_info(&VideoConfig::default());
        assert_eq!(extended[0].width, 1024);
        assert_eq!(extended[8].width, 512);
        assert_eq!(extended[4].height, 256);

        let standard = layer_info(&VideoConfig {
            extend: false,
            ..VideoConfig::default()
        });
        assert_eq!(standard[0].width, 512);
    }
}
