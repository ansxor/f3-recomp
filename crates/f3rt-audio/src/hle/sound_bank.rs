//! `audio/hle/sound_bank.hpp`: the decoded sound-program ROM (sample-bank directory, instrument
//! records and the fixed tables the voice reads).

use crate::bytes::{read_be16, s16};
use crate::hle::patch::{DirectRegion, Layer, Program};
use crate::hle::rom_view::RomView;
use crate::hle::sound_rom_5a7e9117::{
    ATTENUATION_TABLE, BEND_RANGE_PTR, BUDGET_TABLE, Bank, FILTER_TABLE, HEAP_BASE_PTR,
    HEAP_LIMIT_PTR_A, HEAP_LIMIT_PTR_B, HEAP_METADATA_OFFSET, HEAP_NEXT_PTR, HEAP_RESERVE_PTR,
    PAN_TABLE, PHASE_RATE_TABLE, PITCH_TABLE, PROGRAM_BANK_MASK_OFFSET, PROGRAM_COLUMN_STRIDE,
    PROGRAM_DATA_BASE, PROGRAM_DEFAULT_BANK_OFFSET, PROGRAM_MAGIC_OFFSET_0, PROGRAM_MAGIC_OFFSET_1,
    PROGRAM_RECORD_STRIDE, ProgramData, RATE_TABLE, RESET_END_PTR, RESET_START_PTR,
    SAMPLE_BANK_SIZE, SAMPLE_HALF_BANK_SIZE, SAMPLE_TAG_PROBE_OFFSET, SPLIT_DIRECTORY,
    SPLIT_TABLE_BASE, Split, VELOCITY_CURVE_TABLE, program_slot,
};
use crate::memory_map::{SOUND_ROM_BASE, SOUND_ROM_SIZE};

pub struct SoundBank {
    pub rom: RomView,
    /// Sample words borrowed from the sample ROM the Audio implementation owns.
    pub samples: &'static [u16],
    pub metadata: Vec<u8>,
    pub banks: [Bank; 16],
    pub programs: [ProgramData; 1024],
    pub rates: [u16; 128],
    pub phase_rates: [u16; 256],
    pub filters: [u16; 2048],
    pub pitches: [u16; 768],
    pub attenuation: [u8; 128],
    pub pan: [u8; 128],
    pub velocity_curves: [u8; 512],
    pub budgets: [u8; 12],
    pub bend_range: u8,
    pub board_bank_mask: u32,
    pub reset_start: u32,
    pub reset_end: u32,
}

impl SoundBank {
    pub fn new(sound_rom: &'static [u8], sample_words: &'static [u16]) -> SoundBank {
        let mut bank = SoundBank {
            rom: RomView::with_image(sound_rom, 1),
            samples: sample_words,
            metadata: Vec::new(),
            banks: [Bank::default(); 16],
            programs: [ProgramData::default(); 1024],
            rates: [0; 128],
            phase_rates: [0; 256],
            filters: [0; 2048],
            pitches: [0; 768],
            attenuation: [0; 128],
            pan: [0; 128],
            velocity_curves: [0; 512],
            budgets: [0; 12],
            bend_range: 0,
            board_bank_mask: 0,
            reset_start: 0,
            reset_end: 0,
        };
        if bank.rom.size() < SOUND_ROM_SIZE
            || bank.samples.len() < SOUND_ROM_SIZE
            || !bank.samples.len().is_power_of_two()
        {
            panic!("HLE requires mapped sound and power-of-two sample ROM regions");
        }
        bank.board_bank_mask = if bank.samples.len() >= SAMPLE_BANK_SIZE {
            (bank.samples.len() / SAMPLE_BANK_SIZE - 1) as u32
        } else {
            0
        };
        bank.metadata.reserve(0x10000);
        bank.decode_banks();

        let heap_base = u32::from(bank.rom.read_u16(HEAP_BASE_PTR));
        let heap_limit = bank
            .rom
            .read_u32(HEAP_LIMIT_PTR_A)
            .wrapping_add(bank.rom.read_u32(HEAP_LIMIT_PTR_B));
        let heap_size = heap_limit
            .wrapping_sub(heap_base)
            .wrapping_sub(bank.rom.read_u32(HEAP_RESERVE_PTR));
        let heap_next = bank.rom.read_u32(HEAP_NEXT_PTR);
        if heap_base >= HEAP_METADATA_OFFSET
            && heap_base - HEAP_METADATA_OFFSET + 8 <= bank.metadata.len() as u32
        {
            for i in 0..4u32 {
                bank.metadata[(heap_base - HEAP_METADATA_OFFSET + i) as usize] =
                    (heap_size >> (24 - i * 8)) as u8;
                bank.metadata[(heap_base - HEAP_METADATA_OFFSET + 4 + i) as usize] =
                    (heap_next >> (24 - i * 8)) as u8;
            }
        }
        bank.cache_programs();
        for i in 0..bank.rates.len() {
            bank.rates[i] = bank.rom.read_u16(RATE_TABLE + 2 * i as u32);
        }
        for i in 0..bank.phase_rates.len() {
            bank.phase_rates[i] = bank.rom.read_u16(PHASE_RATE_TABLE + 2 * i as u32);
        }
        for i in 0..bank.filters.len() {
            bank.filters[i] = bank.rom.read_u16(FILTER_TABLE + 2 * i as u32);
        }
        for i in 0..bank.pitches.len() {
            bank.pitches[i] = bank.rom.read_u16(PITCH_TABLE + 2 * i as u32);
        }
        bank.bend_range = bank.rom.read_u16(BEND_RANGE_PTR) as u8;
        bank.reset_start = (u32::from(bank.rom.read_u16(RESET_START_PTR)) << 16
            | u32::from(bank.rom.read_u16(RESET_START_PTR + 2)))
            & 0x1fffffe0;
        bank.reset_end = (u32::from(bank.rom.read_u16(RESET_END_PTR)) << 16
            | u32::from(bank.rom.read_u16(RESET_END_PTR + 2)))
            & 0x1fffffe0;
        for i in 0..128 {
            bank.attenuation[i] = bank.rom.read_byte(ATTENUATION_TABLE + i as u32);
            bank.pan[i] = bank.rom.read_byte(PAN_TABLE + i as u32);
        }
        for i in 0..512 {
            bank.velocity_curves[i] = bank.rom.read_byte(VELOCITY_CURVE_TABLE + i as u32);
        }
        for i in 0..12 {
            bank.budgets[i] = bank.rom.read_byte(BUDGET_TABLE + i as u32);
        }
        bank
    }

    pub fn set_bank(&self, bank: u8) {
        self.rom.set_bank(bank);
    }

    pub fn program_data(&self, slot: u32) -> &ProgramData {
        &self.programs[slot as usize]
    }

    pub fn split_for(
        &self,
        patch: Layer,
        layer: u32,
        pitch: i32,
        selector: &mut u8,
        program_id: u16,
    ) -> Split {
        if patch.sample().table() {
            *selector = (self.board_bank_mask * 2) as u8;
            return Split::default();
        }
        self.split_lookup(patch.sample_id(), layer, pitch, selector, program_id)
    }

    pub fn split_for_region(
        &self,
        patch: DirectRegion,
        layer: u32,
        pitch: i32,
        selector: &mut u8,
        program_id: u16,
    ) -> Split {
        self.split_lookup(patch.sample_id(), layer, pitch, selector, program_id)
    }

    pub fn key_weight(&self, patch: Layer, key: i32) -> i32 {
        let amount = i32::from(patch.weight_amount());
        let lo = i32::from(patch.weight_low());
        let hi = i32::from(patch.weight_high());
        if amount == -128 {
            return if key < lo || key > hi { -1 } else { 127 };
        }
        if amount < 0 {
            if key <= lo {
                return 127;
            }
            if key >= hi {
                return 127 + amount;
            }
            return if hi <= lo {
                127
            } else {
                127 + amount + (-amount) * (hi - key) / (hi - lo)
            };
        }
        if key >= hi {
            return 127;
        }
        if key <= lo {
            return 127 - amount;
        }
        if hi <= lo {
            127
        } else {
            127 - amount + amount * (key - lo) / (hi - lo)
        }
    }

    pub fn frequency(&self, pitch: i32) -> u16 {
        let mut pitch = i32::from(s16(pitch as u32));
        pitch = pitch.clamp(-0x4800, 0x5400);
        let mut shifts: u32 = 0;
        let result: u32 = if pitch < 0 {
            while pitch < 0 {
                pitch += 0xc00;
                shifts += 1;
            }
            u32::from(self.pitches[(pitch >> 2) as usize]) >> shifts
        } else {
            while pitch >= 0xc00 {
                pitch -= 0xc00;
                shifts += 1;
            }
            u32::from(self.pitches[(pitch >> 2) as usize]) << shifts
        };
        (result as u16) & 0xfffe
    }

    pub fn rate(&self, r: i32) -> u16 {
        self.rates[r.clamp(0, 99) as usize]
    }

    pub fn coefficient(&self, value: i32, b: u32) -> u16 {
        let index = value.clamp(0, 1023) as u32 + b;
        ((u32::from(self.filters[index as usize]) << 4) as u16) & 0xfff0
    }

    fn split_lookup(
        &self,
        id: u8,
        layer: u32,
        pitch: i32,
        selector: &mut u8,
        program_id: u16,
    ) -> Split {
        let bank_idx = i32::from(
            self.programs[program_slot(program_id) as usize].sample_banks[layer as usize],
        );
        if bank_idx == -2 {
            panic!("HLE instrument names missing sample bank");
        }
        let data: *const u8;
        let available: usize;
        if bank_idx >= 0 {
            let b = &self.banks[bank_idx as usize];
            *selector = bank_idx as u8;
            if id >= b.count {
                panic!("HLE sample ID exceeds external directory");
            }
            let relative = unsafe {
                read_be16(
                    self.metadata
                        .as_ptr()
                        .add(b.offset as usize + 2 + id as usize * 14),
                )
            };
            let offset = b.offset as usize + (relative as i16) as isize as usize;
            if offset + 14 > self.metadata.len() {
                panic!("HLE sample offset exceeds metadata");
            }
            data = unsafe { self.metadata.as_ptr().add(offset) };
            available = self.metadata.len() - offset;
        } else {
            *selector = (self.board_bank_mask * 2) as u8;
            if id > 120 {
                panic!("HLE ROM split ID exceeds table");
            }
            let address = SPLIT_TABLE_BASE.wrapping_add(
                (self.rom.read_u16(SPLIT_DIRECTORY + u32::from(id) * 2) as i16) as i32 as u32,
            );
            data = self.rom.pointer_bytes(address, 14);
            available = self.rom.size() - (data as usize - self.rom.data() as usize);
        }
        let key = ((pitch >> 8) as u8) as u32;
        let mut i: usize = 0;
        while i + 14 <= available {
            let result = unsafe { Split::read(data.add(i)) };
            if key <= u32::from(result.cutoff) {
                return result;
            }
            i += 14;
        }
        panic!("HLE sample splits do not cover key");
    }

    fn append_word(metadata: &mut Vec<u8>, w: u16) {
        metadata.push((w >> 8) as u8);
        metadata.push(w as u8);
    }

    // `b` is the bank index the decoded half-bank's base offset is computed from, so the loop stays an
    // index loop as in the C++.
    #[allow(clippy::needless_range_loop)]
    fn decode_banks(&mut self) {
        let SoundBank {
            banks,
            metadata,
            samples,
            ..
        } = self;
        let samples: &[u16] = samples;
        let num_banks = if 16 < samples.len() / SAMPLE_HALF_BANK_SIZE {
            16
        } else {
            samples.len() / SAMPLE_HALF_BANK_SIZE
        };
        for b in 0..num_banks {
            let mut at: usize = 0;
            let base = b * SAMPLE_HALF_BANK_SIZE;
            let sample_word = |at: &mut usize| -> u16 {
                if *at >= SAMPLE_HALF_BANK_SIZE || base + *at >= samples.len() {
                    panic!("HLE sample metadata exceeds half-bank");
                }
                let word = samples[base + *at];
                *at += 1;
                word
            };
            let flag = sample_word(&mut at);
            let packed = (flag & 0x8000) != 0;
            let data_word = |at: &mut usize| -> u16 {
                if !packed {
                    return sample_word(at);
                }
                let hi = sample_word(at);
                (hi & 0xff00) | (sample_word(at) >> 8)
            };
            let bank = &mut banks[b];
            bank.offset = metadata.len() as u32;
            SoundBank::append_word(metadata, flag);
            let mut last: u32 = 0;
            let mut valid = true;
            loop {
                let offset = data_word(&mut at);
                SoundBank::append_word(metadata, offset);
                if offset == 0 {
                    break;
                }
                let limit = if packed {
                    u32::from(offset) * 2 + 4
                } else {
                    u32::from(offset) + 8
                };
                if limit <= at as u32 * 2 || limit % 14 != 0 {
                    valid = false;
                    break;
                }
                last = if packed {
                    u32::from(offset) * 2
                } else {
                    u32::from(offset)
                };
                if packed {
                    let size = metadata.len();
                    metadata[size - 2] = ((u32::from(offset) + 1) >> 8) as u8;
                    metadata[size - 1] = (u32::from(offset) + 1) as u8;
                }
                bank.count = bank.count.wrapping_add(1);
                for _ in 0..6 {
                    let word = data_word(&mut at);
                    SoundBank::append_word(metadata, word);
                }
            }
            bank.probe = ((at as u32 * 2) << 8) | ((b as u32 & 1) << 28);
            if !valid || last == 0 {
                bank.count = 0;
                continue;
            }
            while at as u32 * 2 < last {
                let word = data_word(&mut at);
                SoundBank::append_word(metadata, word);
            }
            if at as u32 * 2 != last {
                panic!("HLE split metadata is not aligned");
            }
            loop {
                let begin = metadata.len();
                for _ in 0..7 {
                    let word = data_word(&mut at);
                    SoundBank::append_word(metadata, word);
                }
                if metadata[begin + 5] == 127 {
                    break;
                }
            }
            let tag = if base + (SAMPLE_TAG_PROBE_OFFSET as usize) < samples.len() {
                samples[base + SAMPLE_TAG_PROBE_OFFSET as usize]
            } else {
                0
            };
            bank.tag = if packed { (tag >> 8) as u8 } else { tag as u8 };
        }
    }

    fn cache_programs(&mut self) {
        const BASES: [u32; 4] = [PROGRAM_DATA_BASE, PROGRAM_DATA_BASE, 0xc7b050, 0xc760a0];
        let rom_size = self.rom.size();
        let banks = &self.banks;
        // As in the C++, `b` indexes `banks` and is also the bank number stored in the result.
        #[allow(clippy::needless_range_loop)]
        let find_bank = |tag: u8| -> i8 {
            let mut result: i8 = -2;
            for b in 0..banks.len() {
                if banks[b].count != 0 && banks[b].tag == tag {
                    result = b as i8;
                }
            }
            result
        };
        for bank_idx in 0..4u32 {
            let table_base = BASES[bank_idx as usize];
            let populated = (table_base >= SOUND_ROM_BASE)
                && (table_base - SOUND_ROM_BASE + PROGRAM_RECORD_STRIDE as u32 <= rom_size as u32)
                && (self.rom.get((table_base - SOUND_ROM_BASE + 2) as usize) != 0xff
                    || self.rom.get((table_base - SOUND_ROM_BASE + 3) as usize) != 0xff);
            for id in 0..256u32 {
                let slot = bank_idx * 256 + id;
                let data = &mut self.programs[slot as usize];
                data.sample_banks.fill(-1);
                if !populated {
                    data.base = core::ptr::null();
                    data.enabled = 0;
                    continue;
                }
                let mut index = id;
                let mut column = 0u32;
                let mut base = table_base;
                if index >= 80 {
                    base += PROGRAM_COLUMN_STRIDE;
                    index -= 80;
                    if index >= 10 {
                        index -= 10;
                        column += 1;
                    }
                }
                if index >= 40 {
                    index -= 40;
                    column += 1;
                }
                let addr = base + index * PROGRAM_RECORD_STRIDE as u32 + column;
                if addr - SOUND_ROM_BASE + PROGRAM_RECORD_STRIDE as u32 > rom_size as u32 {
                    data.base = core::ptr::null();
                    data.enabled = 0;
                    continue;
                }
                data.base = unsafe { self.rom.data().add((addr - SOUND_ROM_BASE) as usize) };
                data.enabled = Program::new(data.base).enabled();
                if data.enabled == 0 {
                    let p = data.base;
                    let mut mask: u32 = 0;
                    for i in 0..4u32 {
                        mask = (mask << 8)
                            | u32::from(unsafe {
                                *p.add(PROGRAM_BANK_MASK_OFFSET + 2 * i as usize)
                            });
                    }
                    if unsafe { *p.add(PROGRAM_MAGIC_OFFSET_0) } == b'X'
                        && unsafe { *p.add(PROGRAM_MAGIC_OFFSET_1) } == b'E'
                    {
                        for i in 0..17u32 {
                            if (mask & (1u32 << i)) != 0 {
                                data.sample_banks[i as usize] =
                                    find_bank(unsafe { *p.add(PROGRAM_DEFAULT_BANK_OFFSET) });
                            }
                        }
                    }
                } else {
                    for layer in 0..3u32 {
                        let p = unsafe { data.base.add(layer as usize * 0x76) };
                        let mut marker: u32 = 0;
                        let mut tag: u32 = 0;
                        for i in (0..16u32).step_by(2) {
                            if (unsafe { *p.add(i as usize) } & 128) != 0 {
                                marker |= 1u32 << i;
                            }
                            if (unsafe { *p.add(20 + i as usize) } & 128) != 0 {
                                tag |= 1u32 << (i / 2);
                            }
                        }
                        if marker == 0x5515 {
                            data.sample_banks[layer as usize] = find_bank(tag as u8);
                        }
                    }
                }
            }
        }
    }
}
