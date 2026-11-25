//! TC0630FDP character RAM and sprite display-list decoding.

use crate::memory_map::SPRITE_BANK_STRIDE;

/// One 8x8 4bpp character tile: 32 raw bytes -> 64 packed-nibble pixels
/// (`row[3 - x/2] >> ((x & 1) * 4)`).
pub fn decode_charram_tile(tile_src: &[u8], dest: &mut [u8]) {
    for y in 0..8 {
        let row = &tile_src[y * 4..y * 4 + 4];
        for x in 0..8 {
            dest[y * 8 + x] = (row[3 - x / 2] >> ((x & 1) * 4)) & 0x0f;
        }
    }
}

/// One list entry of the sprite display list (`DecodedSpriteEntry`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DecodedSpriteEntry {
    pub x: i32,
    pub y: i32,
    pub scale_x: i32,
    pub scale_y: i32,
    pub tile: u32,
    pub color: u8,
    pub flip_x: bool,
    pub flip_y: bool,
    pub pri: u8,
    pub group: u64,
    pub group_x: i32,
    pub group_y: i32,
    pub group_pc: u32,
}

/// Hardware sprite display-list state that persists across frames.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SpriteRamState {
    pub flipscreen: bool,
    pub bank: bool,
    pub trails: bool,
    pub extra_planes: u8,
    pub pen_mask: u8,
}

impl Default for SpriteRamState {
    fn default() -> Self {
        Self {
            flipscreen: false,
            bank: false,
            trails: false,
            extra_planes: 0,
            pen_mask: 0x0f,
        }
    }
}

pub use f3rt_sprites::presentation::{
    SPRITE_ENTRIES, SPRITE_SLOTS, SpriteGroupSlot, group_key_base, group_key_class, pack_group_key,
    sprite_slot_for, sprite_slot_listed,
};

#[derive(Clone, Copy, Debug, Default)]
pub struct SpritePresentation<'a> {
    pub groups: &'a [SpriteGroupSlot],
}

impl SpritePresentation<'_> {
    pub fn empty(&self) -> bool {
        self.groups.is_empty()
    }
}

const H_START: i32 = 46;
const H_VIS: i32 = 320;

fn read_be16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_be_bytes([bytes[offset], bytes[offset + 1]])
}

/// Producer axis accumulator shared by X and Y.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct SpriteAxis {
    block_scale: i32,
    pos: i32,
    block_pos: i32,
    global: i16,
    subglobal: i16,
}

impl Default for SpriteAxis {
    fn default() -> Self {
        Self {
            block_scale: 1 << 8,
            pos: 0,
            block_pos: 0,
            global: 0,
            subglobal: 0,
        }
    }
}

impl SpriteAxis {
    fn update(&mut self, scroll: u8, posw: u16, multi: bool, block_ctrl: u8, new_zoom: u8) {
        let mut new_pos = if (posw & 0x800) != 0 {
            (posw | 0xf000) as i16
        } else {
            (posw & 0xfff) as i16
        };
        if scroll & 0x01 != 0 {
            self.subglobal = new_pos;
        }
        if scroll & 0x02 != 0 {
            self.global = new_pos;
        }
        if scroll & 0x08 == 0 {
            new_pos = new_pos.wrapping_add(self.global);
            if scroll & 0x04 == 0 {
                new_pos = new_pos.wrapping_add(self.subglobal);
            }
        }

        match block_ctrl {
            0b00 => {
                if !multi {
                    self.block_pos = i32::from(new_pos) << 8;
                    self.block_scale = 0x100 - i32::from(new_zoom);
                }
                self.pos = self.block_pos;
            }
            0b10 => self.pos = self.block_pos,
            0b11 => self.pos += self.block_scale * 16,
            _ => {}
        }
    }
}

struct DecodeState<'a> {
    x: SpriteAxis,
    y: SpriteAxis,
    color: u8,
    multi: bool,
    count: usize,
    block_group: SpriteGroupSlot,
    out: &'a mut [DecodedSpriteEntry],
    ram: &'a mut SpriteRamState,
    visible: std::ops::Range<i32>,
    apply_flip: bool,
}

/// One list entry through the hardware state machine. Returns false when the entry is a
/// self-jump (list end). Splice replacements cannot jump.
impl DecodeState<'_> {
    fn step(&mut self, spr: &[u8], jump: Option<&mut i32>, mut group: SpriteGroupSlot) -> bool {
        let w0 = read_be16(spr, 0);
        let w1 = read_be16(spr, 2);
        let w2 = read_be16(spr, 4);
        let w3 = read_be16(spr, 6);
        let w4 = read_be16(spr, 8);
        let w5 = read_be16(spr, 10);
        let w6 = read_be16(spr, 12);

        // Special command bit in word 3
        if w3 & 0x8000 != 0 {
            self.ram.flipscreen = (w5 & 0x2000) != 0;
            self.ram.extra_planes = ((w5 >> 8) & 3) as u8;
            self.ram.pen_mask = (self.ram.extra_planes << 4) | 0x0f;
            self.ram.trails = (w5 & 0x0002) != 0;
            self.ram.bank = (w5 & 0x0001) != 0;
        }

        // Sprite list jump bit in word 6
        if (w6 & 0x8000) != 0
            && let Some(offs) = jump
        {
            let new_offs = i32::from(w6 & 0x03ff);
            if new_offs == *offs {
                return false;
            }
            *offs = new_offs - 1;
        }

        let spritecont = (w4 >> 8) as u8;
        let lock = (spritecont & 0x04) != 0;
        if !lock {
            self.color = (w4 & 0xff) as u8;
        }

        if self.multi || ((spritecont & 0xf0) != 0 && group.key == 0) {
            group = self.block_group;
        } else {
            self.block_group = group;
        }

        let scroll_mode = ((w2 >> 12) & 0x0f) as u8;
        self.x.update(
            scroll_mode,
            w2 & 0x0fff,
            self.multi,
            (spritecont >> 6) & 3,
            (w1 & 0xff) as u8,
        );
        self.y.update(
            scroll_mode,
            w3 & 0x0fff,
            self.multi,
            (spritecont >> 4) & 3,
            (w1 >> 8) as u8,
        );
        self.multi = (spritecont & 0x08) != 0;

        let tile = u32::from(w0) | (u32::from(w5 & 0x0001) << 16);
        if tile == 0 {
            return true;
        }

        let tx = if self.ram.flipscreen {
            (512 << 8) - self.x.block_scale * 16 - self.x.pos
        } else {
            self.x.pos
        };
        let ty = if self.ram.flipscreen {
            (256 << 8) - self.y.block_scale * 16 - self.y.pos
        } else {
            self.y.pos
        };

        // Cull against the configured scanout crop, not the top edge.
        if tx + self.x.block_scale * 16 <= (H_START << 8)
            || tx > ((H_START + H_VIS - 1) << 8)
            || ty + self.y.block_scale * 16 <= (self.visible.start << 8)
            || ty > ((self.visible.end - 1) << 8)
        {
            return true;
        }

        if self.count >= self.out.len() {
            return true;
        }

        let flip_x = (spritecont & 0x01) != 0;
        let flip_y = (spritecont & 0x02) != 0;

        let color = self.color;
        let s = &mut self.out[self.count];
        self.count += 1;
        s.x = if self.apply_flip { tx } else { self.x.pos };
        s.y = if self.apply_flip { ty } else { self.y.pos };
        s.scale_x = self.x.block_scale;
        s.scale_y = self.y.block_scale;
        s.tile = tile;
        s.color = color;
        s.flip_x = if self.apply_flip && self.ram.flipscreen {
            !flip_x
        } else {
            flip_x
        };
        s.flip_y = if self.apply_flip && self.ram.flipscreen {
            !flip_y
        } else {
            flip_y
        };
        s.pri = (color >> 6) & 3;
        s.group = group.key;
        s.group_x = group.x;
        s.group_y = group.y;
        s.group_pc = group.pc;
        true
    }
}

/// Walk the sprite display list in `spriteram` (0x10000 bytes, base 0x600000) following block
/// chaining, the jump word and command entries. Returns the number of entries written to `out`.
pub fn decode_sprite_list(
    spriteram: &[u8],
    visible: std::ops::Range<i32>,
    out: &mut [DecodedSpriteEntry],
    state: &mut SpriteRamState,
    apply_flip: bool,
    presentation: Option<&SpritePresentation<'_>>,
) -> usize {
    let mut ctx = DecodeState {
        x: SpriteAxis::default(),
        y: SpriteAxis::default(),
        color: 0,
        multi: false,
        count: 0,
        block_group: SpriteGroupSlot::default(),
        out,
        ram: state,
        visible,
        apply_flip,
    };
    let empty_groups: &[SpriteGroupSlot] = &[];
    let groups = presentation.map_or(empty_groups, |p| p.groups);

    let mut total_sprites = 0;
    let mut offs: i32 = 0;
    while offs < 0x400 && total_sprites < 0x400 {
        total_sprites += 1;
        let bank_offset = if ctx.ram.bank {
            SPRITE_BANK_STRIDE as u32
        } else {
            0
        };
        let entry_start = bank_offset as usize + offs as usize * 16;
        let spr = &spriteram[entry_start..entry_start + 16];
        let slot = (if ctx.ram.bank { 0x400 } else { 0 }) + offs as usize;
        let group = groups.get(slot).copied().unwrap_or_default();
        if !ctx.step(spr, Some(&mut offs), group) {
            break;
        }
        offs += 1;
    }
    ctx.count
}

/// Decode a sequence of standalone emitted 16-byte sprite entries using the exact same
/// hardware state machine (`DecodeState`) with a fixed global scroll bias `(bias_x, bias_y)`.
pub fn decode_emitted_entries(
    entries: &[f3rt_sprites::commandw::EmittedEntry],
    visible: std::ops::Range<i32>,
    bias_x: i16,
    bias_y: i16,
    flipscreen: bool,
    apply_flip: bool,
    out: &mut [DecodedSpriteEntry],
) -> usize {
    let mut ram = SpriteRamState {
        flipscreen,
        ..SpriteRamState::default()
    };
    let mut ctx = DecodeState {
        x: SpriteAxis {
            global: bias_x,
            ..SpriteAxis::default()
        },
        y: SpriteAxis {
            global: bias_y,
            ..SpriteAxis::default()
        },
        color: 0,
        multi: false,
        count: 0,
        block_group: SpriteGroupSlot::default(),
        out,
        ram: &mut ram,
        visible,
        apply_flip,
    };
    for entry in entries {
        ctx.step(&entry.bytes, None, entry.group);
    }
    ctx.count
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write_entry(ram: &mut [u8], bank: bool, index: usize, entry: [u16; 7]) {
        let base = if bank { SPRITE_BANK_STRIDE } else { 0 } + index * 16;
        for (i, word) in entry.iter().enumerate() {
            ram[base + i * 2..base + i * 2 + 2].copy_from_slice(&word.to_be_bytes());
        }
    }

    #[test]
    fn charram_tile_nibbles() {
        let mut src = [0u8; 32];
        // Row 0: bytes 0..4 = [0x10, 0x32, 0x54, 0x76]
        src[0..4].copy_from_slice(&[0x10, 0x32, 0x54, 0x76]);
        let mut dest = [0xffu8; 64];
        decode_charram_tile(&src, &mut dest);
        // x:0 -> row[3] low nibble = 6; x:1 -> row[3] high = 7; x:2 -> row[2] low = 4; ...
        assert_eq!(&dest[0..8], &[6, 7, 4, 5, 2, 3, 0, 1]);
        assert_eq!(&dest[8..64], &[0u8; 56]);
    }

    #[test]
    fn decodes_a_visible_sprite() {
        let mut ram = vec![0u8; 0x10000];
        // x.pos = 100 << 8 -> block_pos when scroll mode has no scroll flags and ctrl 00.
        // w2 high nibble 0 (scroll 0), w2 low 12 bits 100. y likewise.
        write_entry(&mut ram, false, 0, [1, 0, 100, 200, 0, 0, 0]);
        write_entry(&mut ram, false, 1, [0, 0, 0, 0, 0, 0, 0x8001]); // self jump ends the list
        let mut out = [DecodedSpriteEntry::default(); 4];
        let mut state = SpriteRamState::default();
        let count = decode_sprite_list(&ram, 24..256, &mut out, &mut state, false, None);
        assert_eq!(count, 1);
        assert_eq!(out[0].tile, 1);
        assert_eq!(out[0].x, 100 << 8);
        assert_eq!(out[0].y, 200 << 8);
        assert_eq!(out[0].scale_x, 0x100);
        assert_eq!(out[0].scale_y, 0x100);
    }

    #[test]
    fn culls_outside_the_scanout_crop() {
        let mut ram = vec![0u8; 0x10000];
        write_entry(&mut ram, false, 0, [1, 0, 100, 0, 0, 0, 0x8000]);
        let mut out = [DecodedSpriteEntry::default(); 4];
        let mut state = SpriteRamState::default();
        assert_eq!(
            decode_sprite_list(&ram, 24..256, &mut out, &mut state, false, None,),
            0,
            "y = 0 is above the crop"
        );
    }

    #[test]
    fn command_entry_updates_hardware_state() {
        let mut ram = vec![0u8; 0x10000];
        // Special command: w3 bit15 set; w5 bits: flipscreen 0x2000, trails 0x0002, bank 0x0001,
        // extra planes in bits 8..9.
        write_entry(
            &mut ram,
            false,
            0,
            [0, 0, 0, 0x8000, 0, 0x2003 | (2 << 8), 0x8000],
        );
        let mut out = [DecodedSpriteEntry::default(); 4];
        let mut state = SpriteRamState::default();
        assert_eq!(
            decode_sprite_list(&ram, 24..256, &mut out, &mut state, false, None,),
            0
        );
        assert!(state.flipscreen);
        assert!(state.trails);
        assert!(state.bank);
        assert_eq!(state.extra_planes, 2);
        assert_eq!(state.pen_mask, 0x2f);
    }

    #[test]
    fn jump_to_itself_ends_the_list() {
        let mut ram = vec![0u8; 0x10000];
        write_entry(&mut ram, false, 0, [0, 0, 0, 0, 0, 0, 0x8000]);
        let mut out = [DecodedSpriteEntry::default(); 4];
        let mut state = SpriteRamState::default();
        assert_eq!(
            decode_sprite_list(&ram, 24..256, &mut out, &mut state, false, None,),
            0
        );
    }

    #[test]
    fn groups_follow_block_heads_and_empty_presentation_is_neutral() {
        let mut ram = vec![0; 0x10000];
        // Entry 0 (head, multi=1 for next), entry 1 (last tile of block, multi=0 for next),
        // entry 2 (independent head).
        write_entry(&mut ram, false, 0, [1, 0, 100, 50, 0x0800, 0, 0]);
        write_entry(&mut ram, false, 1, [2, 0, 120, 50, 0x7400, 0, 0]);
        write_entry(&mut ram, false, 2, [3, 0, 200, 60, 0, 0, 0]);
        let decode = |presentation: Option<&SpritePresentation<'_>>| {
            let mut out = [DecodedSpriteEntry::default(); 4];
            let n = decode_sprite_list(
                &ram,
                24..256,
                &mut out,
                &mut SpriteRamState::default(),
                false,
                presentation,
            );
            assert_eq!(n, 3);
            out
        };
        let baseline = decode(None);
        assert_eq!(baseline[0].x, 100 * 256);
        assert_eq!(baseline[2].x, 200 * 256);
        assert_eq!(baseline[2].tile, 3);
        assert_eq!(decode(Some(&SpritePresentation::default())), baseline);

        let head = SpriteGroupSlot {
            key: 42,
            x: 100 * 256,
            y: 50 * 256,
            pc: 0x1000,
        };
        let stale = SpriteGroupSlot {
            key: 99,
            x: 999 * 256,
            y: 999 * 256,
            pc: 0x9999,
        };
        let independent = SpriteGroupSlot {
            key: 44,
            x: 200 * 256,
            y: 60 * 256,
            pc: 0x2000,
        };
        let mut expected = baseline;
        for (entry, group) in expected.iter_mut().zip([head, head, independent]) {
            entry.group = group.key;
            entry.group_x = group.x;
            entry.group_y = group.y;
            entry.group_pc = group.pc;
        }
        assert_eq!(
            decode(Some(&SpritePresentation {
                groups: &[head, stale, independent],
            })),
            expected
        );
    }

    proptest::proptest! {
        #![proptest_config(proptest::test_runner::Config::with_cases(384))]

        #[test]
        fn multi_tile_group_matches_logical_blocks(
            blocks in proptest::collection::vec((1usize..=8, proptest::bool::ANY, 0u8..4), 0..30),
            bank in proptest::bool::ANY,
            flipscreen in proptest::bool::ANY,
        ) {
            let mut ram = vec![0; 0x10000];
            let mut groups = vec![SpriteGroupSlot::default(); SPRITE_SLOTS];
            let mut expected = Vec::new();
            let bank_start = if bank { usize::from(SPRITE_ENTRIES) } else { 0 };
            for (block, (count, resolved, stale)) in blocks.into_iter().enumerate() {
                let head = if resolved {
                    SpriteGroupSlot { key: block as u64 + 1, x: 127, y: -345, pc: 99 + block as u32 }
                } else {
                    SpriteGroupSlot::default()
                };
                for member in 0..count {
                    let entry = expected.len();
                    let tile = entry as u16 + 1;
                    let control = match (member == 0, member + 1 == count) {
                        (true, true) => 0x0000,
                        (true, false) => 0x0800,
                        (false, true) => 0x7400,
                        (false, false) => 0x7c00,
                    };
                    write_entry(&mut ram, bank, entry, [tile, 0xc080, 260, 100, control, 0, 0]);
                    groups[bank_start + entry] = if member == 0 {
                        head
                    } else if stale & (1 << (member % 2)) != 0 {
                        SpriteGroupSlot { key: 900 + entry as u64, x: -1, y: 17, pc: 65 }
                    } else {
                        SpriteGroupSlot::default()
                    };
                    expected.push((u32::from(tile), head));
                }
            }
            let end = expected.len();
            write_entry(&mut ram, bank, end, [0, 0, 0, 0, 0, 0, 0x8000 | end as u16]);
            let mut out = vec![DecodedSpriteEntry::default(); end + 1];
            let count = decode_sprite_list(
                &ram, 0..256, &mut out,
                &mut SpriteRamState { bank, flipscreen, ..Default::default() }, true,
                Some(&SpritePresentation { groups: &groups }),
            );
            let actual: Vec<_> = out[..count].iter().map(|tile| (tile.tile, SpriteGroupSlot {
                key: tile.group, x: tile.group_x, y: tile.group_y, pc: tile.group_pc,
            })).collect();
            proptest::prop_assert_eq!(actual, expected);
        }
    }

    #[test]
    fn flipscreen_mirrors_when_apply_flip() {
        let mut ram = vec![0u8; 0x10000];
        write_entry(&mut ram, false, 0, [0, 0, 0, 0x8000, 0, 0x2000, 0]);
        write_entry(&mut ram, false, 1, [1, 0, 300, 200, 0, 0, 0]);
        write_entry(&mut ram, false, 2, [0, 0, 0, 0, 0, 0, 0x8002]);
        let mut out = [DecodedSpriteEntry::default(); 4];
        let mut state = SpriteRamState::default();
        let count = decode_sprite_list(&ram, 24..256, &mut out, &mut state, true, None);
        assert_eq!(count, 1);
        assert_eq!(out[0].x, (512 << 8) - 0x100 * 16 - (300 << 8));
        assert!(out[0].flip_x);
        assert!(out[0].flip_y);
    }

    #[test]
    fn presentation_helpers() {
        assert_eq!(sprite_slot_for(0x8000), 0x400);
        assert_eq!(sprite_slot_for(0x0010), 1);
        assert!(sprite_slot_listed(0x3ff0));
        assert!(!sprite_slot_listed(0x4000));
        let key = pack_group_key(0x123456, 7, 0xabcd, 0x1357);
        assert_eq!(group_key_base(key), 0x123456);
        assert_eq!(group_key_class(key), 7);
        let slot = SpriteGroupSlot::default();
        let writer = SpriteGroupSlot {
            key: 1,
            ..SpriteGroupSlot::default()
        };
        assert_eq!(slot.after_write(0, &writer), writer);
        assert_eq!(slot.after_write(5, &SpriteGroupSlot::default()), slot);
        assert_eq!(
            writer.after_write(5, &SpriteGroupSlot::default()),
            SpriteGroupSlot::default()
        );
    }
}
