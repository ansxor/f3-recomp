#![allow(non_camel_case_types)] // The names are `include/f3rt/abi/audio.h`'s.

use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr::NonNull;
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};

use crate::{
    audio::{self, Audio},
    hle::{driver_layout::DriverLayout, events::SAMPLE_RATE},
    memory_map::SHARED_RAM_SIZE,
};

pub type f3rt_audio_status = i32;
pub const F3RT_AUDIO_OK: f3rt_audio_status = 0;
pub const F3RT_AUDIO_UNSUPPORTED_ROM: f3rt_audio_status = 1;
pub const F3RT_AUDIO_ALREADY_INITIALIZED: f3rt_audio_status = 2;
pub const F3RT_AUDIO_INVALID_STATE: f3rt_audio_status = 3;
pub const F3RT_AUDIO_INVALID_ARGUMENT: f3rt_audio_status = 4;
pub const F3RT_AUDIO_LAYOUT_MISMATCH: f3rt_audio_status = 5;

/// Size of the save-state blob written by `f3rt_audio_save_state`.
pub const F3RT_AUDIO_STATE_SIZE: u32 = 32;

const _: () = assert!(crate::snapshot::BYTES == F3RT_AUDIO_STATE_SIZE as usize);

#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct f3rt_audio_stats {
    pub commands: u64,
    pub rendered_frames: u64,
    pub voices_started: u64,
    pub render_ns: u64,
    pub voices_active: u32,
}

struct RawSharedRam(NonNull<[u8; SHARED_RAM_SIZE]>);

impl RawSharedRam {
    /// `None` for a null pointer.
    ///
    /// # Safety
    /// A non-null `ptr` must point to `SHARED_RAM_SIZE` bytes that stay valid until the session
    /// built on this value is dropped, and that nothing else accesses while an ABI call runs.
    unsafe fn from_raw(ptr: *mut u8) -> Option<RawSharedRam> {
        NonNull::new(ptr.cast()).map(RawSharedRam)
    }

    /// # Safety
    /// The `from_raw` contract holds and no `&mut` to the RAM is live.
    unsafe fn as_ref(&self) -> &[u8; SHARED_RAM_SIZE] {
        unsafe { self.0.as_ref() }
    }

    /// # Safety
    /// The `from_raw` contract holds and no other reference to the RAM is live.
    unsafe fn as_mut(&mut self) -> &mut [u8; SHARED_RAM_SIZE] {
        unsafe { self.0.as_mut() }
    }
}

// SAFETY: the pointer is only dereferenced inside an ABI call, and `INSTANCE` serializes those
// calls, so the RAM is never touched from two threads at once.
unsafe impl Send for RawSharedRam {}

struct AbiSession {
    audio: Audio,
    shared_ram: RawSharedRam,
}

impl AbiSession {
    fn with_ram<R>(&mut self, body: impl FnOnce(&mut Audio, &[u8; SHARED_RAM_SIZE]) -> R) -> R {
        // SAFETY: `init` built `shared_ram` under the `from_raw` contract; this `&` ends with
        // `body`, and the C++ caller is inside this call, not touching the RAM.
        let ram = unsafe { self.shared_ram.as_ref() };
        body(&mut self.audio, ram)
    }

    fn with_ram_mut<R>(
        &mut self,
        body: impl FnOnce(&mut Audio, &mut [u8; SHARED_RAM_SIZE]) -> R,
    ) -> R {
        let ram = unsafe { self.shared_ram.as_mut() };
        body(&mut self.audio, ram)
    }
}

static INSTANCE: Mutex<Option<AbiSession>> = Mutex::new(None);

fn instance() -> MutexGuard<'static, Option<AbiSession>> {
    INSTANCE.lock().unwrap_or_else(PoisonError::into_inner)
}

#[inline(never)]
fn fail(function: &str, what: &str) -> ! {
    eprintln!("f3rt: {function}: {what}");
    std::process::abort()
}

/// Calls that must not unwind into C++ run inside `catch_unwind`, which needs the closure to be
/// unwind safe: a failure aborts the process, so no caller sees a half-updated session.
fn guarded<R>(function: &str, body: impl FnOnce(&mut AbiSession) -> R) -> R {
    let mut instance = instance();
    let Some(session) = instance.as_mut() else {
        fail(function, "audio is not initialized");
    };
    match catch_unwind(AssertUnwindSafe(|| body(session))) {
        Ok(value) => value,
        Err(payload) => fail(function, &crate::panic_message(payload.as_ref())),
    }
}

/// `std::span(ptr, size)` over caller memory; a null pointer with a zero length is an empty slice.
///
/// # Safety
/// A non-null `data` must point to `size` readable bytes, `size <= isize::MAX`.
unsafe fn bytes<'a>(data: *const u8, size: usize) -> &'a [u8] {
    if size == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(data, size) }
    }
}

/// # Safety
/// A non-null `data` must point to `size` writable bytes, `size <= isize::MAX`.
unsafe fn bytes_mut<'a>(data: *mut u8, size: usize) -> &'a mut [u8] {
    if size == 0 {
        &mut []
    } else {
        unsafe { std::slice::from_raw_parts_mut(data, size) }
    }
}

/// # Safety
/// A valid `data` must point to `max_frames * 2` writable `int16_t`.
unsafe fn stereo_out_mut<'a>(function: &str, data: *mut i16, max_frames: usize) -> &'a mut [i16] {
    if max_frames == 0 {
        return &mut [];
    }
    let samples = match max_frames.checked_mul(2) {
        Some(samples)
            if samples <= isize::MAX as usize / size_of::<i16>()
                && !data.is_null()
                && data.is_aligned() =>
        {
            samples
        }
        _ => fail(function, "invalid audio output buffer"),
    };
    // SAFETY: non-null, aligned, and the byte length fits `isize`; the caller vouches for the
    // writable samples, which is the C contract.
    unsafe { std::slice::from_raw_parts_mut(data, samples) }
}

/// Copies both ROMs, so the caller's buffers may go away after the call.
///
/// # Safety
/// `sound_rom` and `sample_rom` must point to `sound_rom_size` and `sample_rom_size` readable bytes
/// (`sample_rom` may be null for size 0). `shared_ram` must be null or satisfy
/// `RawSharedRam::from_raw`: 0x800 writable bytes that outlive the instance, as the header requires.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn f3rt_audio_init(
    sound_rom: *const u8,
    sound_rom_size: usize,
    sample_rom: *const u8,
    sample_rom_size: usize,
    shared_ram: *mut u8,
    driver_layout: u8,
) -> f3rt_audio_status {
    let Some(shared_ram) = (unsafe { RawSharedRam::from_raw(shared_ram) }) else {
        return F3RT_AUDIO_INVALID_ARGUMENT;
    };
    if sound_rom.is_null()
        || (sample_rom.is_null() && sample_rom_size > 0)
        || sound_rom_size > isize::MAX as usize
        || sample_rom_size > isize::MAX as usize
    {
        return F3RT_AUDIO_INVALID_ARGUMENT;
    }
    let mut instance = instance();
    if instance.is_some() {
        return F3RT_AUDIO_ALREADY_INITIALIZED;
    }
    let (sound, samples) = unsafe {
        (
            bytes(sound_rom, sound_rom_size),
            bytes(sample_rom, sample_rom_size),
        )
    };
    let Some(layout) = DriverLayout::select(driver_layout, sound) else {
        return F3RT_AUDIO_LAYOUT_MISMATCH;
    };
    let result = catch_unwind(AssertUnwindSafe(|| {
        let sound_rom = Arc::<[u8]>::from(sound);
        let words = audio::decode_sample_words(samples);
        Audio::try_new(sound_rom, words, layout).map(|audio| AbiSession { audio, shared_ram })
    }));
    match result {
        Ok(Ok(session)) => {
            *instance = Some(session);
            F3RT_AUDIO_OK
        }
        Ok(Err(_)) => F3RT_AUDIO_UNSUPPORTED_ROM,
        Err(payload) => fail("f3rt_audio_init", &crate::panic_message(payload.as_ref())),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_shutdown() {
    let session = { instance().take() };
    drop(session);
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_shared_write(offset: u32) {
    guarded("f3rt_audio_shared_write", |session| {
        session.with_ram_mut(|audio, ram| audio.shared_write(ram, offset));
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_set_reset(asserted: bool) {
    guarded("f3rt_audio_set_reset", |session| {
        session.with_ram(|audio, ram| audio.set_reset(ram, asserted));
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_set_sound_bank(page: u32) {
    guarded("f3rt_audio_set_sound_bank", |session| session.audio.set_sound_bank(page));
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_advance(ticks: u32) {
    guarded("f3rt_audio_advance", |session| session.audio.advance(ticks));
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_finish_frame() {
    guarded("f3rt_audio_finish_frame", |session| session.audio.finish_frame());
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_sample_rate() -> u32 {
    SAMPLE_RATE
}

/// # Safety
/// `interleaved_stereo` must point to at least `max_frames * 2` writable `int16_t`, as the header
/// requires.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn f3rt_audio_render(
    interleaved_stereo: *mut i16,
    max_frames: usize,
) -> usize {
    guarded("f3rt_audio_render", |session| {
        let out =
            unsafe { stereo_out_mut("f3rt_audio_render", interleaved_stereo, max_frames) };
        session.audio.render(out, max_frames)
    })
}

/// # Safety
/// `interleaved_stereo` must point to at least `max_frames * 2` writable `int16_t`, as the header
/// requires.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn f3rt_audio_render_ready(
    interleaved_stereo: *mut i16,
    max_frames: usize,
) -> usize {
    guarded("f3rt_audio_render_ready", |session| {
        let out =
            unsafe { stereo_out_mut("f3rt_audio_render_ready", interleaved_stereo, max_frames) };
        session.audio.render_ready(out, max_frames)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_performance_stats() -> f3rt_audio_stats {
    guarded("f3rt_audio_performance_stats", |session| {
        let stats = session.audio.performance_stats();
        f3rt_audio_stats {
            commands: stats.commands,
            rendered_frames: stats.rendered_frames,
            voices_started: stats.voices_started,
            render_ns: stats.render_ns,
            voices_active: stats.voices_active,
        }
    })
}

/// # Safety
/// `dst` must point to `F3RT_AUDIO_STATE_SIZE` writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn f3rt_audio_save_state(dst: *mut u8) {
    guarded("f3rt_audio_save_state", |session| {
        if dst.is_null() {
            fail("f3rt_audio_save_state", "null state buffer");
        }
        let out = unsafe { bytes_mut(dst, F3RT_AUDIO_STATE_SIZE as usize) };
        session.audio.save_state(out);
    });
}

/// # Safety
/// `src` must point to `F3RT_AUDIO_STATE_SIZE` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn f3rt_audio_load_state(src: *const u8) -> f3rt_audio_status {
    guarded("f3rt_audio_load_state", |session| {
        if src.is_null() {
            return F3RT_AUDIO_INVALID_STATE;
        }
        let input = unsafe { bytes(src, F3RT_AUDIO_STATE_SIZE as usize) };
        match session.audio.load_state(input) {
            Ok(()) => F3RT_AUDIO_OK,
            Err(_) => F3RT_AUDIO_INVALID_STATE,
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::hle::driver_layout::STANDARD;

    const SOUND_ROM_LEN: usize = 0x80000;

    fn ram() -> Vec<u8> {
        vec![0u8; SHARED_RAM_SIZE]
    }

    fn standard_rom() -> Vec<u8> {
        let mut rom = vec![0u8; SOUND_ROM_LEN];
        rom[4..8].copy_from_slice(&0x00c1089eu32.to_be_bytes());
        let at = STANDARD.esp_program_table_offset as usize;
        for (n, pointer) in [0xc1c836u32, 0xc1c4e2, 0xc1c4e2].into_iter().enumerate() {
            rom[at + 4 * n..at + 4 * n + 4].copy_from_slice(&pointer.to_be_bytes());
        }
        rom
    }

    #[test]
    fn init_rejects_null_and_oversized_arguments() {
        let rom = [0u8; 16];
        let mut shared = ram();
        let null = std::ptr::null();
        let cases = [
            (null, 16, rom.as_ptr(), 0, shared.as_mut_ptr()),
            (rom.as_ptr(), 16, rom.as_ptr(), 0, std::ptr::null_mut()),
            (rom.as_ptr(), 16, null, 4, shared.as_mut_ptr()),
            (rom.as_ptr(), isize::MAX as usize + 1, rom.as_ptr(), 0, shared.as_mut_ptr()),
            (rom.as_ptr(), 16, rom.as_ptr(), isize::MAX as usize + 1, shared.as_mut_ptr()),
        ];
        for (sound, sound_size, samples, sample_size, shared_ram) in cases {
            // SAFETY: every case is rejected before any pointer is read.
            let status =
                unsafe { f3rt_audio_init(sound, sound_size, samples, sample_size, shared_ram, 0) };
            assert_eq!(status, F3RT_AUDIO_INVALID_ARGUMENT);
        }
    }

    #[test]
    fn init_accepts_a_null_empty_sample_rom_and_reports_the_rom_problems() {
        let mut shared = ram();
        let blank = [0u8; 16];
        // SAFETY: valid pointers and sizes; the empty sample ROM may be null.
        let status = unsafe {
            f3rt_audio_init(blank.as_ptr(), 16, std::ptr::null(), 0, shared.as_mut_ptr(), 0)
        };
        assert_eq!(status, F3RT_AUDIO_LAYOUT_MISMATCH);
        let rom = standard_rom();
        let status = unsafe {
            f3rt_audio_init(rom.as_ptr(), rom.len(), std::ptr::null(), 0, shared.as_mut_ptr(), 0)
        };
        assert_eq!(status, F3RT_AUDIO_UNSUPPORTED_ROM);
    }

    #[test]
    fn shutdown_without_an_instance_is_a_no_op() {
        f3rt_audio_shutdown();
        f3rt_audio_shutdown();
    }
}
