//! Palette decode, mechanically ported from `runtime/renderer/game/palette.hpp`.

use crate::game::ram_shadow::{RamShadow, fresh_content_version};
use crate::game::scene::{SceneSource, VideoRam};
use crate::memory_map::PALETTE_RAM_SIZE;

/// Palette RAM at VBSTART, decoded to 0x00RRGGBB per entry. Palette RAM stores one big-endian
/// 32-bit word per entry with the color in bytes 1..3.
pub struct GamePalette {
    shadow: RamShadow<PALETTE_RAM_SIZE>,
    colors: [u32; Self::SIZE],
    version: u64,
}

impl GamePalette {
    pub const SIZE: usize = PALETTE_RAM_SIZE / 4;

    pub fn new() -> Self {
        Self {
            shadow: RamShadow::new(),
            colors: [0; Self::SIZE],
            version: fresh_content_version(),
        }
    }

    pub fn reset(&mut self) {
        self.colors.fill(0);
        self.shadow.invalidate();
        self.version = fresh_content_version();
    }

    /// Re-decode only when palette RAM changed since the previous decode. A `VideoRam` without
    /// full palette RAM leaves the colors untouched.
    pub fn decode(&mut self, vram: &VideoRam<'_>) {
        if vram.palette.len() < PALETTE_RAM_SIZE {
            return;
        }
        let palette: &[u8; PALETTE_RAM_SIZE] = vram.palette[..PALETTE_RAM_SIZE]
            .try_into()
            .expect("sliced to PALETTE_RAM_SIZE");
        if !self.shadow.changed(palette) {
            return;
        }
        for (i, color) in self.colors.iter_mut().enumerate() {
            *color = u32::from(palette[i * 4 + 1]) << 16
                | u32::from(palette[i * 4 + 2]) << 8
                | u32::from(palette[i * 4 + 3]);
        }
        self.version = fresh_content_version();
    }

    pub fn colors(&self) -> &[u32; Self::SIZE] {
        &self.colors
    }

    pub fn version(&self) -> u64 {
        self.version
    }
}

impl Default for GamePalette {
    fn default() -> Self {
        Self::new()
    }
}

impl SceneSource for GamePalette {
    fn reset(&mut self) {
        Self::reset(self);
    }

    fn decode(&mut self, vram: &VideoRam<'_>) {
        Self::decode(self, vram);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::memory_map::GRAPHICS_RAM_SIZE;

    fn set_color(palette: &mut [u8], at: usize, color: u32) {
        palette[at..at + 4].copy_from_slice(&color.to_be_bytes());
    }

    fn sources() -> (Vec<u8>, Vec<u8>, [u8; 0x20]) {
        (
            vec![0; GRAPHICS_RAM_SIZE],
            vec![0; PALETTE_RAM_SIZE],
            [0; 0x20],
        )
    }

    #[test]
    fn decodes_colors_from_palette_ram() {
        let (graphics, mut palette, control) = sources();
        set_color(&mut palette, 5 * 4, 0xff12_3456); // byte 0 is not part of the color
        set_color(&mut palette, 8191 * 4, 0x00ab_cdef);
        let mut decoded = GamePalette::new();
        decoded.decode(&VideoRam::with_palette(&graphics, &control, 1, &palette));
        assert_eq!(decoded.colors()[5], 0x0012_3456);
        assert_eq!(decoded.colors()[8191], 0x00ab_cdef);
        assert_eq!(decoded.colors()[6], 0);
    }

    #[test]
    fn skips_unchanged_ram_and_bumps_version_on_change() {
        let (graphics, mut palette, control) = sources();
        let mut decoded = GamePalette::new();
        let initial = decoded.version();
        set_color(&mut palette, 4, 0x0011_2233);
        decoded.decode(&VideoRam::with_palette(&graphics, &control, 1, &palette));
        let first = decoded.version();
        assert_ne!(first, initial);
        assert_eq!(decoded.colors()[1], 0x0011_2233);

        decoded.decode(&VideoRam::with_palette(&graphics, &control, 1, &palette));
        assert_eq!(
            decoded.version(),
            first,
            "unchanged palette RAM must not re-decode"
        );

        set_color(&mut palette, 4, 0x0044_5566);
        decoded.decode(&VideoRam::with_palette(&graphics, &control, 1, &palette));
        assert_ne!(decoded.version(), first);
        assert_eq!(decoded.colors()[1], 0x0044_5566);

        let current = decoded.version();
        decoded.decode(&VideoRam::new(&graphics, &control, 2));
        assert_eq!(
            decoded.version(),
            current,
            "no palette RAM leaves the colors untouched"
        );
        assert_eq!(decoded.colors()[1], 0x0044_5566);
    }

    #[test]
    fn reset_clears_colors_and_forces_redecode() {
        let (graphics, mut palette, control) = sources();
        set_color(&mut palette, 12, 0x0077_8899);
        let vram = VideoRam::with_palette(&graphics, &control, 1, &palette);
        let mut decoded = GamePalette::new();
        decoded.decode(&vram);
        let before = decoded.version();

        decoded.reset();
        assert!(
            decoded.version() > before,
            "reset bumps the monotonic version"
        );
        assert_eq!(decoded.colors()[3], 0);
        decoded.decode(&vram);
        assert_eq!(decoded.colors()[3], 0x0077_8899);
        assert!(decoded.version() > before);
    }

    #[test]
    fn empty_palette_span_is_ignored() {
        let (graphics, _, control) = sources();
        let mut decoded = GamePalette::new();
        let initial = decoded.version();
        decoded.decode(&VideoRam::new(&graphics, &control, 1));
        assert_eq!(decoded.version(), initial);
        assert_eq!(decoded.colors(), &[0; GamePalette::SIZE]);
    }

    #[test]
    fn short_palette_span_is_ignored() {
        let (graphics, _, control) = sources();
        let short = vec![0xffu8; PALETTE_RAM_SIZE - 1];
        let mut decoded = GamePalette::new();
        let initial = decoded.version();
        decoded.decode(&VideoRam::with_palette(&graphics, &control, 1, &short));
        assert_eq!(decoded.version(), initial);
        assert_eq!(decoded.colors()[0], 0);
    }
}
