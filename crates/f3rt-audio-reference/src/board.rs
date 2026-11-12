//! `audio/reference/board.cpp`: the Taito EN sound board around the reference engine.
//!
//! `Board::Impl` of the C++ (a pimpl) is `Board` itself. The Rust board is single-threaded behind
//! `&mut self`, so the ring buffer needs no `m_audio_mutex`. The chip callbacks that the C++ binds
//! to `this` write into shared cells that the board folds into its own state right after the chip
//! call that can raise them.

use crate::engine::Engine;
use crate::es5505::ES5505;
use crate::mb87078::MB87078;
use crate::mc68681::MC68681;
use crate::state_io::{StateReader, StateWriter};
use f3rt_audio::memory_map::{
    SHARED_RAM_SIZE, SOUND_DUART_BASE, SOUND_DUART_END, SOUND_RAM_END, SOUND_RAM_HIGH_BASE,
    SOUND_RAM_MASK, SOUND_ROM_BASE, SOUND_ROM_END, SOUND_SHARED_BASE, SOUND_SHARED_END,
    SOUND_VOLUME_BASE, SOUND_VOLUME_END,
};
use f3rt_audio::output_gain::OUTPUT_BOOST;
use std::cell::Cell;
use std::rc::Rc;

const MAX_VOICES: usize = ES5505::MAX_VOICES as usize;
const WORK_RAM_SIZE: usize = 0x10000;
const RING_BUFFER_CAPACITY: usize = 32768;
const CANONICAL_AUDIO_CORE_SIZE: usize = 136;

fn clamp_f32(v: f32, lo: f32, hi: f32) -> f32 {
    if v < lo {
        lo
    } else if hi < v {
        hi
    } else {
        v
    }
}

fn clamp16(val: i32) -> i16 {
    if val > 32767 {
        return 32767;
    }
    if val < -32768 {
        return -32768;
    }
    val as i16
}

/// This is the only place the exchange is applied.
fn swap_dac_channels<T>(interleaved_stereo: &mut [T], frames: usize) -> usize {
    for i in 0..frames {
        interleaved_stereo.swap(i * 2, i * 2 + 1);
    }
    frames
}

#[derive(Default)]
struct CanonicalAudioCore {
    bank_mask: u32,
    bank_table: [u16; 32],
    reset_asserted: u8,
    esp_halted: u8,
    gain_model: u8,
    cpubank2: u8,
    volume_gain: [f32; 2],
    otis_gain: [f32; 2],
    output_gain: [f32; 2],
    cpu_accum: i64,
    duart_accum: u32,
    sample_accum: u64,
    clock_ticks: u64,
    generated_frames: u64,
    rb_count: u32,
}

impl CanonicalAudioCore {
    fn write(&self, writer: &mut StateWriter) {
        writer.write_u32(self.bank_mask);
        for v in self.bank_table {
            writer.write_u16(v);
        }
        writer.write_u8(self.reset_asserted);
        writer.write_u8(self.esp_halted);
        writer.write_u8(self.gain_model);
        writer.write_u8(self.cpubank2);
        for v in self.volume_gain {
            writer.write_f32(v);
        }
        for v in self.otis_gain {
            writer.write_f32(v);
        }
        for v in self.output_gain {
            writer.write_f32(v);
        }
        writer.write_i64(self.cpu_accum);
        writer.write_u32(self.duart_accum);
        writer.write_u64(self.sample_accum);
        writer.write_u64(self.clock_ticks);
        writer.write_u64(self.generated_frames);
        writer.write_u32(self.rb_count);
    }

    fn read(reader: &mut StateReader) -> CanonicalAudioCore {
        let mut core = CanonicalAudioCore::default();
        core.bank_mask = reader.read_u32();
        for v in core.bank_table.iter_mut() {
            *v = reader.read_u16();
        }
        core.reset_asserted = reader.read_u8();
        core.esp_halted = reader.read_u8();
        core.gain_model = reader.read_u8();
        core.cpubank2 = reader.read_u8();
        for v in core.volume_gain.iter_mut() {
            *v = reader.read_f32();
        }
        for v in core.otis_gain.iter_mut() {
            *v = reader.read_f32();
        }
        for v in core.output_gain.iter_mut() {
            *v = reader.read_f32();
        }
        core.cpu_accum = reader.read_i64();
        core.duart_accum = reader.read_u32();
        core.sample_accum = reader.read_u64();
        core.clock_ticks = reader.read_u64();
        core.generated_frames = reader.read_u64();
        core.rb_count = reader.read_u32();
        core.validate();
        core
    }

    fn validate(&self) {
        let valid = self.rb_count <= 32768
            && self.gain_model <= 1
            && self.reset_asserted <= 1
            && self.esp_halted <= 1
            && self.cpubank2 <= 31
            && self.sample_accum < 16000000
            && self.duart_accum < 4
            && self.cpu_accum < 16000000
            && self.cpu_accum >= -16000000i64 * 1024;
        if !valid {
            panic!("Snapshot field outside canonical representation: CanonicalAudioCore");
        }
    }
}

pub struct Board {
    m_work_ram: Vec<u8>,
    m_sound_rom: Vec<u8>,
    m_sample_words: Vec<u16>,
    m_reference: Engine,

    m_shared_ram: *mut u8,
    m_shared_ram_size: usize,

    m_duart: MC68681,
    m_volume: MB87078,

    m_volume_gain: Rc<Cell<[f32; 2]>>,
    m_esp_halted_pending: Rc<Cell<Option<bool>>>,
    m_otis_gain: [f32; 2],
    m_output_gain: [f32; 2],

    m_reset_asserted: bool,
    m_cpubank2: u32,

    m_cpu_runner: Option<Box<dyn FnMut(i32) -> i32>>,
    m_reset_cb: Option<Box<dyn FnMut(bool)>>,

    m_cpu_accum: i64,
    m_duart_accum: u32,
    m_sample_accum: u64,
    m_clock_ticks: u64,
    m_generated_frames: u64,

    m_sample_buffer: Vec<f32>,
    m_rb_read_pos: usize,
    m_rb_write_pos: usize,
    m_rb_count: usize,
}

impl Board {
    /// `shared_ram` is non-owning, as in the C++: the caller keeps `shared_ram_size` bytes alive
    /// and unaliased by other references for the board's lifetime (or passes null).
    pub fn new(
        sound_rom: &[u8],
        sample_rom: &[u8],
        shared_ram: *mut u8,
        shared_ram_size: usize,
    ) -> Board {
        let mut m_duart = MC68681::new();
        let mut m_volume = MB87078::new();
        let m_volume_gain = Rc::new(Cell::new([1.0f32, 1.0f32]));
        let m_esp_halted_pending = Rc::new(Cell::new(None));

        {
            let pending = Rc::clone(&m_esp_halted_pending);
            m_duart.set_outport_callback(Box::new(move |output_pins: u8| {
                pending.set(Some((output_pins & 0x40) != 0));
            }));
        }
        {
            let volume_gain = Rc::clone(&m_volume_gain);
            m_volume.set_gain_callback(Box::new(move |channel: i32, gain: f32| {
                if channel >= 2 {
                    let mut g = volume_gain.get();
                    g[(channel & 1) as usize] = gain;
                    volume_gain.set(g);
                }
            }));
        }

        let mut board = Board {
            m_work_ram: vec![0; WORK_RAM_SIZE],
            m_sound_rom: Vec::new(),
            m_sample_words: Vec::new(),
            m_reference: Engine::new(),
            m_shared_ram: std::ptr::null_mut(),
            m_shared_ram_size: SHARED_RAM_SIZE,
            m_duart,
            m_volume,
            m_volume_gain,
            m_esp_halted_pending,
            m_otis_gain: [0.0; 2],
            m_output_gain: [0.0; 2],
            m_reset_asserted: true,
            m_cpubank2: 1,
            m_cpu_runner: None,
            m_reset_cb: None,
            m_cpu_accum: 0,
            m_duart_accum: 0,
            m_sample_accum: 0,
            m_clock_ticks: 0,
            m_generated_frames: 0,
            m_sample_buffer: vec![0.0f32; RING_BUFFER_CAPACITY * 2],
            m_rb_read_pos: 0,
            m_rb_write_pos: 0,
            m_rb_count: 0,
        };
        board.update_gains();

        board.m_sound_rom = sound_rom.to_vec();
        if board.m_sound_rom.len() >= 8 {
            board.m_work_ram[..8].copy_from_slice(&board.m_sound_rom[..8]);
        }
        let word_count = sample_rom.len() / 2;
        board.m_sample_words = vec![0; word_count];
        for i in 0..word_count {
            board.m_sample_words[i] =
                ((sample_rom[i * 2] as u16) << 8) | sample_rom[i * 2 + 1] as u16;
        }
        unsafe {
            board
                .m_reference
                .set_sample_rom(board.m_sample_words.as_ptr(), word_count)
        };
        board.m_reference.set_bank_mask(if sample_rom.len() >= 0x200000 {
            (sample_rom.len() / 0x200000) as u32 - 1
        } else {
            0
        });
        board.m_shared_ram = shared_ram;
        board.m_shared_ram_size = shared_ram_size;
        board
    }

    fn sync_esp_halted(&mut self) {
        if let Some(halted) = self.m_esp_halted_pending.take() {
            self.m_reference.set_esp_halted(halted);
        }
    }

    fn update_gains(&mut self) {
        let volume_gain = self.m_volume_gain.get();
        for channel in 0..2usize {
            let physical = volume_gain[channel];
            self.m_otis_gain[channel] = 0.18f32;
            self.m_output_gain[channel] = OUTPUT_BOOST * physical;
        }
    }

    pub fn set_cpu_runner(&mut self, runner: Box<dyn FnMut(i32) -> i32>) {
        self.m_cpu_runner = Some(runner);
    }

    pub fn set_reset(&mut self, asserted: bool) {
        if self.m_reset_asserted != asserted {
            self.m_reset_asserted = asserted;
            if let Some(cb) = self.m_reset_cb.as_mut() {
                cb(asserted);
            }
        }
    }

    pub fn is_reset(&self) -> bool {
        self.m_reset_asserted
    }

    pub fn set_reset_callback(&mut self, cb: Box<dyn FnMut(bool)>) {
        self.m_reset_cb = Some(cb);
    }

    pub fn reset_board(&mut self) {
        self.set_reset(true);
        self.m_cpu_accum = 0;
        self.m_cpubank2 = 1;
        self.m_duart.reset();
        self.sync_esp_halted();
        self.m_reference.reset();
        self.m_volume.reset();
        self.update_gains();
        if self.m_sound_rom.len() >= 8 {
            self.m_work_ram[..8].copy_from_slice(&self.m_sound_rom[..8]);
        }
    }

    pub fn set_sound_bank(&mut self, page: u32) {
        self.m_cpubank2 = page;
    }

    pub fn sound_bank(&self) -> u32 {
        self.m_cpubank2
    }

    fn push_frame(&mut self, left: f32, right: f32) {
        if self.m_rb_count >= RING_BUFFER_CAPACITY {
            self.m_rb_read_pos = (self.m_rb_read_pos + 1) % RING_BUFFER_CAPACITY;
            self.m_rb_count -= 1;
        }
        self.m_sample_buffer[self.m_rb_write_pos * 2 + 0] = left;
        self.m_sample_buffer[self.m_rb_write_pos * 2 + 1] = right;
        self.m_rb_write_pos = (self.m_rb_write_pos + 1) % RING_BUFFER_CAPACITY;
        self.m_rb_count += 1;
    }

    fn generate_one_frame(&mut self) {
        self.m_generated_frames += 1;
        let mut frame = (0.0f32, 0.0f32);
        self.m_reference.generate_one_frame(
            &self.m_otis_gain,
            &self.m_output_gain,
            &mut |l, r| frame = (l, r),
        );
        self.push_frame(frame.0, frame.1);
    }

    fn advance_slice(&mut self, main_cycles: u32) {
        self.m_clock_ticks += main_cycles as u64;
        self.m_duart_accum += main_cycles;
        let duart_cycles = self.m_duart_accum / 4;
        self.m_duart_accum %= 4;

        if duart_cycles > 0 {
            self.m_duart.advance(duart_cycles);
            self.sync_esp_halted();
        }

        self.m_sample_accum += main_cycles as u64 * self.m_reference.sample_rate() as u64;
        while self.m_sample_accum >= 16000000u64 {
            self.m_sample_accum -= 16000000u64;
            self.generate_one_frame();
        }

        if !self.m_reset_asserted && self.m_cpu_runner.is_some() {
            self.m_cpu_accum += main_cycles as i64 * 15238090i64;
            if self.m_cpu_accum >= 16000000i64 {
                let cycles = (self.m_cpu_runner.as_mut().unwrap())(1);
                self.m_cpu_accum -= cycles as i64 * 16000000i64;
            }
        } else {
            self.m_cpu_accum = 0;
        }
    }

    pub fn advance(&mut self, main_cycles: u32) {
        let mut main_cycles = main_cycles;
        while main_cycles > 0 {
            let mut step = main_cycles;
            if !self.m_reset_asserted && self.m_cpu_runner.is_some() {
                let until_cpu =
                    ((16000000i64 - self.m_cpu_accum + 15238089i64) / 15238090i64) as u32;
                step = step.min(until_cpu);
            }
            let rate = self.m_reference.sample_rate();
            if self.m_sample_accum + step as u64 * rate as u64 >= 16000000u64 {
                step = ((16000000u64 - self.m_sample_accum + rate as u64 - 1) / rate as u64) as u32;
            }
            main_cycles -= step;
            self.advance_slice(step);
        }
    }

    fn shared_ram_get(&self, idx: usize) -> u8 {
        if !self.m_shared_ram.is_null() && idx < self.m_shared_ram_size {
            unsafe { *self.m_shared_ram.add(idx) }
        } else {
            0xff
        }
    }

    fn shared_ram_set(&mut self, idx: usize, value: u8) {
        if !self.m_shared_ram.is_null() && idx < self.m_shared_ram_size {
            unsafe { *self.m_shared_ram.add(idx) = value };
        }
    }

    pub fn read8(&mut self, address: u32) -> u8 {
        let address = address & 0x00ffffff;

        if address < SOUND_RAM_END {
            return self.m_work_ram[(address & SOUND_RAM_MASK) as usize];
        }

        if address >= SOUND_SHARED_BASE && address < SOUND_SHARED_END {
            if (address & 1) == 0 {
                let idx = ((address - SOUND_SHARED_BASE) >> 1) as usize;
                return self.shared_ram_get(idx);
            }
            return 0xff;
        }

        if self.m_reference.is_mapped(address) {
            return self.m_reference.read8(address);
        }

        if address >= SOUND_DUART_BASE && address < SOUND_DUART_END {
            if address & 1 != 0 {
                let value = self.m_duart.read((address >> 1) & 0x0f);
                self.sync_esp_halted();
                return value;
            }
            return 0xff;
        }

        if address >= SOUND_VOLUME_BASE && address < SOUND_VOLUME_END {
            if (address & 1) == 0 {
                let offset = (address >> 1) & 1;
                return self.m_volume.read(offset ^ 1);
            }
            return 0xff;
        }

        if address >= SOUND_ROM_BASE && address < SOUND_ROM_END {
            let mut off = address - SOUND_ROM_BASE;
            if self.m_sound_rom.len() > 0x80000 {
                if address >= 0xc20000 && address < 0xc40000 {
                    off = self.m_cpubank2.wrapping_mul(0x20000).wrapping_add(address - 0xc20000);
                } else if address >= 0xc40000 {
                    off = 0x40000 + (address - 0xc40000);
                }
            }
            return if (off as usize) < self.m_sound_rom.len() {
                self.m_sound_rom[off as usize]
            } else {
                0xff
            };
        }

        if address >= SOUND_RAM_HIGH_BASE {
            return self.m_work_ram[(address & SOUND_RAM_MASK) as usize];
        }

        0xff
    }

    pub fn read16(&mut self, address: u32) -> u16 {
        let address = address & 0x00fffffe;

        if address < SOUND_RAM_END {
            let off = (address & 0xfffe) as usize;
            return ((self.m_work_ram[off] as u16) << 8) | self.m_work_ram[off + 1] as u16;
        }

        if address >= SOUND_SHARED_BASE && address < SOUND_SHARED_END {
            let idx = ((address - SOUND_SHARED_BASE) >> 1) as usize;
            let data = self.shared_ram_get(idx);
            return ((data as u16) << 8) | 0xff;
        }

        if self.m_reference.is_mapped(address) {
            return self.m_reference.read16(address);
        }

        if address >= SOUND_DUART_BASE && address < SOUND_DUART_END {
            let value = self.m_duart.read((address >> 1) & 0x0f);
            self.sync_esp_halted();
            return value as u16;
        }

        if address >= SOUND_VOLUME_BASE && address < SOUND_VOLUME_END {
            let offset = (address >> 1) & 1;
            return (self.m_volume.read(offset ^ 1) as u16) << 8;
        }

        if address >= SOUND_ROM_BASE && address < SOUND_ROM_END {
            let hi = self.read8(address) as u16;
            return (hi << 8) | self.read8(address + 1) as u16;
        }

        if address >= SOUND_RAM_HIGH_BASE {
            let off = (address & 0xfffe) as usize;
            return ((self.m_work_ram[off] as u16) << 8) | self.m_work_ram[off + 1] as u16;
        }

        0xffff
    }

    pub fn read32(&mut self, address: u32) -> u32 {
        let hi = self.read16(address) as u32;
        (hi << 16) | self.read16(address.wrapping_add(2)) as u32
    }

    pub fn write8(&mut self, address: u32, value: u8) {
        let address = address & 0x00ffffff;

        if address < SOUND_RAM_END {
            self.m_work_ram[(address & SOUND_RAM_MASK) as usize] = value;
            return;
        }

        if address >= SOUND_SHARED_BASE && address < SOUND_SHARED_END {
            if (address & 1) == 0 {
                let idx = ((address - SOUND_SHARED_BASE) >> 1) as usize;
                self.shared_ram_set(idx, value);
            }
            return;
        }

        if self.m_reference.is_mapped(address) {
            self.m_reference.write8(address, value);
            return;
        }

        if address >= SOUND_DUART_BASE && address < SOUND_DUART_END {
            if address & 1 != 0 {
                self.m_duart.write((address >> 1) & 0x0f, value);
                self.sync_esp_halted();
            }
            return;
        }

        if address >= SOUND_VOLUME_BASE && address < SOUND_VOLUME_END {
            if (address & 1) == 0 {
                let offset = (address >> 1) & 1;
                self.m_volume.write(offset ^ 1, value);
                self.update_gains();
            }
            return;
        }

        if address >= SOUND_RAM_HIGH_BASE {
            self.m_work_ram[(address & SOUND_RAM_MASK) as usize] = value;
        }
    }

    pub fn write16(&mut self, address: u32, value: u16) {
        let address = address & 0x00fffffe;

        if address < SOUND_RAM_END {
            let off = (address & 0xfffe) as usize;
            self.m_work_ram[off] = ((value >> 8) & 0xff) as u8;
            self.m_work_ram[off + 1] = (value & 0xff) as u8;
            return;
        }

        if address >= SOUND_SHARED_BASE && address < SOUND_SHARED_END {
            let idx = ((address - SOUND_SHARED_BASE) >> 1) as usize;
            self.shared_ram_set(idx, ((value >> 8) & 0xff) as u8);
            return;
        }

        if self.m_reference.is_mapped(address) {
            self.m_reference.write16(address, value);
            return;
        }

        if address >= SOUND_DUART_BASE && address < SOUND_DUART_END {
            self.m_duart.write((address >> 1) & 0x0f, (value & 0xff) as u8);
            self.sync_esp_halted();
            return;
        }

        if address >= SOUND_VOLUME_BASE && address < SOUND_VOLUME_END {
            let offset = (address >> 1) & 1;
            self.m_volume.write(offset ^ 1, ((value >> 8) & 0xff) as u8);
            self.update_gains();
            return;
        }

        if address >= SOUND_RAM_HIGH_BASE {
            let off = (address & 0xfffe) as usize;
            self.m_work_ram[off] = ((value >> 8) & 0xff) as u8;
            self.m_work_ram[off + 1] = (value & 0xff) as u8;
        }
    }

    pub fn write32(&mut self, address: u32, value: u32) {
        self.write16(address, (value >> 16) as u16);
        self.write16(address.wrapping_add(2), (value & 0xffff) as u16);
    }

    pub fn irq_level(&self) -> i32 {
        if self.m_duart.irq_pending() {
            return 6;
        }
        0
    }

    pub fn irq_ack(&mut self, level: i32) -> u8 {
        if level == 6 {
            return self.m_duart.get_irq_vector();
        }
        (0x18 + level) as u8
    }

    pub fn set_irq_callback(&mut self, cb: Box<dyn FnMut(bool)>) {
        self.m_duart.set_irq_callback(cb);
    }

    pub fn clock_ticks(&self) -> u64 {
        self.m_clock_ticks
    }

    pub fn generated_frames(&self) -> u64 {
        self.m_generated_frames
    }

    pub fn sample_rate(&self) -> u32 {
        self.m_reference.sample_rate()
    }

    pub fn available_frames(&self) -> usize {
        self.m_rb_count
    }

    /// `render(int16_t *, size_t)`: `interleaved_stereo` holds at least `2 * max_frames` samples.
    pub fn render(&mut self, interleaved_stereo: &mut [i16], max_frames: usize) -> usize {
        let frames = max_frames.min(self.m_rb_count);
        for i in 0..frames {
            let l = self.m_sample_buffer[self.m_rb_read_pos * 2 + 0];
            let r = self.m_sample_buffer[self.m_rb_read_pos * 2 + 1];
            self.m_rb_read_pos = (self.m_rb_read_pos + 1) % RING_BUFFER_CAPACITY;

            interleaved_stereo[i * 2 + 0] =
                clamp16((clamp_f32(l, -1.0f32, 1.0f32) * 32768.0f32) as i32);
            interleaved_stereo[i * 2 + 1] =
                clamp16((clamp_f32(r, -1.0f32, 1.0f32) * 32768.0f32) as i32);
        }
        self.m_rb_count -= frames;
        swap_dac_channels(interleaved_stereo, frames)
    }

    /// `render(float *, size_t)`.
    pub fn render_f32(&mut self, interleaved_stereo: &mut [f32], max_frames: usize) -> usize {
        let frames = max_frames.min(self.m_rb_count);
        for i in 0..frames {
            interleaved_stereo[i * 2 + 0] = self.m_sample_buffer[self.m_rb_read_pos * 2 + 0];
            interleaved_stereo[i * 2 + 1] = self.m_sample_buffer[self.m_rb_read_pos * 2 + 1];
            self.m_rb_read_pos = (self.m_rb_read_pos + 1) % RING_BUFFER_CAPACITY;
        }
        self.m_rb_count -= frames;
        swap_dac_channels(interleaved_stereo, frames)
    }

    pub fn render_ready(&mut self, interleaved_stereo: &mut [i16], max_frames: usize) -> usize {
        self.render(interleaved_stereo, max_frames)
    }

    pub fn state_size(&self) -> usize {
        CANONICAL_AUDIO_CORE_SIZE
            + WORK_RAM_SIZE
            + std::mem::size_of::<f32>() * RING_BUFFER_CAPACITY * 2
            + self.m_reference.es5505_ref().state_size()
            + self.m_reference.es5510_ref().state_size()
            + self.m_duart.state_size()
            + self.m_volume.state_size()
    }

    fn write_state(&self, writer: &mut StateWriter) {
        let mut core = CanonicalAudioCore::default();
        core.bank_mask = self.m_reference.bank_mask();
        for i in 0..MAX_VOICES {
            core.bank_table[i] = self.m_reference.bank_table(i as i32);
        }
        core.reset_asserted = self.m_reset_asserted as u8;
        core.esp_halted = self.m_reference.esp_halted() as u8;
        core.gain_model = 1;
        core.cpubank2 = self.m_cpubank2 as u8;
        core.volume_gain = self.m_volume_gain.get();
        core.otis_gain = self.m_otis_gain;
        core.output_gain = self.m_output_gain;
        core.cpu_accum = self.m_cpu_accum;
        core.duart_accum = self.m_duart_accum;
        core.sample_accum = self.m_sample_accum;
        core.clock_ticks = self.m_clock_ticks;
        core.generated_frames = self.m_generated_frames;
        core.rb_count = self.m_rb_count as u32;
        core.write(writer);

        writer.write_bytes(&self.m_work_ram);

        if self.m_rb_count > 0 {
            let first_part = self.m_rb_count.min(RING_BUFFER_CAPACITY - self.m_rb_read_pos);
            for v in &self.m_sample_buffer[self.m_rb_read_pos * 2..(self.m_rb_read_pos + first_part) * 2] {
                writer.write_f32(*v);
            }
            if self.m_rb_count > first_part {
                let second_part = self.m_rb_count - first_part;
                for v in &self.m_sample_buffer[0..second_part * 2] {
                    writer.write_f32(*v);
                }
            }
        }
        let zero_frames = RING_BUFFER_CAPACITY - self.m_rb_count;
        if zero_frames > 0 {
            let bytes = zero_frames * 2 * std::mem::size_of::<f32>();
            if writer.remaining() < bytes {
                panic!("StateWriter buffer overflow");
            }
            writer.write_bytes(&vec![0u8; bytes]);
        }

        self.m_reference.save_state(writer);
        self.m_duart.save_state(writer);
        self.m_volume.save_state(writer);
    }

    fn read_state(&mut self, reader: &mut StateReader) {
        let core = CanonicalAudioCore::read(reader);
        self.m_reference.set_bank_mask(core.bank_mask);
        for i in 0..MAX_VOICES {
            self.m_reference.set_bank_table(i as i32, core.bank_table[i]);
        }
        self.m_reset_asserted = core.reset_asserted != 0;
        self.m_reference.set_esp_halted(core.esp_halted != 0);
        self.m_cpubank2 = core.cpubank2 as u32;
        self.m_volume_gain.set(core.volume_gain);
        self.m_otis_gain = core.otis_gain;
        self.m_output_gain = core.output_gain;
        self.m_cpu_accum = core.cpu_accum;
        self.m_duart_accum = core.duart_accum;
        self.m_sample_accum = core.sample_accum;
        self.m_clock_ticks = core.clock_ticks;
        self.m_generated_frames = core.generated_frames;

        reader.read_bytes(&mut self.m_work_ram);

        self.m_rb_count = (core.rb_count as usize).min(RING_BUFFER_CAPACITY);
        self.m_rb_read_pos = 0;
        self.m_rb_write_pos = self.m_rb_count % RING_BUFFER_CAPACITY;
        for i in 0..self.m_rb_count * 2 {
            self.m_sample_buffer[i] = reader.read_f32();
        }
        for v in &mut self.m_sample_buffer[self.m_rb_count * 2..] {
            *v = 0.0;
        }
        reader.advance((RING_BUFFER_CAPACITY - self.m_rb_count) * 2 * std::mem::size_of::<f32>());

        self.m_reference.load_state(reader);
        self.m_duart.load_state(reader);
        self.sync_esp_halted();
        self.m_volume.load_state(reader);
        self.update_gains();
    }

    pub fn save_state(&self, dst: &mut [u8]) {
        if dst.len() != self.state_size() {
            panic!("Board::save_state size mismatch");
        }
        let mut writer = StateWriter::new(dst);
        self.write_state(&mut writer);
        if writer.remaining() != 0 {
            panic!("Board::save_state remaining unwritten bytes");
        }
    }

    pub fn load_state(&mut self, src: &[u8]) {
        if src.len() != self.state_size() {
            panic!("Board::load_state size mismatch");
        }
        let mut reader = StateReader::new(src);
        self.read_state(&mut reader);
        if reader.remaining() != 0 {
            panic!("Board::load_state remaining unread bytes");
        }
    }
}
