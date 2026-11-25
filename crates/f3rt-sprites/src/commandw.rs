// Command War draw span $9a3c..$9a6c: projection $a1c8, cull $a3ee, body compiler $9d32..$a192.

use std::collections::HashMap;

use crate::presentation::{RAM_BASE, RAM_MIRROR_MASK, SpriteGroupSlot};

pub const DESC_TABLE_ADDR: usize = 0x05a02c;
pub const SIZE_OFFSET_TABLE_ADDR: usize = 0x06a000;
pub const ANCHOR_OFFSET_TABLE_ADDR: usize = 0x0c9000;
pub const ANCHOR_SCALE_TABLE_ADDR: usize = 0x06b870;

pub const CAMERA_X_ADDR: u32 = 0x409036; // a5 - 0x6fca
pub const CAMERA_Y_ADDR: u32 = 0x409038; // a5 - 0x6fc8
pub const CAMERA_Z_ADDR: u32 = 0x40903a; // a5 - 0x6fc6
pub const CAMERA_COS_ADDR: u32 = 0x409040; // a5 - 0x6fc0
pub const CAMERA_SIN_ADDR: u32 = 0x409042; // a5 - 0x6fbe
pub const ZOOM_CLAMP_DISABLE_ADDR: u32 = 0x408255; // a5 - 0x7dab

pub const OBJ_RECORD_SIZE: usize = 0x80;
pub const OBJ_FLAGS0_OFF: usize = 0x00;
pub const OBJ_FLAGS1_OFF: usize = 0x01;
pub const OBJ_ZOOM_Y_OFF: usize = 0x04;
pub const OBJ_ZOOM_X_OFF: usize = 0x05;
pub const OBJ_SCREEN_X_OFF: usize = 0x06;
pub const OBJ_SCREEN_Y_OFF: usize = 0x08;
pub const OBJ_COLOR_OFF: usize = 0x0a;
pub const OBJ_WORLD_X_OFF: usize = 0x20;
pub const OBJ_WORLD_Y_OFF: usize = 0x22;
pub const OBJ_WORLD_Z_OFF: usize = 0x24;
pub const OBJ_CLASS_OFF: usize = 0x26;
pub const OBJ_FULL_DETAIL_DESC_OFF: usize = 0x2a;

pub const BODY_SINGLE_PC: u32 = 0x0000_9e9a;
pub const BODY_MULTI_PC: u32 = 0x0000_a0be;

pub const LOD_THRESHOLDS: [u8; 4] = [0x00, 0x80, 0xc0, 0xe0];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EmittedEntry {
    pub bytes: [u8; 16],
    pub group: SpriteGroupSlot,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ProjectedBody {
    pub screen_x: i16,
    pub screen_y: i16,
    pub zoom_x: u8,
    pub zoom_y: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FullDetailRecord {
    pub flags0: u8,
    pub color: u8,
    pub desc: u16,
    pub projected: ProjectedBody,
}

// The main loop can advance the object RAM at $410000 before VBlank.
#[derive(Clone, Debug, Default)]
pub struct FullDetailCache {
    records: HashMap<(u64, i32, i32), (u64, FullDetailRecord)>,
}

impl FullDetailCache {
    const EXPIRE_FRAMES: u64 = 4;

    pub fn record_write(&mut self, slot: SpriteGroupSlot, ram: &[u8], frame: u64) {
        if slot.key == 0 || (slot.pc != BODY_SINGLE_PC && slot.pc != BODY_MULTI_PC) {
            return;
        }
        let base = crate::presentation::group_key_base(slot.key);
        if let Some(rec) = capture_full_detail_record(ram, base) {
            self.records
                .insert((slot.key, slot.x, slot.y), (frame, rec));
        } else {
            self.records.remove(&(slot.key, slot.x, slot.y));
        }
    }

    pub fn lookup(&self, key: u64, group_x: i32, group_y: i32) -> Option<FullDetailRecord> {
        self.records.get(&(key, group_x, group_y)).map(|(_, r)| *r)
    }

    pub fn frame_end(&mut self, frame: u64) {
        self.records
            .retain(|_, (recorded, _)| recorded.saturating_add(Self::EXPIRE_FRAMES) >= frame);
    }

    pub fn reset(&mut self) {
        self.records.clear();
    }
}

fn read_be16(mem: &[u8], off: usize) -> Option<u16> {
    let bytes = mem.get(off..off + 2)?;
    Some(u16::from_be_bytes([bytes[0], bytes[1]]))
}

fn read_be32(mem: &[u8], off: usize) -> Option<u32> {
    let bytes = mem.get(off..off + 4)?;
    Some(u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}

fn ram_u8(ram: &[u8], addr: u32) -> Option<u8> {
    ram.get(((addr & 0x00ff_ffff) & RAM_MIRROR_MASK) as usize)
        .copied()
}

fn ram_i16(ram: &[u8], addr: u32) -> Option<i16> {
    let off = ((addr & 0x00ff_ffff) & RAM_MIRROR_MASK) as usize;
    read_be16(ram, off).map(|v| v as i16)
}

// DIVS.W leaves the destination register unchanged on quotient overflow.
fn m68k_divs_w(num: i32, den: i16) -> i16 {
    num.checked_div(i32::from(den))
        .and_then(|quotient| i16::try_from(quotient).ok())
        .unwrap_or(num as i16)
}

/// Recovers the unrescaled zoom byte from a rescaled zoom byte (`($4 - T[k]) << k`).
pub const fn unrescale_zoom_unchecked(rescaled: u8, lod_class: u8) -> u8 {
    if lod_class == 0 || lod_class > 3 {
        return rescaled;
    }
    (rescaled >> lod_class).wrapping_add(LOD_THRESHOLDS[lod_class as usize])
}

struct ObjectRecord<'a>(&'a [u8]);

impl<'a> ObjectRecord<'a> {
    fn at(ram: &'a [u8], base: u32) -> Option<Self> {
        let offset = (base & RAM_MIRROR_MASK) as usize;
        Some(Self(ram.get(offset..offset + OBJ_RECORD_SIZE)?))
    }

    fn word(&self, offset: usize) -> i16 {
        i16::from_be_bytes([self.0[offset], self.0[offset + 1]])
    }

    fn lod(&self) -> u8 {
        (self.0[OBJ_FLAGS1_OFF] & 0x0c) >> 2
    }

    fn descriptor(&self) -> Option<u16> {
        let desc = self.word(OBJ_FULL_DETAIL_DESC_OFF) as u16;
        (self.lod() != 0 && desc != 0).then_some(desc)
    }

    fn project(&self, camera_ram: &[u8]) -> Option<ProjectedBody> {
        let flags = self.0[OBJ_FLAGS0_OFF];
        if flags & 0x80 == 0 {
            return None;
        }
        let body = if flags & 0x40 != 0 {
            self.project_world(camera_ram)?
        } else {
            let lod = self.lod();
            ProjectedBody {
                screen_x: self.word(OBJ_SCREEN_X_OFF),
                screen_y: self.word(OBJ_SCREEN_Y_OFF),
                zoom_x: unrescale_zoom_unchecked(self.0[OBJ_ZOOM_X_OFF], lod),
                zoom_y: unrescale_zoom_unchecked(self.0[OBJ_ZOOM_Y_OFF], lod),
            }
        };
        ((-128..=0x180).contains(&body.screen_y) && (0..=0x200).contains(&body.screen_x))
            .then_some(body)
    }

    fn project_world(&self, ram: &[u8]) -> Option<ProjectedBody> {
        let x = ram_i16(ram, CAMERA_X_ADDR)?.wrapping_sub(self.word(OBJ_WORLD_X_OFF));
        let y = self
            .word(OBJ_WORLD_Y_OFF)
            .wrapping_add(ram_i16(ram, CAMERA_Y_ADDR)?);
        let z = self
            .word(OBJ_WORLD_Z_OFF)
            .wrapping_add(ram_i16(ram, CAMERA_Z_ADDR)?);
        let sin = i32::from(ram_i16(ram, CAMERA_SIN_ADDR)?);
        let cos = i32::from(ram_i16(ram, CAMERA_COS_ADDR)?);
        let vertical = (i32::from(z) * cos).wrapping_add(i32::from(y) * sin);
        let depth = (i32::from(z) * sin).wrapping_sub(i32::from(y) * cos);
        if ((depth >> 7) as i16) < 0x100 {
            return None;
        }
        let divisor = (depth >> 5) as i16;
        if divisor == 0 {
            return None;
        }
        let perspective = |numerator| m68k_divs_w(numerator, divisor).wrapping_neg();
        let mut zoom = perspective(0x40000) as u8;
        if zoom >= 0xe0
            && self.word(OBJ_CLASS_OFF) > 1
            && ram_u8(ram, ZOOM_CLAMP_DISABLE_ADDR)? == 0
        {
            zoom = 0xe0;
        }
        Some(ProjectedBody {
            screen_x: perspective(i32::from(x) << 10).wrapping_add(0x100),
            screen_y: perspective((vertical >> 4) << 7).wrapping_add(0xf1),
            zoom_x: zoom,
            zoom_y: zoom,
        })
    }

    fn capture(self, ram: &[u8]) -> Option<FullDetailRecord> {
        Some(FullDetailRecord {
            desc: self.descriptor()?,
            projected: self.project(ram)?,
            flags0: self.0[OBJ_FLAGS0_OFF],
            color: self.0[OBJ_COLOR_OFF],
        })
    }
}

pub fn project_body(ram: &[u8], base: u32) -> Option<ProjectedBody> {
    ObjectRecord::at(ram, base)?.project(ram)
}

pub fn full_detail_descriptor(ram: &[u8], base: u32) -> Option<u16> {
    if (base & 0x00ff_ffff) < RAM_BASE {
        return None;
    }
    ObjectRecord::at(ram, base)?.descriptor()
}

pub fn capture_full_detail_record(ram: &[u8], base: u32) -> Option<FullDetailRecord> {
    if (base & 0x00ff_ffff) < RAM_BASE {
        return None;
    }
    ObjectRecord::at(ram, base)?.capture(ram)
}

fn anchor_offset(rom: &[u8], desc: u16, axis_off: usize, zoom: u8) -> Option<i16> {
    let entry_addr = ANCHOR_OFFSET_TABLE_ADDR + usize::from(desc) * 4 + axis_off;
    let mut d0 = read_be16(rom, entry_addr)? as i16;
    if zoom == 0 {
        return Some(d0);
    }
    let neg = d0 < 0;
    if neg {
        d0 = d0.wrapping_neg();
    }
    let idx = (d0 as u16 & 0x00fc) as usize;
    let tbl_ptr = read_be32(rom, ANCHOR_SCALE_TABLE_ADDR + idx)? as usize;
    let mag = i16::from(*rom.get(tbl_ptr + usize::from(zoom))?);
    Some(if neg { mag.wrapping_neg() } else { mag })
}

#[derive(Clone, Copy)]
struct DescriptorRom<'a>(&'a [u8]);

impl<'a> DescriptorRom<'a> {
    fn grid(self, desc: u16) -> Option<Grid<'a>> {
        let offset = read_be32(self.0, DESC_TABLE_ADDR + usize::from(desc) * 4)? as usize;
        Some(Grid {
            rom: self.0,
            desc,
            offset,
        })
    }
}

struct Grid<'a> {
    rom: &'a [u8],
    desc: u16,
    offset: usize,
}

impl Grid<'_> {
    fn emit(
        self,
        record: FullDetailRecord,
        group: SpriteGroupSlot,
        out: &mut Vec<EmittedEntry>,
    ) -> Option<()> {
        let rows = read_be16(self.rom, self.offset)?;
        let columns = read_be16(self.rom, self.offset + 2)?;
        let flip = record.flags0 & 3;
        let anchor = record.flags0 & 8 != 0;
        let body = record.projected;
        let x = self.axis_origin(
            columns,
            0,
            body.screen_x,
            body.zoom_x,
            flip & 1 != 0,
            anchor,
        )?;
        let y = self.axis_origin(rows, 2, body.screen_y, body.zoom_y, flip & 2 != 0, anchor)?;
        let zoom = u16::from_be_bytes([body.zoom_y, body.zoom_x]);
        let mut tiles = TileCursor::new(self.rom, self.offset + 4, rows, flip)?;
        let entry_count = (usize::from(columns) + 1) * (usize::from(rows) + 1);
        out.reserve(entry_count);
        let flip_bits = u16::from(flip) << 8;
        for column in 0..=columns {
            for row in 0..=rows {
                let tile = tiles.next()?;
                let head = column == 0 && row == 0;
                let last = column == columns && row == rows;
                let chain = if last { 0 } else { 0x0800 };
                let (words, tag) = if head {
                    (
                        [
                            tile,
                            zoom,
                            x,
                            y,
                            flip_bits | chain | u16::from(record.color),
                        ],
                        group,
                    )
                } else {
                    let step = if row == 0 { 0xc400 } else { 0x7400 };
                    (
                        [tile, 0, 0, 0, flip_bits | chain | step],
                        SpriteGroupSlot::default(),
                    )
                };
                let mut bytes = [0; 16];
                for (dst, word) in bytes.as_chunks_mut::<2>().0.iter_mut().zip(words) {
                    *dst = word.to_be_bytes();
                }
                out.push(EmittedEntry { bytes, group: tag });
            }
            tiles.next_column();
        }
        Some(())
    }

    fn axis_origin(
        &self,
        dimension: u16,
        anchor_axis: usize,
        position: i16,
        zoom: u8,
        flipped: bool,
        anchored: bool,
    ) -> Option<u16> {
        let table = read_be32(
            self.rom,
            SIZE_OFFSET_TABLE_ADDR + usize::from(dimension) * 4,
        )? as usize;
        let size_offset = read_be16(self.rom, table + usize::from(zoom) * 2)? as i16;
        let anchor = if anchored {
            anchor_offset(self.rom, self.desc, anchor_axis, zoom)?
        } else {
            0
        };
        let anchor = if flipped {
            anchor
        } else {
            anchor.wrapping_neg()
        };
        Some(position.wrapping_sub(size_offset).wrapping_add(anchor) as u16 & 0x0fff)
    }
}

struct TileCursor<'a> {
    rom: &'a [u8],
    next: usize,
    column: usize,
    row_stride: usize,
    flip: u8,
}

impl<'a> TileCursor<'a> {
    fn new(rom: &'a [u8], header: usize, rows: u16, flip: u8) -> Option<Self> {
        // $a30e: the Y-flipped cursors start one word past the first tile and predecrement.
        let start = if flip == 0 {
            header + 6
        } else {
            let delta = read_be16(rom, header + usize::from(flip - 1) * 2)? as i16;
            header.wrapping_add_signed(isize::from(delta))
        };
        let row_stride = usize::from(rows.wrapping_add(1).wrapping_mul(2));
        Some(Self {
            rom,
            next: start,
            column: start,
            row_stride,
            flip,
        })
    }

    fn next(&mut self) -> Option<u16> {
        if self.flip & 2 != 0 {
            self.next = self.next.checked_sub(2)?;
            read_be16(self.rom, self.next)
        } else {
            let tile = read_be16(self.rom, self.next)?;
            self.next += 2;
            Some(tile)
        }
    }

    fn next_column(&mut self) {
        // $a31e: only one-axis flips need an explicit cursor reset at the column boundary.
        self.column = match self.flip {
            1 => self.column.wrapping_sub(self.row_stride),
            2 => self.column.wrapping_add(self.row_stride),
            _ => return,
        };
        self.next = self.column;
    }
}

pub fn emit_full_detail_record(
    rom: &[u8],
    record: FullDetailRecord,
    group_key: u64,
    out: &mut Vec<EmittedEntry>,
) -> Option<()> {
    let descriptors = DescriptorRom(rom);
    let descriptor = descriptors.grid(record.desc)?;
    let parts = *rom.get(descriptor.offset)?;
    let mut group = SpriteGroupSlot {
        key: group_key,
        x: i32::from(record.projected.screen_x) << 8,
        y: i32::from(record.projected.screen_y) << 8,
        pc: BODY_SINGLE_PC,
    };
    if parts == 0 || parts == 0xff {
        return descriptor.emit(record, group, out);
    }
    group.pc = BODY_MULTI_PC;
    for index in 0..usize::from(parts) {
        let offset = descriptor.offset + 2 + index * 4;
        let desc = read_be16(rom, offset)?;
        let color_add = *rom.get(offset + 2)?;
        let flip_xor = *rom.get(offset + 3)? & 3;
        let part = FullDetailRecord {
            desc,
            color: record.color.wrapping_add(color_add),
            flags0: record.flags0 ^ flip_xor,
            ..record
        };
        descriptors.grid(desc)?.emit(part, group, out)?;
    }
    Some(())
}

/// Command War's `0x9984..0x9a00` global+subglobal scroll bias applied to every body sprite entry.
pub const fn scroll_bias(flipped: bool) -> (i16, i16) {
    if flipped { (50, -16) } else { (-50, 17) }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::presentation::pack_group_key;
    use proptest::prelude::*;

    fn put_be16(buf: &mut [u8], off: usize, val: u16) {
        buf[off..off + 2].copy_from_slice(&val.to_be_bytes());
    }

    #[test]
    fn full_detail_cache_captures_clears_and_expires() {
        let mut cache = FullDetailCache::default();
        let mut ram = vec![0u8; 0x20000];
        let base = 0x410080u32;
        let off = (base & RAM_MIRROR_MASK) as usize;
        ram[off + OBJ_FLAGS0_OFF] = 0x80;
        ram[off + OBJ_FLAGS1_OFF] = 0x08;
        ram[off + OBJ_ZOOM_Y_OFF] = 0x40;
        ram[off + OBJ_ZOOM_X_OFF] = 0x40;
        put_be16(&mut ram, off + OBJ_SCREEN_X_OFF, 0x100);
        put_be16(&mut ram, off + OBJ_SCREEN_Y_OFF, 0x080);
        ram[off + OBJ_COLOR_OFF] = 0xb6;
        put_be16(&mut ram, off + OBJ_FULL_DETAIL_DESC_OFF, 0x0123);

        let slot = SpriteGroupSlot {
            key: pack_group_key(base, 1, 0, 0),
            x: 0x100 << 8,
            y: 0x080 << 8,
            pc: BODY_SINGLE_PC,
        };
        cache.record_write(slot, &ram, 10);
        let rec = cache.lookup(slot.key, slot.x, slot.y).unwrap();
        assert_eq!(rec.desc, 0x0123);
        assert_eq!(rec.projected.zoom_x, unrescale_zoom_unchecked(0x40, 2));

        ram[off + OBJ_FLAGS1_OFF] = 0x00;
        cache.record_write(slot, &ram, 11);
        assert!(cache.lookup(slot.key, slot.x, slot.y).is_none());

        ram[off + OBJ_FLAGS1_OFF] = 0x08;
        cache.record_write(slot, &ram, 12);
        cache.frame_end(16);
        assert!(cache.lookup(slot.key, slot.x, slot.y).is_some());
        cache.frame_end(17);
        assert!(cache.lookup(slot.key, slot.x, slot.y).is_none());
    }

    #[test]
    fn divs_word_preserves_destination_when_signed_long_quotient_overflows() {
        assert_eq!(m68k_divs_w(i32::MIN, -1), 0);
    }

    proptest! {
        #![proptest_config(ProptestConfig::with_cases(512))]

        #[test]
        fn divs_word_matches_signed_quotient_or_preserves_destination(
            numerator in prop_oneof![any::<i32>(), Just(i32::MIN), -100_000i32..=100_000],
            divisor in prop_oneof![any::<i16>(), -4i16..=4],
        ) {
            let expected = i64::from(numerator)
                .checked_div(i64::from(divisor))
                .and_then(|quotient| i16::try_from(quotient).ok())
                .unwrap_or(numerator as i16);
            prop_assert_eq!(m68k_divs_w(numerator, divisor), expected);
        }

        #[test]
        fn unrescale_inverts_forward_rescale_when_above_threshold(
            lod_class in 1u8..=3,
            zoom in 0u8..=255,
        ) {
            let threshold = LOD_THRESHOLDS[usize::from(lod_class)];
            let limit = threshold.saturating_add(0xff >> lod_class);
            if zoom >= threshold && zoom <= limit {
                let rescaled = zoom.wrapping_sub(threshold).wrapping_shl(u32::from(lod_class));
                prop_assert_eq!(unrescale_zoom_unchecked(rescaled, lod_class), zoom);
            }
        }

    }
}
