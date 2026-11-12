//! Ports of the C++ `runtime/tests/audio.cpp` tests that drive the reference board and chips.

use crate::board::Board;
use crate::es5510::ES5510;
use crate::mc68681::MC68681;
use crate::state_io::{StateReader, StateWriter};
use f3rt_audio::memory_map::{SOUND_DUART_BASE, SOUND_ES5505_BASE, SOUND_ES5510_BASE, SOUND_SHARED_BASE, SOUND_VOLUME_BASE};
use std::cell::Cell;
use std::rc::Rc;

const OTIS_PAGE: u32 = 0x20001e;
const SND_WORK_BYTE: u32 = 0x600;

const fn duart_reg(reg: u32) -> u32 {
    SOUND_DUART_BASE + 2 * reg + 1
}

const fn otis_reg(reg: u32) -> u32 {
    SOUND_ES5505_BASE + 2 * reg
}

fn make_board(sample_rom: &[u8], shared: *mut u8) -> Board {
    Board::new(&[], sample_rom, shared, 0x800)
}

fn preset(d: &mut MC68681, count: u32) {
    d.write(6, (count >> 8) as u8);
    d.write(7, count as u8);
}

#[test]
fn duart_counter_restart_and_expiry() {
    let mut restart = MC68681::new();
    preset(&mut restart, 3);
    restart.write(4, 0x30);
    restart.read(14);
    restart.advance(17);
    restart.read(14);
    restart.advance(47);
    assert_eq!(
        restart.read(5) & 8,
        0,
        "Restarting the counter discards the preceding divider phase"
    );
    restart.advance(1);
    assert!(
        (restart.read(5) & 8) != 0 && restart.read(6) == 0xff && restart.read(7) == 0xff,
        "Counter expires at the new deadline and reloads the reference 0xffff period"
    );
    restart.advance(16);
    assert_eq!(
        restart.read(7),
        0xfe,
        "Counter underflow reload is independent of the programmed preset"
    );
    restart.read(15);
    restart.advance(1000000);
    assert_eq!(
        restart.read(5) & 8,
        0,
        "Stop-counter read acknowledges and cancels counter-mode expiration"
    );
}

#[test]
fn duart_timer_mode_and_clock_source() {
    let mut mode = MC68681::new();
    preset(&mut mode, 3);
    mode.write(4, 0x30);
    mode.read(14);
    mode.advance(17);
    mode.read(15);
    mode.write(4, 0x60);
    mode.advance(5);
    assert_eq!(
        mode.read(5) & 8,
        0,
        "Entering timer mode starts a fresh full period without old divider residue"
    );
    mode.advance(1);
    assert_ne!(mode.read(5) & 8, 0, "Timer ready asserts after both half-periods");
    mode.read(15);
    mode.advance(6);
    assert_ne!(
        mode.read(5) & 8,
        0,
        "Timer-mode acknowledgement does not stop periodic interrupts"
    );

    let mut source = MC68681::new();
    preset(&mut source, 2);
    source.write(4, 0x70);
    source.advance(8);
    source.write(4, 0x60);
    source.advance(23);
    assert!(
        source.read(7) == 1 && (source.read(5) & 8) == 0,
        "Clock-source change preserves the armed duration"
    );
    source.advance(1);
    source.advance(1);
    assert_eq!(
        source.read(5) & 8,
        0,
        "The next half-period uses the newly selected source"
    );
    source.advance(1);
    assert_ne!(
        source.read(5) & 8,
        0,
        "Reload adopts the new clock without rescaling elapsed time"
    );
}

#[test]
fn duart_reset_retains_expiration() {
    let mut reset = MC68681::new();
    reset.advance(1000000);
    assert_eq!(reset.read(5), 0, "A cold DUART has no scheduled counter event");
    preset(&mut reset, 100);
    reset.write(4, 0x60);
    reset.advance(37);
    reset.reset();
    assert!(
        reset.read(5) == 0 && !reset.irq_pending(),
        "Board reset clears the visible IRQ registers"
    );
    reset.advance(62);
    assert_eq!(
        reset.read(5),
        0,
        "Board reset preserves the remaining deadline, not a restarted period"
    );
    reset.advance(1);
    assert!(
        reset.read(5) == 8 && !reset.irq_pending(),
        "Retained expiration latches counter-ready while reset IMR masks IRQ"
    );
    reset.write(12, 64);
    reset.write(5, 8);
    assert!(
        reset.irq_pending() && reset.get_irq_vector() == 64 && reset.irq_pending(),
        "Unmasking retained counter-ready asserts IRQ; IACK supplies vector without clearing it"
    );
    reset.read(15);
    assert!(
        !reset.irq_pending(),
        "Counter acknowledge clears a retained post-reset interrupt"
    );
}

#[test]
fn duart_tx_framing_and_interrupts() {
    for channel in [0u32, 1] {
        let mut duart = MC68681::new();
        let base = channel * 8;
        let half: u32 = if channel != 0 { 64 } else { 32 };
        let mask: u8 = if channel != 0 { 0x10 } else { 0x01 };
        let irq = Rc::new(Cell::new(false));
        {
            let irq = Rc::clone(&irq);
            duart.set_irq_callback(Box::new(move |asserted| irq.set(asserted)));
        }
        let configure = |duart: &mut MC68681| {
            duart.write(base, 0x13);
            duart.write(base, 0x0f);
            duart.write(base + 1, 0xee);
            duart.write(5, mask);
            duart.write(base + 2, 4);
        };
        assert_eq!(duart.read(base + 1) & 0x0c, 0, "Reset disables the UART transmitter");
        configure(&mut duart);
        assert!(
            (duart.read(base + 1) & 0x0c) == 0x0c && irq.get(),
            "Enabling an idle transmitter asserts ready/empty and its IRQ"
        );
        duart.write(base + 3, 0x80);
        assert!(
            (duart.read(base + 1) & 0x0c) == 0 && !irq.get(),
            "THR write clears ready and empty"
        );
        duart.advance(3 * half - 1);
        assert_eq!(
            duart.read(base + 1) & 0x0c,
            0,
            "TX ready waits through the start-bit time"
        );
        duart.advance(1);
        assert!(
            (duart.read(base + 1) & 0x0c) == 4 && irq.get(),
            "The second rising edge releases THR and asserts TX ready"
        );
        duart.advance(18 * half - 1);
        assert_eq!(
            duart.read(base + 1) & 8,
            0,
            "TX empty stays clear until the last framed bit"
        );
        duart.advance(1);
        assert_eq!(
            duart.read(base + 1) & 0x0c,
            0x0c,
            "First 8N2 frame completes after 21 half-bit clocks"
        );
        duart.write(base + 3, 0x90);
        duart.advance(4 * half);
        duart.write(base + 3, 0x7f);
        duart.write(base + 3, 0x44);
        assert!(
            (duart.read(base + 1) & 0x0c) == 0 && !irq.get(),
            "A queued byte occupies THR while the current byte shifts"
        );
        duart.advance(18 * half);
        assert!(
            (duart.read(base + 1) & 0x0c) == 4 && irq.get(),
            "Buffered transfer restores ready without asserting empty"
        );
        duart.advance(22 * half - 1);
        assert_eq!(
            duart.read(base + 1) & 8,
            0,
            "Queued frame retains the continuous serial edge phase"
        );
        duart.advance(1);
        assert_eq!(
            duart.read(base + 1) & 0x0c,
            0x0c,
            "Holding-register overflow does not enqueue an extra frame"
        );
        duart.reset();
        configure(&mut duart);
        duart.write(base + 3, 0x80);
        duart.advance(22 * half - 1);
        assert_eq!(
            duart.read(base + 1) & 8,
            0,
            "Board reset retains the stopped serial clock edge state"
        );
        duart.advance(1);
        assert_ne!(
            duart.read(base + 1) & 8,
            0,
            "Post-reset frame uses a full first bit period"
        );
        duart.write(base + 3, 0x55);
        duart.advance(4 * half);
        duart.write(base + 2, 0x30);
        duart.advance(100 * half);
        assert!(
            (duart.read(base + 1) & 0x0c) == 0 && !irq.get(),
            "Transmitter reset aborts the frame and clears its IRQ"
        );
        duart.write(base + 2, 0x10);
        duart.write(base, 0);
        duart.write(base, 7);
        duart.write(base + 1, 0xef);
        duart.write(base + 2, 4);
        duart.write(base + 3, 0x15);
        let fast_half = half / 16;
        duart.advance(16 * fast_half - 1);
        assert_eq!(
            duart.read(base + 1) & 8,
            0,
            "Word length, parity and direct external clock determine frame duration"
        );
        duart.advance(1);
        assert_ne!(
            duart.read(base + 1) & 8,
            0,
            "5E1 frame completes at its eight-bit boundary"
        );
    }
}

#[test]
fn duart_counter_derived_tx_clock() {
    let mut counter_clock = MC68681::new();
    counter_clock.write(6, 0);
    counter_clock.write(7, 1);
    counter_clock.write(4, 0x60);
    counter_clock.write(8, 0x13);
    counter_clock.write(8, 0x0f);
    counter_clock.write(9, 0xed);
    counter_clock.write(10, 4);
    counter_clock.write(11, 0x80);
    counter_clock.read(14);
    counter_clock.advance(46);
    assert_eq!(
        counter_clock.read(9) & 0x0c,
        0,
        "Counter-derived TX clock retains the divide-by-16 prescaler"
    );
    counter_clock.advance(1);
    assert_eq!(
        counter_clock.read(9) & 0x0c,
        4,
        "Counter-derived TX ready follows the second serial rising edge"
    );
    counter_clock.advance(287);
    assert_eq!(
        counter_clock.read(9) & 8,
        0,
        "Counter-derived TX empty waits for the full frame"
    );
    counter_clock.advance(1);
    assert_eq!(
        counter_clock.read(9) & 0x0c,
        0x0c,
        "Counter-derived 8N2 frame completes at its eleventh serial edge"
    );
}

/// `CanonicalES5510Registers` (171 bytes, packed little-endian) as the leading bytes of the
/// ES5510 snapshot; only the fields the tests touch have accessors.
struct Es5510Registers(Vec<u8>);

impl Es5510Registers {
    const SIZE: usize = 171;
    const HALT_ASSERTED: usize = 0;
    const PC: usize = 1;
    const STATE: usize = 2;
    const MEMMASK: usize = 34;
    const MEMINCREMENT: usize = 38;
    const MEMSHIFT: usize = 42;
    const DLENGTH: usize = 43;
    const DBASE: usize = 55;
    const RAM_P_ADDRESS: usize = 159;

    fn i32_at(&self, at: usize) -> i32 {
        i32::from_le_bytes(self.0[at..at + 4].try_into().unwrap())
    }

    fn set_i32(&mut self, at: usize, v: i32) {
        self.0[at..at + 4].copy_from_slice(&v.to_le_bytes());
    }
}

struct Es5510Harness {
    dsp: ES5510,
    saved: Vec<u8>,
}

impl Es5510Harness {
    fn new() -> Es5510Harness {
        let dsp = ES5510::new();
        let saved = vec![0u8; dsp.state_size()];
        Es5510Harness { dsp, saved }
    }

    fn capture(&mut self) -> Es5510Registers {
        let mut writer = StateWriter::new(&mut self.saved);
        self.dsp.save_state(&mut writer);
        Es5510Registers(self.saved[..Es5510Registers::SIZE].to_vec())
    }

    fn restore(&mut self, state: &Es5510Registers) {
        self.saved[..Es5510Registers::SIZE].copy_from_slice(&state.0);
        let mut reader = StateReader::new(&self.saved);
        self.dsp.load_state(&mut reader);
    }
}

#[test]
fn dsp_end_and_safety_budget_ordering() {
    let mut h = Es5510Harness::new();
    for halted in [false, true] {
        for end in [-1i32, 0, 1, 159] {
            h.dsp.reset();
            if end >= 0 {
                *h.dsp.instr_at(end) = 0xf000;
            }
            let mut state = h.capture();
            state.0[Es5510Registers::STATE] = if halted { 1 } else { 0 };
            h.restore(&state);
            h.dsp.run_once();
            let state = h.capture();
            let expected: u32 = if end < 0 {
                if halted { 200 } else { 201 }
            } else if end == 0 {
                if halted { 1 } else { 161 }
            } else {
                (end + 1) as u32
            };
            assert!(
                state.0[Es5510Registers::PC] as u32 == expected
                    && state.0[Es5510Registers::STATE] == (if end < 0 { 0 } else { 1 })
                    && state.0[Es5510Registers::HALT_ASSERTED] != 0,
                "DSP END and safety budget preserve wake-up and first-cycle HALT ordering"
            );
        }
    }
    for pc in [159u32, 160, 255] {
        h.dsp.reset();
        let mut state = h.capture();
        state.0[Es5510Registers::PC] = pc as u8;
        h.restore(&state);
        h.dsp.run_once();
        let state = h.capture();
        assert!(
            state.0[Es5510Registers::PC] == (pc + 201) as u8 && state.0[Es5510Registers::STATE] == 0,
            "DSP no-END safety cutoff preserves the 8-bit PC wrap above the instruction store"
        );
    }
}

fn delay_addressing_state(address: i32) -> Es5510Registers {
    let mut state = Es5510Registers(vec![0u8; Es5510Registers::SIZE]);
    state.0[Es5510Registers::PC] = 1;
    state.set_i32(Es5510Registers::DBASE, address);
    state.set_i32(Es5510Registers::DLENGTH, 16);
    state.set_i32(Es5510Registers::MEMINCREMENT, 1);
    state.0[Es5510Registers::MEMSHIFT] = 0;
    state.set_i32(Es5510Registers::MEMMASK, 0xffffff);
    state
}

#[test]
fn dsp_delay_addressing() {
    let mut h = Es5510Harness::new();
    for address in [-1i32, 0, 16, 17, 33, 34, 127] {
        h.dsp.reset();
        *h.dsp.instr_at(1) = 0xf000;
        let _ = h.capture();
        let state = delay_addressing_state(address);
        h.restore(&state);
        h.dsp.run_once();
        let state = h.capture();
        assert_eq!(
            state.i32_at(Es5510Registers::RAM_P_ADDRESS),
            (address % 17) & 0xffffff,
            "DSP delay addressing preserves signed remainder and zero, one and multiple wraps"
        );
    }
}

struct XorShift64(u64);

impl XorShift64 {
    fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x
    }
}

#[test]
fn dsp_delay_addressing_modulo_invariant() {
    let mut rng = XorShift64(0x9e3779b97f4a7c15);
    let mut addresses = vec![-1000i32, 999, -17, 17, 0];
    for _ in 0..200 {
        addresses.push((rng.next() % 2000) as i32 - 1000);
    }
    for address in addresses {
        let expected = (address % 17) & 0xffffff;
        let mut h = Es5510Harness::new();
        h.dsp.reset();
        *h.dsp.instr_at(1) = 0xf000;
        let state = delay_addressing_state(address);
        h.restore(&state);
        h.dsp.run_once();
        let s2 = h.capture();
        assert_eq!(s2.i32_at(Es5510Registers::RAM_P_ADDRESS), expected, "address {address}");
    }
}

#[test]
fn audio_mixer_board_gain_and_mute() {
    let rom: [u8; 4] = [0x40, 0, 0x40, 0];
    let mut audio = make_board(&rom, std::ptr::null_mut());
    audio.write16(OTIS_PAGE, 0x20);
    for reg in 1..=6u32 {
        audio.write16(otis_reg(reg), 0x0800);
    }
    audio.write16(OTIS_PAGE, 0);
    audio.write16(otis_reg(8), 0xef00);
    audio.write16(otis_reg(9), 0xef00);
    audio.write16(otis_reg(0), 0x0d00);
    let esp_reg = |reg: u32| SOUND_ES5510_BASE + 2 * reg + 1;
    let esp_instruction = |audio: &mut Board, index: u32, word: u64| {
        for byte in 0..6u32 {
            audio.write8(esp_reg(0x03 + byte), (word >> (40 - 8 * byte)) as u8);
        }
        audio.write8(esp_reg(0xc0), index as u8);
    };
    esp_instruction(&mut audio, 0, 0x0000ebf19000u64);
    esp_instruction(&mut audio, 1, 0x0000eaf09000u64);
    esp_instruction(&mut audio, 2, 0x00000000f000u64);
    audio.write8(duart_reg(14), 0x40);
    let mut pcm = [0i16; 2];
    let sample = |audio: &mut Board, pcm: &mut [i16; 2]| {
        audio.advance(538);
        assert_eq!(audio.render(pcm, 1), 1, "One complete audio sample is available");
    };
    sample(&mut audio, &mut pcm);
    assert!(
        pcm[0] == 2136 && pcm[1] == 2136,
        "Default gain matches post-DAC analog attenuation"
    );
    audio.write8(SOUND_VOLUME_BASE, 2);
    audio.write8(SOUND_VOLUME_BASE + 2, 0);
    sample(&mut audio, &mut pcm);
    assert!(
        pcm[0] == 2136 && pcm[1] == 0,
        "Channel-0 volume mute follows its data through the DAC swap and preserves channel 1"
    );
    audio.write8(SOUND_VOLUME_BASE, 7);
    audio.write8(SOUND_VOLUME_BASE + 2, 0x33);
    sample(&mut audio, &mut pcm);
    assert!(
        pcm[0] == 1070 && pcm[1] == 0,
        "Minus-six-dB control attenuates once and preserves channel mute"
    );
    audio.set_reset(false);
    audio.set_reset(true);
    sample(&mut audio, &mut pcm);
    assert!(
        pcm[0] == 1070 && pcm[1] == 0,
        "CPU-line reset preserves attenuation and playing OTIS voices"
    );
    audio.reset_board();
    sample(&mut audio, &mut pcm);
    assert!(
        pcm[0] == 2136 && pcm[1] == 2136,
        "Board reset restores volume without resetting OTIS voices"
    );
}

#[test]
fn audio_sample_rate_no_clock_drift() {
    let mut clock_audio = make_board(&[], std::ptr::null_mut());
    let mut clock_samples = [0i16; 128];
    let mut sample_count: u64 = 0;
    for _ in 0..10000u32 {
        clock_audio.advance(16000);
        sample_count += clock_audio.render(&mut clock_samples, 64) as u64;
    }
    assert_eq!(
        sample_count,
        clock_audio.sample_rate() as u64 * 10,
        "Ten seconds of audio match the advertised stream rate without clock drift"
    );
    clock_audio.advance(638);
    clock_audio.reset_board();
    clock_audio.advance(438);
    assert_eq!(
        clock_audio.render(&mut clock_samples, 64),
        2,
        "Board reset preserves queued audio and fractional sample-clock phase"
    );
}

#[test]
fn dpram_otis_and_sound_reset() {
    let mut shared = [0u8; 0x800];
    let mut samples = vec![0u8; 0x2468c];
    samples[0x2468a] = 0x45;
    samples[0x2468b] = 0x67;
    let mut audio = make_board(&samples, shared.as_mut_ptr());
    unsafe { *shared.as_mut_ptr().add(0x10) = 0x75 };
    assert_eq!(
        audio.read16(SOUND_SHARED_BASE + 0x20),
        0x75ff,
        "DPRAM sound high-byte lane"
    );
    audio.write16(SOUND_SHARED_BASE + 0x20, 0xaabb);
    assert_eq!(unsafe { *shared.as_ptr().add(0x10) }, 0xaa, "DPRAM reverse lane");
    audio.write16(OTIS_PAGE, 0);
    audio.write16(otis_reg(10), 0x0246);
    audio.write16(otis_reg(11), 0x8a00);
    audio.write16(OTIS_PAGE, 0x20);
    assert_eq!(
        audio.read16(otis_reg(6)),
        0x4567,
        "OTIS stopped voice reads full 20-bit sample address"
    );
    audio.write16(SND_WORK_BYTE, 0xa55a);
    audio.set_reset(false);
    audio.set_reset(true);
    audio.set_reset(false);
    assert_eq!(
        audio.read16(SND_WORK_BYTE),
        0xa55a,
        "Sound CPU RESET preserves board work RAM"
    );
}

#[test]
fn cpu_line_reset_preserves_dsp_and_duart() {
    let mut audio = make_board(&[], std::ptr::null_mut());
    audio.set_reset(true);
    audio.write8(duart_reg(12), 0x66);
    audio.write8(SOUND_ES5510_BASE + 1, 0x12);
    audio.write8(SOUND_ES5510_BASE + 3, 0x34);
    audio.write8(SOUND_ES5510_BASE + 5, 0x56);
    audio.write8(SOUND_ES5510_BASE + 0x141, 0);
    audio.set_reset(false);
    audio.set_reset(true);
    audio.write8(SOUND_ES5510_BASE + 0x101, 0);
    assert!(
        audio.read8(SOUND_ES5510_BASE + 1) == 0x12 && audio.read8(duart_reg(12)) == 0x66,
        "CPU-line reset preserves DSP registers and DUART configuration"
    );
}
