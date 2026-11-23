//! Blend-state accumulation, mechanically ported from `runtime/renderer/game/mix.hpp`.

use crate::game::SceneLayer;

/// Running blend state for one scanout pixel.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PixelMix {
    pub source: u16,
    pub destination: u16,
    pub source_weight: u8,
    pub destination_weight: u8,
    pub source_priority: u8,
    pub destination_priority: u8,
    pub source_mode: u8,
}

/// Fold one layer's blend state into the pixel.
pub fn mix(pixel: &mut PixelMix, layer: &SceneLayer, color: u16, select: bool, blend: &[u8; 4]) {
    if layer.blend_mode == pixel.source_mode {
        return;
    }
    if layer.priority > pixel.source_priority {
        let mut slot = usize::from(select);
        if layer.blend_mode == 1 || layer.blend_mode == 2 {
            if layer.blend_mode == 1 {
                slot += 2;
            }
            if blend[slot] == 0 {
                return;
            }
            pixel.source_weight = blend[slot];
        } else {
            if blend[slot] == 0 && blend[slot + 2] == 0 {
                return;
            }
            pixel.source_weight = blend[slot + 2];
            pixel.destination_weight = blend[slot];
            pixel.destination_priority = layer.priority;
            pixel.destination = color;
        }
        pixel.source = color;
        pixel.source_mode = layer.blend_mode;
        pixel.source_priority = layer.priority;
    } else if layer.priority >= pixel.destination_priority {
        pixel.destination = if layer.priority == pixel.destination_priority {
            0
        } else {
            color
        };
        pixel.destination_priority = layer.priority;
        pixel.destination_weight =
            blend[usize::from(select) + if pixel.source_mode == 1 { 0 } else { 2 }];
    }
}

/// FDA 15-bit palette word RRRRGGGGBBBBRGBx: nibbles shifted, low bits fill the lsb.
pub fn color15(color: u32) -> u32 {
    ((color & 0xf000) << 8)
        | ((color & 8) << 16)
        | ((color & 0x0f00) << 4)
        | ((color & 4) << 9)
        | (color & 0x00f0)
        | ((color & 2) << 2)
}

/// Resolve the pixel to ARGB using the palette.
pub fn rgb<const PALETTE_15: bool>(pixel: &PixelMix, palette: &[u32]) -> u32 {
    let fetch = |index: u16| {
        let color = palette[(index & 8191) as usize];
        if PALETTE_15 { color15(color) } else { color }
    };
    if pixel.source_weight == 8 && pixel.destination_weight == 0 {
        return 0xff00_0000 | fetch(pixel.source);
    }
    if pixel.source_weight == 0 && pixel.destination_weight == 8 {
        return 0xff00_0000 | fetch(pixel.destination);
    }
    let source = fetch(pixel.source);
    let destination = fetch(pixel.destination);
    let mut color = 0xff00_0000u32;
    for shift in [0u32, 8, 16] {
        let value = (((source >> shift) & 255) * u32::from(pixel.source_weight)
            + ((destination >> shift) & 255) * u32::from(pixel.destination_weight))
            >> 3;
        color |= value.min(255) << shift;
    }
    color
}

pub const fn background_mix(background: u16) -> PixelMix {
    PixelMix {
        source: 0,
        destination: background,
        source_weight: 0,
        destination_weight: 8,
        source_priority: 0,
        destination_priority: 0,
        source_mode: 255,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn layer(priority: u8, blend_mode: u8) -> SceneLayer {
        SceneLayer {
            priority,
            blend_mode,
            ..SceneLayer::default()
        }
    }

    #[test]
    fn higher_priority_layer_becomes_source() {
        let mut pixel = background_mix(9);
        let blend = [8, 4, 6, 2];
        mix(&mut pixel, &layer(3, 1), 42, false, &blend);
        assert_eq!(pixel.source, 42);
        assert_eq!(pixel.source_mode, 1);
        assert_eq!(pixel.source_priority, 3);
        assert_eq!(pixel.source_weight, blend[2]);
        assert_eq!(pixel.destination, 9);
    }

    #[test]
    fn same_mode_is_ignored() {
        let mut pixel = background_mix(9);
        pixel.source_mode = 2;
        let mut copy = pixel;
        mix(&mut copy, &layer(3, 2), 42, false, &[8; 4]);
        assert_eq!(copy, pixel);
    }

    #[test]
    fn zero_weights_skip_the_layer() {
        let mut pixel = background_mix(9);
        mix(&mut pixel, &layer(3, 1), 42, false, &[8, 8, 0, 8]);
        assert_eq!(pixel.source, 0, "zero source weight leaves the pixel alone");

        let mut pixel = background_mix(9);
        mix(&mut pixel, &layer(3, 0), 42, false, &[0, 8, 0, 8]);
        assert_eq!(pixel.source, 0);
    }

    #[test]
    fn lower_priority_sets_destination_without_touching_source_priority() {
        let mut pixel = PixelMix {
            source_priority: 3,
            destination_priority: 1,
            source_mode: 1,
            ..PixelMix::default()
        };
        mix(&mut pixel, &layer(2, 3), 7, true, &[1, 2, 3, 4]);
        assert_eq!(pixel.destination, 7);
        assert_eq!(pixel.destination_priority, 2);
        assert_eq!(
            pixel.destination_weight, 2,
            "source_mode 1 selects blend[1]"
        );
    }

    #[test]
    fn equal_priority_destination_is_background() {
        let mut pixel = PixelMix {
            source_priority: 5,
            destination_priority: 2,
            source_mode: 9,
            ..PixelMix::default()
        };
        mix(&mut pixel, &layer(2, 0), 7, false, &[5; 4]);
        assert_eq!(pixel.destination, 0);
        assert_eq!(pixel.destination_weight, 5);
    }

    #[test]
    fn color15_expands_five_bits_without_replication() {
        assert_eq!(color15(0xf000), 0xf0_0000);
        assert_eq!(color15(0x0f00), 0x00_f000);
        assert_eq!(color15(0x00f0), 0x0000_00f0);
        // Bit 3 (high G lsb) -> bit 19; bit 2 -> bit 11; bit 1 -> bit 3.
        assert_eq!(color15(8), 1 << 19);
        assert_eq!(color15(4), 1 << 11);
        assert_eq!(color15(2), 1 << 3);
    }

    #[test]
    fn rgb_fast_paths_and_blend() {
        let palette: Vec<u32> = (0..8192).map(|i| i as u32).collect();
        let opaque = PixelMix {
            source: 16,
            destination: 32,
            source_weight: 8,
            destination_weight: 0,
            ..PixelMix::default()
        };
        assert_eq!(rgb::<false>(&opaque, &palette), 0xff00_0000 | 16);
        let background = PixelMix {
            source: 16,
            destination: 32,
            source_weight: 0,
            destination_weight: 8,
            ..PixelMix::default()
        };
        assert_eq!(rgb::<false>(&background, &palette), 0xff00_0000 | 32);

        let blended = PixelMix {
            source: 0x40,
            destination: 0x80,
            source_weight: 4,
            destination_weight: 4,
            ..PixelMix::default()
        };
        assert_eq!(rgb::<false>(&blended, &palette), 0xff00_0060);
    }

    #[test]
    fn rgb_palette15_applies_color15() {
        let mut palette = vec![0u32; 8192];
        palette[1] = 0xf000;
        let pixel = PixelMix {
            source: 1,
            destination: 0,
            source_weight: 8,
            destination_weight: 0,
            ..PixelMix::default()
        };
        assert_eq!(rgb::<true>(&pixel, &palette), 0xfff0_0000);
        assert_eq!(rgb::<false>(&pixel, &palette), 0xff00_f000);
    }

    #[test]
    fn palette_index_masks_to_13_bits() {
        let palette: Vec<u32> = (0..8192).map(|i| i as u32).collect();
        let pixel = PixelMix {
            source: 8192 + 5,
            source_weight: 8,
            ..PixelMix::default()
        };
        assert_eq!(rgb::<false>(&pixel, &palette), 0xff00_0000 | 5);
    }
}
