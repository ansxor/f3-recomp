//! ROM graphics unpacking.

/// Decoded 16x16 6bpp sprite and playfield tiles.
#[derive(Clone, Debug, Default)]
pub struct GraphicsAssets {
    sprite_tile_mask: u32,
    playfield_tile_mask: u32,
    sprite_tile_count: u32,
    playfield_tile_count: u32,
    decoded_sprites: Vec<u8>,
    decoded_tiles: Vec<u8>,
}

impl GraphicsAssets {
    pub fn load_roms(
        &mut self,
        sprites: &[u8],
        sprites_hi: &[u8],
        tilemap: &[u8],
        tilemap_hi: &[u8],
    ) -> bool {
        if sprites.is_empty()
            || tilemap.is_empty()
            || !sprites.len().is_multiple_of(128)
            || !tilemap.len().is_multiple_of(128)
            || (!sprites_hi.is_empty() && sprites_hi.len() != sprites.len() / 2)
            || (!tilemap_hi.is_empty() && tilemap_hi.len() != tilemap.len() / 2)
        {
            return false;
        }

        let sprite_tiles = sprites.len() / 128;
        let playfield_tiles = tilemap.len() / 128;
        self.sprite_tile_count = sprite_tiles as u32;
        self.playfield_tile_count = playfield_tiles as u32;
        self.sprite_tile_mask = if (sprite_tiles & (sprite_tiles - 1)) != 0 {
            0
        } else {
            (sprite_tiles - 1) as u32
        };
        self.playfield_tile_mask = if (playfield_tiles & (playfield_tiles - 1)) != 0 {
            0
        } else {
            (playfield_tiles - 1) as u32
        };
        self.decoded_sprites = vec![0u8; sprite_tiles * 256];
        self.decoded_tiles = vec![0u8; playfield_tiles * 256];

        // Decode 16x16 6bpp sprites: low 4 bpp packed LSB, high 2 bpp
        for c in 0..sprite_tiles {
            let lo_tile = &sprites[c * 128..c * 128 + 128];
            let hi_tile = if sprites_hi.is_empty() {
                None
            } else {
                Some(&sprites_hi[c * 64..c * 64 + 64])
            };
            let dest = &mut self.decoded_sprites[c * 256..c * 256 + 256];

            for y in 0..16 {
                let lo_row = &lo_tile[y * 8..y * 8 + 8];
                let hi_row = hi_tile.map(|hi| &hi[y * 4..y * 4 + 4]);
                let dest_row = &mut dest[y * 16..y * 16 + 16];

                for x in 0..16 {
                    let lo = if x & 1 != 0 {
                        lo_row[x >> 1] >> 4
                    } else {
                        lo_row[x >> 1] & 0x0f
                    };
                    let hi = hi_row.map_or(0, |hi| ((hi[x >> 2] >> (2 * (x & 3))) & 0x03) << 4);
                    dest_row[x] = lo | hi;
                }
            }
        }

        // Decode 16x16 6bpp tilemaps: low 4 bpp packed LSB, high 2 bpp
        for c in 0..playfield_tiles {
            let lo_tile = &tilemap[c * 128..c * 128 + 128];
            let hi_tile = if tilemap_hi.is_empty() {
                None
            } else {
                Some(&tilemap_hi[c * 64..c * 64 + 64])
            };
            let dest = &mut self.decoded_tiles[c * 256..c * 256 + 256];

            for y in 0..16 {
                let lo_row = &lo_tile[y * 8..y * 8 + 8];
                let hi_row = hi_tile.map(|hi| &hi[y * 4..y * 4 + 4]);
                let dest_row = &mut dest[y * 16..y * 16 + 16];

                for x in 0..16 {
                    let lo = if x & 1 != 0 {
                        lo_row[x >> 1] >> 4
                    } else {
                        lo_row[x >> 1] & 0x0f
                    };
                    let hi = match hi_row {
                        Some(hi) if x < 8 => (((hi[1] >> x) & 1) << 5) | (((hi[0] >> x) & 1) << 4),
                        Some(hi) => {
                            (((hi[3] >> (x - 8)) & 1) << 5) | (((hi[2] >> (x - 8)) & 1) << 4)
                        }
                        None => 0,
                    };
                    dest_row[x] = lo | hi;
                }
            }
        }

        true
    }

    pub fn sprite_tiles(&self) -> &[u8] {
        &self.decoded_sprites
    }

    pub fn playfield_tiles(&self) -> &[u8] {
        &self.decoded_tiles
    }

    pub fn sprite_tile(&self, code: u32) -> &[u8] {
        Self::tile(
            &self.decoded_sprites,
            code,
            self.sprite_tile_count,
            self.sprite_tile_mask,
        )
    }

    pub fn playfield_tile(&self, code: u32) -> &[u8] {
        Self::tile(
            &self.decoded_tiles,
            code,
            self.playfield_tile_count,
            self.playfield_tile_mask,
        )
    }

    pub fn sprite_tile_count(&self) -> u32 {
        self.sprite_tile_count
    }

    pub fn playfield_tile_count(&self) -> u32 {
        self.playfield_tile_count
    }

    fn tile(pixels: &[u8], code: u32, count: u32, mask: u32) -> &[u8] {
        if count == 0 {
            return &[];
        }
        let index = if mask != 0 { code & mask } else { code % count };
        let start = index as usize * 256;
        &pixels[start..start + 256]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sprite_rom() -> Vec<u8> {
        let mut rom = vec![0u8; 128];
        // Row 0, first two bytes: lo nibbles 1 and 2.
        rom[0] = 0x12;
        rom
    }

    #[test]
    fn rejects_malformed_rom_sizes() {
        let mut assets = GraphicsAssets::default();
        assert!(!assets.load_roms(&[], &[], &[0; 128], &[]));
        assert!(!assets.load_roms(&[0; 128], &[], &[], &[]));
        assert!(!assets.load_roms(&[0; 127], &[], &[0; 128], &[]));
        assert!(!assets.load_roms(&[0; 128], &[0; 32], &[0; 128], &[]));
        assert!(!assets.load_roms(&[0; 128], &[], &[0; 128], &[0; 32]));
        assert!(assets.load_roms(&[0; 128], &[], &[0; 128], &[]));
    }

    #[test]
    fn decodes_low_nibbles_without_high_rom() {
        let mut assets = GraphicsAssets::default();
        assert!(assets.load_roms(&sprite_rom(), &[], &[0; 128], &[]));
        let tile = assets.sprite_tile(0);
        assert_eq!(tile.len(), 256);
        assert_eq!(tile[0], 2);
        assert_eq!(tile[1], 1);
        assert_eq!(tile[16], 0);
    }

    #[test]
    fn decodes_high_two_bits_for_sprites() {
        let mut rom = vec![0u8; 128];
        let mut hi = vec![0u8; 64];
        // x=0: hi bits from byte0 bit0 -> 0x10.
        hi[0] = 1;
        let mut assets = GraphicsAssets::default();
        assert!(assets.load_roms(&rom, &hi, &[0; 128], &[]));
        assert_eq!(assets.sprite_tile(0)[0], 0x10);
        // x=1: bits 2..3 of byte0 -> shifted to 0x20? bit 2 set -> high 2bpp value 1 at bits 4..5.
        rom[0] = 0;
        hi[0] = 0b0100;
        let mut assets = GraphicsAssets::default();
        assert!(assets.load_roms(&rom, &hi, &[0; 128], &[]));
        assert_eq!(assets.sprite_tile(0)[1], 0x10);
    }

    #[test]
    fn decodes_high_bits_for_tilemaps_in_two_halves() {
        let mut hi = vec![0u8; 64];
        // x=0: hi[0] bit0 -> 0x10. x=8: hi[2] bit0 -> 0x10.
        hi[0] = 1;
        hi[2] = 1;
        let mut assets = GraphicsAssets::default();
        assert!(assets.load_roms(&[0; 128], &[], &[0; 128], &hi));
        let tile = assets.playfield_tile(0);
        assert_eq!(tile[0], 0x10);
        assert_eq!(tile[8], 0x10);
    }

    #[test]
    fn tile_lookup_masks_power_of_two_and_modulo_otherwise() {
        let mut assets = GraphicsAssets::default();
        assert!(assets.load_roms(&[0; 256], &[], &[0; 384], &[]));
        assert_eq!(assets.sprite_tile_count(), 2);
        assert_eq!(assets.playfield_tile_count(), 3);
        // power of two: mask
        assert_eq!(
            assets.sprite_tile(2).as_ptr(),
            assets.sprite_tiles()[0..].as_ptr()
        );
        // non power of two: modulo
        assert_eq!(
            assets.playfield_tile(3).as_ptr(),
            assets.playfield_tiles()[0..].as_ptr()
        );
        assert_eq!(
            assets.playfield_tile(4).as_ptr(),
            assets.playfield_tiles()[256..].as_ptr()
        );
    }
}
