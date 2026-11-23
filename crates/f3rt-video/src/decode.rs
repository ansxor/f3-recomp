//! Generic TC0630FDP video-RAM decode primitives, ported from
//! `runtime/renderer/decode.{hpp,cpp}` and `runtime/sprites/presentation.hpp`.

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

// --- presentation.hpp ---

/// Entries per sprite bank.
pub const SPRITE_ENTRIES: u16 = 0x400;
/// Slots across both banks.
pub const SPRITE_SLOTS: usize = 0x800;

pub const fn sprite_slot_for(offset: u32) -> u16 {
    (((offset >> 15) & 1) * SPRITE_ENTRIES as u32 + ((offset & 0x7fff) >> 4)) as u16
}

pub const fn sprite_slot_listed(offset: u32) -> bool {
    ((offset & 0x7fff) >> 4) < SPRITE_ENTRIES as u32
}

pub const fn pack_group_key(base: u32, group_class: u32, gen_x: u16, gen_y: u16) -> u64 {
    ((base & 0x00ff_ffff) as u64) << 40
        | ((group_class & 0xff) as u64) << 32
        | (gen_x as u64) << 16
        | gen_y as u64
}

pub const fn group_key_base(key: u64) -> u32 {
    ((key >> 40) & 0x00ff_ffff) as u32
}

pub const fn group_key_class(key: u64) -> u32 {
    ((key >> 32) & 0xff) as u32
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SpriteGroupSlot {
    pub key: u64,
    pub x: i32,
    pub y: i32,
    pub pc: u32,
}

impl SpriteGroupSlot {
    pub fn after_write(&self, entry_byte: u32, writer: &Self) -> Self {
        if writer.key != 0 {
            return *writer;
        }
        if (4..8).contains(&entry_byte) {
            return Self::default();
        }
        *self
    }
}

#[derive(Clone, Copy, Debug)]
pub struct SpriteSplice<'a> {
    pub bank: bool, // false: 0x600000 half, true: 0x608000 half
    pub first: u16, // inclusive entry index within the bank (0..0x3ff)
    pub last: u16,
    pub real: &'a [u8],
    pub replacement: &'a [u8], // n * 16 raw entries; jump words inside are ignored
    pub groups: &'a [SpriteGroupSlot],
}

#[derive(Clone, Copy, Debug, Default)]
pub struct SpritePresentation<'a> {
    pub groups: &'a [SpriteGroupSlot],
    pub splices: &'a [SpriteSplice<'a>],
}

impl SpritePresentation<'_> {
    pub fn empty(&self) -> bool {
        self.groups.is_empty() && self.splices.is_empty()
    }
}

/// Uses `SPRITE_BANK_STRIDE` from `include/f3rt/memory_map.hpp`.
const SPRITE_BANK_STRIDE: u32 = 0x8000;
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

/// Walk the sprite display list in `spriteram` (0x10000 bytes, base 0x600000) following block
/// chaining, the jump word and command entries. Returns the number of entries written to `out`.
struct DecodeState<'a> {
    x: SpriteAxis,
    y: SpriteAxis,
    color: u8,
    multi: bool,
    count: usize,
    block_group: u64,
    block_pc: u32,
    block_x: i32,
    block_y: i32,
    out: &'a mut [DecodedSpriteEntry],
    ram: &'a mut SpriteRamState,
    visible_y: i32,
    visible_height: i32,
}

/// One list entry through the hardware state machine. Returns false when the entry is a
/// self-jump (list end). `allow_jump` is false for splice replacements.
#[allow(clippy::too_many_arguments)]
fn step(
    ctx: &mut DecodeState<'_>,
    spr: &[u8],
    offs: &mut i32,
    allow_jump: bool,
    apply_flip: bool,
    mut group: u64,
    mut group_x: i32,
    mut group_y: i32,
    mut group_pc: u32,
) -> bool {
    let w0 = read_be16(spr, 0);
    let w1 = read_be16(spr, 2);
    let w2 = read_be16(spr, 4);
    let w3 = read_be16(spr, 6);
    let w4 = read_be16(spr, 8);
    let w5 = read_be16(spr, 10);
    let w6 = read_be16(spr, 12);

    // Special command bit in word 3
    if w3 & 0x8000 != 0 {
        ctx.ram.flipscreen = (w5 & 0x2000) != 0;
        ctx.ram.extra_planes = ((w5 >> 8) & 3) as u8;
        ctx.ram.pen_mask = (ctx.ram.extra_planes << 4) | 0x0f;
        ctx.ram.trails = (w5 & 0x0002) != 0;
        ctx.ram.bank = (w5 & 0x0001) != 0;
    }

    // Sprite list jump bit in word 6
    if allow_jump && (w6 & 0x8000) != 0 {
        let new_offs = i32::from(w6 & 0x03ff);
        if new_offs == *offs {
            return false;
        }
        *offs = new_offs - 1;
    }

    let spritecont = (w4 >> 8) as u8;
    let lock = (spritecont & 0x04) != 0;
    if !lock {
        ctx.color = (w4 & 0xff) as u8;
    }

    let own_group = group != 0;
    if spritecont & 0x08 == 0 {
        ctx.block_group = group;
        ctx.block_pc = group_pc;
        ctx.block_x = group_x;
        ctx.block_y = group_y;
    } else if !own_group {
        group = ctx.block_group;
        group_pc = ctx.block_pc;
        group_x = ctx.block_x;
        group_y = ctx.block_y;
    } else {
        ctx.block_group = group;
        ctx.block_pc = group_pc;
        ctx.block_x = group_x;
        ctx.block_y = group_y;
    }

    let scroll_mode = ((w2 >> 12) & 0x0f) as u8;
    ctx.x.update(
        scroll_mode,
        w2 & 0x0fff,
        ctx.multi,
        (spritecont >> 6) & 3,
        (w1 & 0xff) as u8,
    );
    ctx.y.update(
        scroll_mode,
        w3 & 0x0fff,
        ctx.multi,
        (spritecont >> 4) & 3,
        (w1 >> 8) as u8,
    );
    ctx.multi = (spritecont & 0x08) != 0;

    let tile = u32::from(w0) | (u32::from(w5 & 0x0001) << 16);
    if tile == 0 {
        return true;
    }

    let tx = if ctx.ram.flipscreen {
        (512 << 8) - ctx.x.block_scale * 16 - ctx.x.pos
    } else {
        ctx.x.pos
    };
    let ty = if ctx.ram.flipscreen {
        (256 << 8) - ctx.y.block_scale * 16 - ctx.y.pos
    } else {
        ctx.y.pos
    };

    // Cull against the configured scanout crop, not the top edge.
    if tx + ctx.x.block_scale * 16 <= (H_START << 8)
        || tx > ((H_START + H_VIS - 1) << 8)
        || ty + ctx.y.block_scale * 16 <= (ctx.visible_y << 8)
        || ty > ((ctx.visible_y + ctx.visible_height - 1) << 8)
    {
        return true;
    }

    if ctx.count >= ctx.out.len() {
        return true;
    }

    let flip_x = (spritecont & 0x01) != 0;
    let flip_y = (spritecont & 0x02) != 0;

    let color = ctx.color;
    let s = &mut ctx.out[ctx.count];
    ctx.count += 1;
    s.x = if apply_flip { tx } else { ctx.x.pos };
    s.y = if apply_flip { ty } else { ctx.y.pos };
    s.scale_x = ctx.x.block_scale;
    s.scale_y = ctx.y.block_scale;
    s.tile = tile;
    s.color = color;
    s.flip_x = if apply_flip && ctx.ram.flipscreen {
        !flip_x
    } else {
        flip_x
    };
    s.flip_y = if apply_flip && ctx.ram.flipscreen {
        !flip_y
    } else {
        flip_y
    };
    s.pri = (color >> 6) & 3;
    s.group = group;
    s.group_x = group_x;
    s.group_y = group_y;
    s.group_pc = group_pc;
    true
}

/// Walk the sprite display list in `spriteram` (0x10000 bytes, base 0x600000) following block
/// chaining, the jump word and command entries. Returns the number of entries written to `out`.
#[allow(clippy::too_many_arguments)]
pub fn decode_sprite_list(
    spriteram: &[u8],
    visible_y: i32,
    visible_height: i32,
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
        block_group: 0,
        block_pc: 0,
        block_x: 0,
        block_y: 0,
        out,
        ram: state,
        visible_y,
        visible_height,
    };
    let empty_groups: &[SpriteGroupSlot] = &[];
    let empty_splices: &[SpriteSplice<'_>] = &[];
    let splices = presentation.map_or(empty_splices, |p| p.splices);
    let groups = presentation.map_or(empty_groups, |p| p.groups);

    let mut total_sprites = 0;
    let mut offs: i32 = 0;
    while offs < 0x400 && total_sprites < 0x400 {
        total_sprites += 1;
        let bank_offset = if ctx.ram.bank { SPRITE_BANK_STRIDE } else { 0 };
        let entry_start = bank_offset as usize + offs as usize * 16;
        let spr = &spriteram[entry_start..entry_start + 16];

        let mut hit: Option<&SpriteSplice<'_>> = None;
        for sp in splices {
            if sp.bank != ctx.ram.bank || i32::from(sp.first) != offs {
                continue;
            }
            let n = i32::from(sp.last) - i32::from(sp.first) + 1;
            if sp.last < sp.first
                || sp.last >= 0x400
                || sp.real.len() != n as usize * 16
                || &spriteram[entry_start..entry_start + n as usize * 16] != sp.real
            {
                break;
            }
            hit = Some(sp);
            break;
        }
        if let Some(hit) = hit {
            let n = i32::from(hit.last) - i32::from(hit.first) + 1;
            let reps = hit.replacement.len() / 16;
            for k in 0..reps {
                let mut dummy = offs;
                let group = hit.groups.get(k).copied().unwrap_or_default();
                let entry = &hit.replacement[k * 16..k * 16 + 16];
                step(
                    &mut ctx, entry, &mut dummy, false, apply_flip, group.key, group.x, group.y,
                    group.pc,
                );
            }
            total_sprites += (n - 1) as usize;
            offs = i32::from(hit.last) + 1;
            continue;
        }

        {
            let slot = (if ctx.ram.bank { 0x400 } else { 0 }) + offs as usize;
            let group = groups.get(slot).copied().unwrap_or_default();
            if !step(
                &mut ctx, spr, &mut offs, true, apply_flip, group.key, group.x, group.y, group.pc,
            ) {
                break;
            }
        }
        offs += 1;
    }
    ctx.count
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write_entry(ram: &mut [u8], bank: bool, index: usize, entry: [u16; 7]) {
        let base = if bank { SPRITE_BANK_STRIDE as usize } else { 0 } + index * 16;
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
        let count = decode_sprite_list(&ram, 24, 232, &mut out, &mut state, false, None);
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
            decode_sprite_list(&ram, 24, 232, &mut out, &mut state, false, None),
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
            decode_sprite_list(&ram, 24, 232, &mut out, &mut state, false, None),
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
            decode_sprite_list(&ram, 24, 232, &mut out, &mut state, false, None),
            0
        );
    }

    #[test]
    fn splice_replaces_entries() {
        let mut ram = vec![0u8; 0x10000];
        write_entry(&mut ram, false, 0, [0x1111, 0, 0, 0, 0, 0, 0x8000]);
        let real = ram[0..32].to_vec();
        let mut replacement = vec![0u8; 32];
        let base = 0;
        let entry: [u16; 7] = [1, 0, 100, 200, 0, 0, 0];
        for (i, word) in entry.iter().enumerate() {
            replacement[base + i * 2..base + i * 2 + 2].copy_from_slice(&word.to_be_bytes());
        }
        let splice = SpriteSplice {
            bank: false,
            first: 0,
            last: 1,
            real: &real,
            replacement: &replacement,
            groups: &[],
        };
        let presentation = SpritePresentation {
            groups: &[],
            splices: &[splice],
        };
        let mut out = [DecodedSpriteEntry::default(); 4];
        let mut state = SpriteRamState::default();
        let count = decode_sprite_list(
            &ram,
            24,
            232,
            &mut out,
            &mut state,
            false,
            Some(&presentation),
        );
        // The second replacement entry is all zeros (tile 0), so only the first decodes.
        assert_eq!(count, 1);
        assert_eq!(out[0].tile, 1);
    }

    #[test]
    fn flipscreen_mirrors_when_apply_flip() {
        let mut ram = vec![0u8; 0x10000];
        write_entry(&mut ram, false, 0, [0, 0, 0, 0x8000, 0, 0x2000, 0]);
        write_entry(&mut ram, false, 1, [1, 0, 300, 200, 0, 0, 0]);
        write_entry(&mut ram, false, 2, [0, 0, 0, 0, 0, 0, 0x8002]);
        let mut out = [DecodedSpriteEntry::default(); 4];
        let mut state = SpriteRamState::default();
        let count = decode_sprite_list(&ram, 24, 232, &mut out, &mut state, true, None);
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
