pub const SPRITE_ENTRIES: u16 = 0x400;
pub const SPRITE_SLOTS: usize = 0x800;

pub const RAM_BASE: u32 = 0x400000;
pub const RAM_END: u32 = 0x440000;
pub const RAM_MIRROR_MASK: u32 = 0x1ffff;
pub const GRAPHICS_BASE: u32 = 0x600000;
pub const SPRITE_RAM_SIZE: u32 = 0x10000;

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
    pub const fn after_write(&self, entry_byte: u32, writer: &Self) -> Self {
        if writer.key != 0 {
            return *writer;
        }
        if entry_byte >= 4 && entry_byte < 8 {
            return Self {
                key: 0,
                x: 0,
                y: 0,
                pc: 0,
            };
        }
        *self
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SpriteRamWrite {
    pub start: u32,
    pub width: u32,
}

impl SpriteRamWrite {
    pub(crate) fn at_address(address: u32, width: u32) -> Option<Self> {
        let start = (address & 0x00ff_ffff).wrapping_sub(GRAPHICS_BASE);
        (start < SPRITE_RAM_SIZE).then_some(Self { start, width })
    }

    pub(crate) fn retag(
        self,
        slots: &mut [SpriteGroupSlot; SPRITE_SLOTS],
        writer: &SpriteGroupSlot,
    ) {
        let end = self.start.saturating_add(self.width).min(SPRITE_RAM_SIZE);
        let mut start = self.start;
        while start < end {
            let entry_start = start & !15;
            if sprite_slot_listed(start) {
                let first = start - entry_start;
                let last = (end - entry_start).min(16);
                let tag_byte = if writer.key == 0 { first.max(4) } else { first };
                if tag_byte < last {
                    let slot = &mut slots[usize::from(sprite_slot_for(start))];
                    *slot = slot.after_write(tag_byte, writer);
                }
            }
            start = entry_start + 16;
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct MachineRam<'a> {
    ram: &'a [u8],
}

impl<'a> MachineRam<'a> {
    pub const fn new(ram: &'a [u8]) -> Self {
        Self { ram }
    }

    pub const fn bytes(&self) -> &'a [u8] {
        self.ram
    }

    pub(crate) fn mapped_address(address: u32) -> Option<u32> {
        let address = address & 0x00ff_ffff;
        (RAM_BASE..RAM_END).contains(&address).then_some(address)
    }

    pub fn offset16(&self, address: u32) -> Option<usize> {
        let offset = (Self::mapped_address(address)? & RAM_MIRROR_MASK) as usize;
        if offset + 2 > self.ram.len() {
            return None;
        }
        Some(offset)
    }

    pub fn read_ram16(&self, address: u32) -> Option<i16> {
        let offset = self.offset16(address)?;
        Some(i16::from_be_bytes([self.ram[offset], self.ram[offset + 1]]))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn listed_entries_are_first_half_of_each_bank() {
        assert!(sprite_slot_listed(0x0000));
        assert!(sprite_slot_listed(0x3ff0));
        assert!(!sprite_slot_listed(0x4000));
        assert!(!sprite_slot_listed(0x7fff));
        assert!(sprite_slot_listed(0x8000));
        assert!(!sprite_slot_listed(0xc000));
        assert!(sprite_slot_listed(0x10001));
        assert_eq!(sprite_slot_for(0x8010), 0x401);
    }

    #[test]
    fn group_key_round_trips() {
        let key = pack_group_key(0x410080, 2, 0x1234, 0x5678);
        assert_eq!(group_key_base(key), 0x410080);
        assert_eq!(group_key_class(key), 2);
        assert_eq!((key >> 16) as u16, 0x1234);
        assert_eq!(key as u16, 0x5678);
    }

    #[test]
    fn after_write_retags_clears_or_keeps() {
        let old_tag = SpriteGroupSlot {
            key: 1,
            x: 2,
            y: 3,
            pc: 4,
        };
        let writer = SpriteGroupSlot {
            key: 5,
            x: 6,
            y: 7,
            pc: 8,
        };
        let empty = SpriteGroupSlot::default();
        for byte in 0..16 {
            assert_eq!(old_tag.after_write(byte, &writer), writer);
            let expected = if (4..8).contains(&byte) {
                empty
            } else {
                old_tag
            };
            assert_eq!(old_tag.after_write(byte, &empty), expected);
        }
    }

    #[test]
    fn machine_ram_reads_mirrored_be16() {
        let mut buf = vec![0u8; 0x20000];
        buf[0x100] = 0xff;
        buf[0x101] = 0xfd;
        let ram = MachineRam::new(&buf);
        assert_eq!(ram.read_ram16(RAM_BASE + 0x100), Some(-3));
        assert_eq!(ram.read_ram16(RAM_BASE + 0x20100), Some(-3));
        assert_eq!(ram.read_ram16(0x000000), None);
        assert_eq!(ram.read_ram16(RAM_END), None);
        assert_eq!(ram.read_ram16(RAM_BASE + 0x1ffff), None);
    }

    mod properties {
        use super::*;
        use proptest::prelude::*;

        proptest! {
            #[test]
            fn group_key_pack_unpack_invariants(
                base in any::<u32>(),
                cls in any::<u32>(),
                gen_x in any::<u16>(),
                gen_y in any::<u16>(),
            ) {
                let key = pack_group_key(base, cls, gen_x, gen_y);
                prop_assert_eq!(group_key_base(key), base & 0x00ff_ffff);
                prop_assert_eq!(group_key_class(key), cls & 0xff);
                prop_assert_eq!((key >> 16) as u16, gen_x);
                prop_assert_eq!(key as u16, gen_y);
            }

            #[test]
            fn listed_slots_always_index_within_sprite_slots(offset in any::<u32>()) {
                let listed = sprite_slot_listed(offset);
                let slot = sprite_slot_for(offset);
                if listed {
                    prop_assert!(usize::from(slot) < SPRITE_SLOTS);
                    let expected_bank = ((offset >> 15) & 1) as u16;
                    prop_assert_eq!(slot / SPRITE_ENTRIES, expected_bank);
                }
            }

            #[test]
            fn after_write_matches_contract(
                old_key in any::<u64>(),
                old_x in any::<i32>(),
                old_y in any::<i32>(),
                old_pc in any::<u32>(),
                w_key in any::<u64>(),
                w_x in any::<i32>(),
                w_y in any::<i32>(),
                w_pc in any::<u32>(),
                byte in 0u32..16,
            ) {
                let old = SpriteGroupSlot { key: old_key, x: old_x, y: old_y, pc: old_pc };
                let writer = SpriteGroupSlot { key: w_key, x: w_x, y: w_y, pc: w_pc };
                let next = old.after_write(byte, &writer);
                if writer.key != 0 {
                    prop_assert_eq!(next, writer);
                } else if (4..8).contains(&byte) {
                    prop_assert_eq!(next, SpriteGroupSlot::default());
                } else {
                    prop_assert_eq!(next, old);
                }
            }
        }
    }
}
