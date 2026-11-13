//! `runtime/eeprom.hpp` — the 93C46 serial EEPROM — and `include/f3rt/abi/eeprom.h` over it.
//!
//! The C++ and this crate are the same machine: same 64 words, same modes, same serial-interface
//! state, same operation order and the same programming delays. `abi` holds the one process-wide
//! instance; the C++ shim's failure line (an abort with the message) is reproduced here.

use std::fs::File;
use std::io::{ErrorKind, Read};
use std::path::Path;

/// The 64 x 16 organization: `std::array<uint16_t, 64> Eeprom::words`.
pub const WORDS: usize = 64;
/// The file image: 64 big-endian words.
pub const FILE_BYTES: usize = 128;
/// `sizeof(CanonicalEeprom)`: the save-state blob.
pub const STATE_BYTES: usize = 157;

/// Absolute 16 MHz main-clock ticks a single-word erase takes.
const ERASE_CYCLES: u64 = 16000;
/// Absolute 16 MHz main-clock ticks a single-word write takes.
const WRITE_CYCLES: u64 = 28000;
/// Absolute 16 MHz main-clock ticks a write-all or erase-all takes.
const WRITE_ALL_CYCLES: u64 = 128000;

/// The C++ `require_state` message for a blob outside the canonical representation.
const OUTSIDE_CANONICAL: &str = "Snapshot field outside canonical representation";

/// `Eeprom::Mode`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum Mode {
    Command = 0,
    Read = 1,
    Write = 2,
    WriteAll = 3,
    Done = 4,
}

impl Mode {
    fn from_u8(value: u8) -> Option<Mode> {
        Some(match value {
            0 => Mode::Command,
            1 => Mode::Read,
            2 => Mode::Write,
            3 => Mode::WriteAll,
            4 => Mode::Done,
            _ => return None,
        })
    }
}

/// 93C46, 64 x 16 organization. DO read dummy bit and sequential reads included.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Eeprom {
    pub words: [u16; WORDS],
    mode: Mode,
    selected: bool,
    old_clock: bool,
    data_out: bool,
    writable: bool,
    shift: u32,
    count: u32,
    address: u32,
    read_bit: u32,
    ready_at: u64,
}

impl Default for Eeprom {
    fn default() -> Eeprom {
        Eeprom {
            words: [0xffff; WORDS],
            mode: Mode::Command,
            selected: false,
            old_clock: false,
            data_out: true,
            writable: false,
            shift: 0,
            count: 0,
            address: 0,
            read_bit: 0,
            ready_at: 0,
        }
    }
}

impl Eeprom {
    /// Absolute 16 MHz main-clock ticks. Programming completion is independent of serial clock
    /// edges; DO reports busy when CS is raised to poll it.
    pub fn output(&self, now: u64) -> bool {
        if self.selected && self.mode == Mode::Command && self.count == 0 {
            now >= self.ready_at
        } else {
            self.data_out
        }
    }

    pub fn reset(&mut self) {
        self.selected = false;
        self.old_clock = false;
        self.writable = false;
        self.ready_at = 0;
        self.reset_command();
    }

    pub fn pins(&mut self, pins: u8, now: u64) {
        let select = (pins & 0x10) != 0;
        let clock = (pins & 0x08) != 0;
        let bit = (pins & 0x04) != 0;
        if !select {
            self.selected = false;
            self.old_clock = clock;
            self.reset_command();
            return;
        }
        if !self.selected {
            self.selected = true;
            self.reset_command();
        }
        if clock && !self.old_clock {
            self.edge(bit, now);
        }
        self.old_clock = clock;
    }

    /// The factory-image seeding of `Machine`: 128 big-endian bytes into `words`, nothing else.
    pub fn seed_be_bytes(&mut self, image: &[u8; FILE_BYTES]) {
        for (index, word) in self.words.iter_mut().enumerate() {
            *word = u16::from_be_bytes([image[2 * index], image[2 * index + 1]]);
        }
    }

    /// `Eeprom::load`: a missing file is a no-op, a wrong size or an unreadable file is an error.
    pub fn load(&mut self, path: &Path) -> Result<(), LoadError> {
        let mut file = match File::open(path) {
            Ok(file) => file,
            Err(error) if error.kind() == ErrorKind::NotFound => return Ok(()),
            Err(_) => return Err(LoadError::InvalidFile),
        };
        let size = file.metadata().map_err(|_| LoadError::InvalidFile)?.len();
        if size != FILE_BYTES as u64 {
            return Err(LoadError::InvalidFile);
        }
        let mut image = [0u8; FILE_BYTES];
        file.read_exact(&mut image).map_err(|_| LoadError::Io)?;
        self.seed_be_bytes(&image);
        Ok(())
    }

    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        let mut image = [0u8; FILE_BYTES];
        for (index, word) in self.words.iter().enumerate() {
            image[2 * index..2 * index + 2].copy_from_slice(&word.to_be_bytes());
        }
        std::fs::write(path, image)
    }

    pub fn save_state(&self, dst: &mut [u8; STATE_BYTES]) {
        let state = CanonicalEeprom {
            words: self.words,
            mode: self.mode as u8,
            selected: self.selected as u8,
            old_clock: self.old_clock as u8,
            data_out: self.data_out as u8,
            writable: self.writable as u8,
            shift: self.shift,
            count: self.count,
            address: self.address,
            read_bit: self.read_bit,
            ready_at: self.ready_at,
        };
        state.write(dst);
    }

    /// Applies a blob only if it is canonical; an invalid one leaves the instance unchanged.
    pub fn load_state(&mut self, src: &[u8; STATE_BYTES]) -> Result<(), &'static str> {
        let state = CanonicalEeprom::parse(src)?;
        let mode = Mode::from_u8(state.mode).ok_or(OUTSIDE_CANONICAL)?;
        self.words = state.words;
        self.mode = mode;
        self.selected = state.selected != 0;
        self.old_clock = state.old_clock != 0;
        self.data_out = state.data_out != 0;
        self.writable = state.writable != 0;
        self.shift = state.shift;
        self.count = state.count;
        self.address = state.address;
        self.read_bit = state.read_bit;
        self.ready_at = state.ready_at;
        Ok(())
    }

    fn reset_command(&mut self) {
        self.mode = Mode::Command;
        self.shift = 0;
        self.count = 0;
        self.data_out = true;
    }

    fn edge(&mut self, bit: bool, now: u64) {
        if self.mode == Mode::Read {
            self.data_out = ((self.words[self.address as usize] >> (15 - self.read_bit)) & 1) != 0;
            self.read_bit += 1;
            if self.read_bit == 16 {
                self.read_bit = 0;
                self.address = (self.address + 1) & 63;
            }
            return;
        }
        if self.mode == Mode::Done {
            return;
        }
        if self.mode == Mode::Command && self.count == 0 && (!bit || now < self.ready_at) {
            return;
        }
        self.shift = (self.shift << 1) | bit as u32;
        self.count += 1;
        if self.mode == Mode::Write || self.mode == Mode::WriteAll {
            if self.count == 16 {
                if self.writable {
                    if self.mode == Mode::WriteAll {
                        self.words.fill(self.shift as u16);
                    } else {
                        self.words[self.address as usize] = self.shift as u16;
                    }
                    self.ready_at = now
                        + if self.mode == Mode::WriteAll {
                            WRITE_ALL_CYCLES
                        } else {
                            WRITE_CYCLES
                        };
                }
                self.mode = Mode::Done;
                self.data_out = true;
            }
            return;
        }
        if self.count != 9 {
            return;
        }
        self.address = self.shift & 63;
        match (self.shift >> 6) & 3 {
            2 => {
                self.mode = Mode::Read;
                self.read_bit = 0;
                self.data_out = false;
            }
            1 => {
                self.mode = Mode::Write;
                self.count = 0;
                self.shift = 0;
            }
            3 => {
                if self.writable {
                    self.words[self.address as usize] = 0xffff;
                    self.ready_at = now + ERASE_CYCLES;
                }
                self.mode = Mode::Done;
            }
            _ => match self.address >> 4 {
                0 => {
                    self.writable = false;
                    self.mode = Mode::Done;
                }
                1 => {
                    self.mode = Mode::WriteAll;
                    self.count = 0;
                    self.shift = 0;
                }
                2 => {
                    if self.writable {
                        self.words.fill(0xffff);
                        self.ready_at = now + WRITE_ALL_CYCLES;
                    }
                    self.mode = Mode::Done;
                }
                3 => {
                    self.writable = true;
                    self.mode = Mode::Done;
                }
                _ => {}
            },
        }
    }
}

/// `Eeprom::load`'s two failures; a missing file is not one of them.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LoadError {
    /// The file is not exactly `FILE_BYTES` bytes (C++: "Invalid EEPROM file").
    InvalidFile,
    /// The file could not be read (C++: "EEPROM read failed").
    Io,
}

// `CanonicalEeprom` in `runtime/state_io.hpp`: the packed, host-endian save-state layout.
pub const WORDS_AT: usize = 0;
pub const MODE_AT: usize = 128;
pub const SELECTED_AT: usize = 129;
pub const OLD_CLOCK_AT: usize = 130;
pub const DATA_OUT_AT: usize = 131;
pub const WRITABLE_AT: usize = 132;
pub const SHIFT_AT: usize = 133;
pub const COUNT_AT: usize = 137;
pub const ADDRESS_AT: usize = 141;
pub const READ_BIT_AT: usize = 145;
pub const READY_AT: usize = 149;

const _: () = assert!(WORDS_AT == 0 && MODE_AT == WORDS * 2);
const _: () = assert!(READY_AT + 8 == STATE_BYTES);
const _: () = assert!(SHIFT_AT + 4 == COUNT_AT && COUNT_AT + 4 == ADDRESS_AT);
const _: () = assert!(ADDRESS_AT + 4 == READ_BIT_AT && READ_BIT_AT + 4 == READY_AT);

/// The serialized `Eeprom`: `#pragma pack(1)` in C++, so this is a field-by-field byte view.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct CanonicalEeprom {
    pub words: [u16; WORDS],
    pub mode: u8,
    pub selected: u8,
    pub old_clock: u8,
    pub data_out: u8,
    pub writable: u8,
    pub shift: u32,
    pub count: u32,
    pub address: u32,
    pub read_bit: u32,
    pub ready_at: u64,
}

impl CanonicalEeprom {
    pub fn parse(src: &[u8; STATE_BYTES]) -> Result<CanonicalEeprom, &'static str> {
        let mut words = [0u16; WORDS];
        for (index, word) in words.iter_mut().enumerate() {
            *word = u16::from_ne_bytes([src[2 * index], src[2 * index + 1]]);
        }
        let state = CanonicalEeprom {
            words,
            mode: src[MODE_AT],
            selected: src[SELECTED_AT],
            old_clock: src[OLD_CLOCK_AT],
            data_out: src[DATA_OUT_AT],
            writable: src[WRITABLE_AT],
            shift: read_ne_u32_at(src, SHIFT_AT),
            count: read_ne_u32_at(src, COUNT_AT),
            address: read_ne_u32_at(src, ADDRESS_AT),
            read_bit: read_ne_u32_at(src, READ_BIT_AT),
            ready_at: read_ne_u64_at(src, READY_AT),
        };
        if state.valid() {
            Ok(state)
        } else {
            Err(OUTSIDE_CANONICAL)
        }
    }

    pub fn write(&self, dst: &mut [u8; STATE_BYTES]) {
        for (index, word) in self.words.iter().enumerate() {
            dst[2 * index..2 * index + 2].copy_from_slice(&word.to_ne_bytes());
        }
        dst[MODE_AT] = self.mode;
        dst[SELECTED_AT] = self.selected;
        dst[OLD_CLOCK_AT] = self.old_clock;
        dst[DATA_OUT_AT] = self.data_out;
        dst[WRITABLE_AT] = self.writable;
        write_ne_u32_at(dst, SHIFT_AT, self.shift);
        write_ne_u32_at(dst, COUNT_AT, self.count);
        write_ne_u32_at(dst, ADDRESS_AT, self.address);
        write_ne_u32_at(dst, READ_BIT_AT, self.read_bit);
        write_ne_u64_at(dst, READY_AT, self.ready_at);
    }

    /// `validate_state_value(const CanonicalEeprom &)`.
    fn valid(&self) -> bool {
        self.mode <= 4
            && self.selected <= 1
            && self.old_clock <= 1
            && self.data_out <= 1
            && self.writable <= 1
            && self.address < WORDS as u32
            && self.read_bit < 16
            && self.count <= 16
    }
}

fn read_ne_u32_at(src: &[u8; STATE_BYTES], at: usize) -> u32 {
    u32::from_ne_bytes([src[at], src[at + 1], src[at + 2], src[at + 3]])
}

fn read_ne_u64_at(src: &[u8; STATE_BYTES], at: usize) -> u64 {
    u64::from_ne_bytes([
        src[at],
        src[at + 1],
        src[at + 2],
        src[at + 3],
        src[at + 4],
        src[at + 5],
        src[at + 6],
        src[at + 7],
    ])
}

fn write_ne_u32_at(dst: &mut [u8; STATE_BYTES], at: usize, value: u32) {
    dst[at..at + 4].copy_from_slice(&value.to_ne_bytes());
}

fn write_ne_u64_at(dst: &mut [u8; STATE_BYTES], at: usize, value: u64) {
    dst[at..at + 8].copy_from_slice(&value.to_ne_bytes());
}

pub mod abi {
    //! `include/f3rt/abi/eeprom.h`: one process-wide `Eeprom`, single-threaded per the header.
    //!
    //! The C++ shim held a `std::unique_ptr<f3rt::Eeprom>` and turned failures into an abort with
    //! the message; the same status codes and the same abort line come out here.
    #![allow(non_camel_case_types)] // The names are `include/f3rt/abi/eeprom.h`'s.

    use std::ffi::{CStr, c_char};
    use std::path::PathBuf;
    use std::sync::Mutex;

    use crate::{Eeprom, FILE_BYTES, LoadError, STATE_BYTES, WORDS};

    pub type f3rt_eeprom_status = i32;
    pub const F3RT_EEPROM_OK: f3rt_eeprom_status = 0;
    pub const F3RT_EEPROM_ALREADY_INITIALIZED: f3rt_eeprom_status = 1;
    pub const F3RT_EEPROM_INVALID_STATE: f3rt_eeprom_status = 2;
    pub const F3RT_EEPROM_INVALID_FILE: f3rt_eeprom_status = 3;
    pub const F3RT_EEPROM_IO_ERROR: f3rt_eeprom_status = 4;

    /// `F3RT_EEPROM_WORDS`: the 93C46's 64 x 16 organization.
    pub const F3RT_EEPROM_WORDS: u32 = 64;
    /// `F3RT_EEPROM_FILE_BYTES`: the big-endian file image.
    pub const F3RT_EEPROM_FILE_BYTES: u32 = 128;
    /// `F3RT_EEPROM_STATE_SIZE`: `sizeof(CanonicalEeprom)`.
    pub const F3RT_EEPROM_STATE_SIZE: u32 = 157;

    const _: () = assert!(F3RT_EEPROM_WORDS as usize == WORDS);
    const _: () = assert!(F3RT_EEPROM_FILE_BYTES as usize == FILE_BYTES);
    const _: () = assert!(F3RT_EEPROM_STATE_SIZE as usize == STATE_BYTES);

    static INSTANCE: Mutex<Option<Eeprom>> = Mutex::new(None);

    #[inline(never)]
    fn fail(function: &str, what: &str) -> ! {
        eprintln!("f3rt: {function}: {what}");
        std::process::abort()
    }

    fn with<R>(function: &str, body: impl FnOnce(&mut Eeprom) -> R) -> R {
        let mut instance = INSTANCE
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        match instance.as_mut() {
            Some(eeprom) => body(eeprom),
            None => fail(function, "eeprom is not initialized"),
        }
    }

    /// `std::filesystem::path(const char *)`: native bytes on unix, UTF-8 lossy elsewhere.
    fn c_path(path: &CStr) -> PathBuf {
        let bytes = path.to_bytes();
        #[cfg(unix)]
        {
            use std::ffi::OsStr;
            use std::os::unix::ffi::OsStrExt;
            PathBuf::from(OsStr::from_bytes(bytes))
        }
        #[cfg(not(unix))]
        {
            PathBuf::from(String::from_utf8_lossy(bytes).into_owned())
        }
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn f3rt_eeprom_init() -> f3rt_eeprom_status {
        let mut instance = INSTANCE
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if instance.is_some() {
            return F3RT_EEPROM_ALREADY_INITIALIZED;
        }
        *instance = Some(Eeprom::default());
        F3RT_EEPROM_OK
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn f3rt_eeprom_shutdown() {
        *INSTANCE
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner) = None;
    }

    /// # Safety
    ///
    /// `bytes` must point to `F3RT_EEPROM_FILE_BYTES` readable bytes.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn f3rt_eeprom_seed_be_bytes(bytes: *const u8) {
        with("f3rt_eeprom_seed_be_bytes", |eeprom| {
            // SAFETY: the caller vouches for F3RT_EEPROM_FILE_BYTES readable bytes.
            let image = unsafe { &*bytes.cast::<[u8; FILE_BYTES]>() };
            eeprom.seed_be_bytes(image);
        });
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn f3rt_eeprom_reset() {
        with("f3rt_eeprom_reset", |eeprom| eeprom.reset());
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn f3rt_eeprom_pins(pins: u8, now: u64) {
        with("f3rt_eeprom_pins", |eeprom| eeprom.pins(pins, now));
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn f3rt_eeprom_output(now: u64) -> bool {
        with("f3rt_eeprom_output", |eeprom| eeprom.output(now))
    }

    /// # Safety
    ///
    /// `path` must be a NUL-terminated C string.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn f3rt_eeprom_load_file(path: *const c_char) -> f3rt_eeprom_status {
        // SAFETY: the header requires a NUL-terminated C string.
        let path = c_path(unsafe { CStr::from_ptr(path) });
        with("f3rt_eeprom_load_file", |eeprom| match eeprom.load(&path) {
            Ok(()) => F3RT_EEPROM_OK,
            Err(LoadError::InvalidFile) => F3RT_EEPROM_INVALID_FILE,
            Err(LoadError::Io) => F3RT_EEPROM_IO_ERROR,
        })
    }

    /// # Safety
    ///
    /// `path` must be a NUL-terminated C string.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn f3rt_eeprom_save_file(path: *const c_char) -> f3rt_eeprom_status {
        // SAFETY: the header requires a NUL-terminated C string.
        let path = c_path(unsafe { CStr::from_ptr(path) });
        with("f3rt_eeprom_save_file", |eeprom| match eeprom.save(&path) {
            Ok(()) => F3RT_EEPROM_OK,
            Err(_) => F3RT_EEPROM_IO_ERROR,
        })
    }

    /// # Safety
    ///
    /// `dst` must point to `F3RT_EEPROM_STATE_SIZE` writable bytes.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn f3rt_eeprom_save_state(dst: *mut u8) {
        with("f3rt_eeprom_save_state", |eeprom| {
            // SAFETY: the caller vouches for F3RT_EEPROM_STATE_SIZE writable bytes.
            let dst = unsafe { &mut *dst.cast::<[u8; STATE_BYTES]>() };
            eeprom.save_state(dst);
        });
    }

    /// # Safety
    ///
    /// `src` must point to `F3RT_EEPROM_STATE_SIZE` readable bytes.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn f3rt_eeprom_load_state(src: *const u8) -> f3rt_eeprom_status {
        with("f3rt_eeprom_load_state", |eeprom| {
            // SAFETY: the caller vouches for F3RT_EEPROM_STATE_SIZE readable bytes.
            let src = unsafe { &*src.cast::<[u8; STATE_BYTES]>() };
            match eeprom.load_state(src) {
                Ok(()) => F3RT_EEPROM_OK,
                Err(_) => F3RT_EEPROM_INVALID_STATE,
            }
        })
    }
}

#[cfg(test)]
mod tests {
    use super::abi::*;
    use super::*;
    use std::ffi::CString;
    use std::path::PathBuf;

    fn send_bit(eeprom: &mut Eeprom, bit: bool, now: u64) {
        let pins = 0x10 | if bit { 4 } else { 0 };
        eeprom.pins(pins, now);
        eeprom.pins(pins | 8, now);
    }

    fn command(eeprom: &mut Eeprom, word: u16, now: u64) {
        eeprom.pins(0, now);
        for bit in (0..9).rev() {
            send_bit(eeprom, ((word >> bit) & 1) != 0, now);
        }
    }

    fn ewen(eeprom: &mut Eeprom, now: u64) {
        command(eeprom, 0x130, now);
        eeprom.pins(0, now);
    }

    fn serial_write(eeprom: &mut Eeprom, address: u16, value: u16, now: u64) {
        command(eeprom, 0x140 | address, now);
        for bit in (0..16).rev() {
            send_bit(eeprom, ((value >> bit) & 1) != 0, now);
        }
        eeprom.pins(0, now);
    }

    fn read_word(eeprom: &mut Eeprom, now: u64) -> u16 {
        let mut value = 0u16;
        for _ in 0..16 {
            send_bit(eeprom, false, now);
            value = (value << 1) | eeprom.output(now) as u16;
        }
        value
    }

    fn temporary_path(name: &str) -> PathBuf {
        let unique = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!(
            "f3rt-eeprom-{}-{name}-{unique}.nv",
            std::process::id()
        ))
    }

    #[test]
    fn serial_reads_wrap_from_address_63_to_zero() {
        let mut eeprom = Eeprom::default();
        eeprom.words[0] = 0x1234;
        eeprom.words[1] = 0x89ab;
        eeprom.words[63] = 0xfedc;
        command(&mut eeprom, 0x1bf, 0);
        assert!(!eeprom.output(0), "a read starts with the serial dummy bit");
        assert_eq!(read_word(&mut eeprom, 0), 0xfedc);
        assert_eq!(read_word(&mut eeprom, 0), 0x1234);
        assert_eq!(read_word(&mut eeprom, 0), 0x89ab);
    }

    #[test]
    fn writes_need_ewen_and_ewds_locks_again() {
        let mut eeprom = Eeprom::default();
        serial_write(&mut eeprom, 0, 0xa65c, 1000);
        assert_eq!(eeprom.words[0], 0xffff, "writes are ignored until EWEN");
        assert_eq!(eeprom.ready_at, 0);

        ewen(&mut eeprom, 2000);
        serial_write(&mut eeprom, 0, 0xa65c, 3000);
        assert_eq!(eeprom.words[0], 0xa65c);
        assert_eq!(eeprom.ready_at, 3000 + WRITE_CYCLES);

        command(&mut eeprom, 0x180, 40000);
        assert_eq!(read_word(&mut eeprom, 40000), 0xa65c);

        command(&mut eeprom, 0x100, 50000);
        eeprom.pins(0, 50000);
        serial_write(&mut eeprom, 0, 0x0001, 60000);
        assert_eq!(eeprom.words[0], 0xa65c, "EWDS locks writing again");
    }

    #[test]
    fn erase_needs_ewen_and_takes_erase_cycles() {
        let mut eeprom = Eeprom::default();
        eeprom.words[5] = 0x1234;
        command(&mut eeprom, 0x1c0 | 5, 10);
        assert_eq!(eeprom.words[5], 0x1234, "erase is ignored until EWEN");
        assert_eq!(eeprom.ready_at, 0);
        assert_eq!(eeprom.mode, Mode::Done);

        ewen(&mut eeprom, 100);
        command(&mut eeprom, 0x1c0 | 5, 200);
        assert_eq!(eeprom.words[5], 0xffff);
        assert_eq!(eeprom.ready_at, 200 + ERASE_CYCLES);
        assert_eq!(eeprom.mode, Mode::Done);
    }

    #[test]
    fn write_all_programs_every_word() {
        let mut eeprom = Eeprom::default();
        command(&mut eeprom, 0x110, 100);
        assert_eq!(eeprom.words[0], 0xffff, "write-all is ignored until EWEN");

        ewen(&mut eeprom, 200);
        command(&mut eeprom, 0x110, 300);
        for bit in (0..16).rev() {
            send_bit(&mut eeprom, ((0x5a5au16 >> bit) & 1) != 0, 300);
        }
        eeprom.pins(0, 300);
        assert!(eeprom.words.iter().all(|&word| word == 0x5a5a));
        assert_eq!(eeprom.ready_at, 300 + WRITE_ALL_CYCLES);
    }

    #[test]
    fn erase_all_clears_every_word() {
        let mut eeprom = Eeprom::default();
        eeprom.words[0] = 0x1234;
        command(&mut eeprom, 0x120, 100);
        assert_eq!(eeprom.words[0], 0x1234, "erase-all is ignored until EWEN");

        ewen(&mut eeprom, 200);
        command(&mut eeprom, 0x120, 300);
        assert!(eeprom.words.iter().all(|&word| word == 0xffff));
        assert_eq!(eeprom.ready_at, 300 + WRITE_ALL_CYCLES);
    }

    #[test]
    fn busy_polling_blocks_commands_until_ready_at() {
        let mut eeprom = Eeprom::default();
        ewen(&mut eeprom, 0);
        serial_write(&mut eeprom, 7, 0xbeef, 1000);
        let ready_at = eeprom.ready_at;
        assert_eq!(ready_at, 1000 + WRITE_CYCLES);

        eeprom.pins(0, 2000);
        eeprom.pins(0x10, 2000);
        assert!(!eeprom.output(ready_at - 1), "DO is low while programming");
        assert!(eeprom.output(ready_at), "DO reports ready at ready_at");

        command(&mut eeprom, 0x100, 2000);
        eeprom.pins(0, 2000);
        assert!(eeprom.writable, "bits clocked while busy are dropped");

        command(&mut eeprom, 0x100, ready_at);
        eeprom.pins(0, ready_at);
        assert!(!eeprom.writable, "EWDS takes effect once ready");
    }

    #[test]
    fn reset_keeps_words_and_clears_the_pending_delay() {
        let mut eeprom = Eeprom::default();
        ewen(&mut eeprom, 0);
        serial_write(&mut eeprom, 0, 0xa65c, 1000);
        eeprom.reset();
        let mut expected = Eeprom::default();
        expected.words[0] = 0xa65c;
        assert_eq!(eeprom, expected, "reset keeps the stored words");
        assert!(eeprom.output(2000), "reset clears the pending delay");
    }

    #[test]
    fn save_state_layout_and_round_trip() {
        let mut eeprom = Eeprom {
            words: [0x0102; WORDS],
            ..Eeprom::default()
        };
        eeprom.words[0] = 0x1234;
        eeprom.words[63] = 0xabcd;
        eeprom.mode = Mode::WriteAll;
        eeprom.selected = true;
        eeprom.old_clock = true;
        eeprom.data_out = false;
        eeprom.writable = true;
        eeprom.shift = 0xdead_beef;
        eeprom.count = 12;
        eeprom.address = 42;
        eeprom.read_bit = 7;
        eeprom.ready_at = 0x1122_3344_5566_7788;

        let mut blob = [0xa5u8; STATE_BYTES];
        eeprom.save_state(&mut blob);

        assert_eq!(STATE_BYTES, 157);
        assert_eq!(MODE_AT, 128);
        assert_eq!(SELECTED_AT, 129);
        assert_eq!(OLD_CLOCK_AT, 130);
        assert_eq!(DATA_OUT_AT, 131);
        assert_eq!(WRITABLE_AT, 132);
        assert_eq!(SHIFT_AT, 133);
        assert_eq!(COUNT_AT, 137);
        assert_eq!(ADDRESS_AT, 141);
        assert_eq!(READ_BIT_AT, 145);
        assert_eq!(READY_AT, 149);

        assert_eq!(&blob[0..2], &0x1234u16.to_ne_bytes(), "words[0]");
        assert_eq!(&blob[2..4], &0x0102u16.to_ne_bytes(), "words[1]");
        assert_eq!(&blob[126..128], &0xabcdu16.to_ne_bytes(), "words[63]");
        assert_eq!(blob[MODE_AT], 3, "mode");
        assert_eq!(blob[SELECTED_AT], 1, "selected");
        assert_eq!(blob[OLD_CLOCK_AT], 1, "old_clock");
        assert_eq!(blob[DATA_OUT_AT], 0, "data_out");
        assert_eq!(blob[WRITABLE_AT], 1, "writable");
        assert_eq!(
            &blob[SHIFT_AT..COUNT_AT],
            &0xdead_beefu32.to_ne_bytes(),
            "shift"
        );
        assert_eq!(&blob[COUNT_AT..ADDRESS_AT], &12u32.to_ne_bytes(), "count");
        assert_eq!(
            &blob[ADDRESS_AT..READ_BIT_AT],
            &42u32.to_ne_bytes(),
            "address"
        );
        assert_eq!(
            &blob[READ_BIT_AT..READY_AT],
            &7u32.to_ne_bytes(),
            "read_bit"
        );
        assert_eq!(
            &blob[READY_AT..STATE_BYTES],
            &0x1122_3344_5566_7788u64.to_ne_bytes(),
            "ready_at"
        );

        let state = CanonicalEeprom::parse(&blob).unwrap();
        assert_eq!(state.words, eeprom.words);
        assert_eq!(state.mode, 3);
        assert_eq!(state.shift, 0xdead_beef);
        assert_eq!(state.ready_at, 0x1122_3344_5566_7788);

        let mut restored = Eeprom::default();
        restored.load_state(&blob).unwrap();
        assert_eq!(restored, eeprom);
    }

    #[test]
    fn load_state_rejects_out_of_range_fields() {
        let mut target = Eeprom::default();
        target.words[0] = 0x4242;

        for (offset, value, field) in [
            (MODE_AT, 5u8, "mode"),
            (SELECTED_AT, 2, "selected"),
            (OLD_CLOCK_AT, 2, "old_clock"),
            (DATA_OUT_AT, 2, "data_out"),
            (WRITABLE_AT, 2, "writable"),
        ] {
            let mut blob = valid_blob();
            blob[offset] = value;
            assert_eq!(target.load_state(&blob), Err(OUTSIDE_CANONICAL), "{field}");
            assert_eq!(
                target.words[0], 0x4242,
                "{field} leaves the instance unchanged"
            );
            assert!(CanonicalEeprom::parse(&blob).is_err(), "{field}");
        }

        for (offset, value, field) in [
            (ADDRESS_AT, 64u32, "address"),
            (READ_BIT_AT, 16, "read_bit"),
            (COUNT_AT, 17, "count"),
        ] {
            let mut blob = valid_blob();
            blob[offset..offset + 4].copy_from_slice(&value.to_ne_bytes());
            assert_eq!(target.load_state(&blob), Err(OUTSIDE_CANONICAL), "{field}");
            assert_eq!(
                target.words[0], 0x4242,
                "{field} leaves the instance unchanged"
            );
        }

        let mut blob = valid_blob();
        blob[MODE_AT] = 4;
        blob[ADDRESS_AT..READ_BIT_AT].copy_from_slice(&63u32.to_ne_bytes());
        blob[READ_BIT_AT..READY_AT].copy_from_slice(&15u32.to_ne_bytes());
        blob[COUNT_AT..ADDRESS_AT].copy_from_slice(&16u32.to_ne_bytes());
        assert_eq!(target.load_state(&blob), Ok(()));
        assert_eq!(target.mode, Mode::Done);
        assert_eq!(target.address, 63);
        assert_eq!(target.read_bit, 15);
        assert_eq!(target.count, 16);
    }

    fn valid_blob() -> [u8; STATE_BYTES] {
        let mut blob = [0u8; STATE_BYTES];
        Eeprom::default().save_state(&mut blob);
        blob
    }

    #[test]
    fn missing_file_is_a_no_op() {
        let path = temporary_path("missing");
        let mut eeprom = Eeprom::default();
        eeprom.words[0] = 0x1234;
        assert_eq!(eeprom.load(&path), Ok(()));
        assert_eq!(eeprom.words[0], 0x1234);
        assert!(!path.exists());
    }

    #[test]
    fn wrong_size_file_is_rejected() {
        let path = temporary_path("short");
        std::fs::write(&path, [0u8; FILE_BYTES - 1]).unwrap();
        let mut eeprom = Eeprom::default();
        eeprom.words[0] = 0x1234;
        assert_eq!(eeprom.load(&path), Err(LoadError::InvalidFile));
        assert_eq!(eeprom.words[0], 0x1234);
        std::fs::remove_file(&path).unwrap();
    }

    #[test]
    fn file_round_trip_is_big_endian() {
        let path = temporary_path("round-trip");
        let mut eeprom = Eeprom::default();
        eeprom.words[0] = 0x1234;
        eeprom.words[63] = 0xabcd;
        eeprom.save(&path).unwrap();

        let image = std::fs::read(&path).unwrap();
        assert_eq!(image.len(), FILE_BYTES);
        assert_eq!(&image[0..2], &[0x12, 0x34]);
        assert_eq!(&image[126..128], &[0xab, 0xcd]);

        let mut restored = Eeprom::default();
        restored.load(&path).unwrap();
        assert_eq!(restored.words, eeprom.words);
        std::fs::remove_file(&path).unwrap();
    }

    #[test]
    fn abi_singleton_lifecycle() {
        f3rt_eeprom_shutdown();
        assert_eq!(f3rt_eeprom_init(), F3RT_EEPROM_OK);
        assert_eq!(f3rt_eeprom_init(), F3RT_EEPROM_ALREADY_INITIALIZED);

        let mut image = [0xffu8; FILE_BYTES];
        image[0] = 0x12;
        image[1] = 0x34;
        image[126] = 0xfe;
        image[127] = 0xdc;
        // SAFETY: `image` is F3RT_EEPROM_FILE_BYTES bytes.
        unsafe { f3rt_eeprom_seed_be_bytes(image.as_ptr()) };
        let mut blob = [0u8; STATE_BYTES];
        // SAFETY: `blob` is F3RT_EEPROM_STATE_SIZE bytes.
        unsafe { f3rt_eeprom_save_state(blob.as_mut_ptr()) };
        let state = CanonicalEeprom::parse(&blob).unwrap();
        assert_eq!(state.words[0], 0x1234);
        assert_eq!(state.words[1], 0xffff);
        assert_eq!(state.words[63], 0xfedc);

        f3rt_eeprom_reset();
        let mut reset_state = [0u8; STATE_BYTES];
        // SAFETY: both blobs are F3RT_EEPROM_STATE_SIZE bytes.
        unsafe { f3rt_eeprom_save_state(reset_state.as_mut_ptr()) };
        assert!(f3rt_eeprom_output(0), "DO is ready after a reset");
        f3rt_eeprom_pins(0x10, 0);
        let mut selected_state = [0u8; STATE_BYTES];
        unsafe { f3rt_eeprom_save_state(selected_state.as_mut_ptr()) };
        assert_ne!(selected_state, reset_state);

        // SAFETY: both blobs are F3RT_EEPROM_STATE_SIZE bytes.
        unsafe {
            assert_eq!(f3rt_eeprom_load_state(reset_state.as_ptr()), F3RT_EEPROM_OK);
            f3rt_eeprom_save_state(selected_state.as_mut_ptr());
        }
        assert_eq!(
            selected_state, reset_state,
            "load_state restores the instance"
        );

        let mut invalid = reset_state;
        invalid[MODE_AT] = 5;
        // SAFETY: `invalid` is F3RT_EEPROM_STATE_SIZE bytes.
        unsafe {
            assert_eq!(
                f3rt_eeprom_load_state(invalid.as_ptr()),
                F3RT_EEPROM_INVALID_STATE
            );
            f3rt_eeprom_save_state(selected_state.as_mut_ptr());
        }
        assert_eq!(
            selected_state, reset_state,
            "an invalid blob changes nothing"
        );

        let path = temporary_path("abi");
        let c_path = CString::new(path.to_str().unwrap()).unwrap();
        // SAFETY: `c_path` is NUL-terminated.
        unsafe {
            assert_eq!(f3rt_eeprom_load_file(c_path.as_ptr()), F3RT_EEPROM_OK);
            assert_eq!(f3rt_eeprom_save_file(c_path.as_ptr()), F3RT_EEPROM_OK);
            assert_eq!(f3rt_eeprom_load_file(c_path.as_ptr()), F3RT_EEPROM_OK);
        }
        let mut loaded = [0u8; STATE_BYTES];
        // SAFETY: `loaded` is F3RT_EEPROM_STATE_SIZE bytes.
        unsafe { f3rt_eeprom_save_state(loaded.as_mut_ptr()) };
        assert_eq!(loaded, reset_state);
        std::fs::remove_file(&path).unwrap();

        f3rt_eeprom_shutdown();
    }
}
