//! `include/f3rt/abi/audio.h` over `crate::audio`: one process-wide instance, panics turned into
//! status codes (`init`, `load_state`) or an abort with the message the C++ ABI shim prints.
//!
//! The C++ shim caught `std::exception` and aborted with `what()`; the Rust counterpart catches the
//! panic payload and aborts with the same text, so a failure prints the same line.

#![allow(non_camel_case_types)] // The names are `include/f3rt/abi/audio.h`'s.

use std::panic::{AssertUnwindSafe, catch_unwind};

use crate::audio::{self, Audio};

pub type f3rt_audio_status = i32;
pub const F3RT_AUDIO_OK: f3rt_audio_status = 0;
pub const F3RT_AUDIO_UNSUPPORTED_ROM: f3rt_audio_status = 1;
pub const F3RT_AUDIO_ALREADY_INITIALIZED: f3rt_audio_status = 2;
pub const F3RT_AUDIO_INVALID_STATE: f3rt_audio_status = 3;

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

struct Global(std::cell::UnsafeCell<Option<Audio>>);

// SAFETY: the header requires every call to come from one thread, exactly like the C++
// `std::unique_ptr` instance this replaces.
unsafe impl Sync for Global {}

static INSTANCE: Global = Global(std::cell::UnsafeCell::new(None));

fn instance() -> &'static mut Option<Audio> {
    // SAFETY: single-owner-thread contract of the header.
    unsafe { &mut *INSTANCE.0.get() }
}

#[inline(never)]
fn fail(function: &str, what: &str) -> ! {
    eprintln!("f3rt: {function}: {what}");
    std::process::abort()
}

fn audio(function: &str) -> &'static mut Audio {
    match instance().as_mut() {
        Some(audio) => audio,
        None => fail(function, "audio is not initialized"),
    }
}

fn guarded<R, F: FnOnce(&mut Audio) -> R>(function: &str, body: F) -> R {
    match catch_unwind(AssertUnwindSafe(|| body(audio(function)))) {
        Ok(value) => value,
        Err(payload) => fail(function, &crate::panic_message(payload.as_ref())),
    }
}

/// `std::span(ptr, size)` over caller memory; a null pointer with a zero length is an empty slice.
unsafe fn bytes<'a>(data: *const u8, size: usize) -> &'a [u8] {
    if size == 0 { &[] } else { unsafe { std::slice::from_raw_parts(data, size) } }
}

unsafe fn bytes_mut<'a>(data: *mut u8, size: usize) -> &'a mut [u8] {
    if size == 0 { &mut [] } else { unsafe { std::slice::from_raw_parts_mut(data, size) } }
}

/// Calls that must not unwind into C++ run inside `catch_unwind`, which needs the closure to be
/// unwind safe: the state is process-wide and single-threaded, so this only asserts what the header
/// already requires.
/// # Safety
/// `sound_rom` and `sample_rom` must point to their stated lengths for the duration of the call;
/// `shared_ram` must point to the 0x800-byte dual-port RAM, which must outlive the instance.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn f3rt_audio_init(
    sound_rom: *const u8,
    sound_rom_size: usize,
    sample_rom: *const u8,
    sample_rom_size: usize,
    shared_ram: *mut u8,
) -> f3rt_audio_status {
    if instance().is_some() {
        return F3RT_AUDIO_ALREADY_INITIALIZED;
    }
    // SAFETY: the caller hands over both ROM buffers for the duration of the call.
    let (sound, samples) = unsafe {
        (bytes(sound_rom, sound_rom_size), bytes(sample_rom, sample_rom_size))
    };
    let result = catch_unwind(AssertUnwindSafe(|| {
        let words = audio::decode_sample_words(samples);
        if !Audio::supported(sound, &words) {
            return Some(F3RT_AUDIO_UNSUPPORTED_ROM);
        }
        *instance() = Some(Audio::new(sound.to_vec(), words, shared_ram));
        None
    }));
    match result {
        Ok(Some(status)) => status,
        Ok(None) => F3RT_AUDIO_OK,
        Err(payload) => fail("f3rt_audio_init", &crate::panic_message(payload.as_ref())),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_shutdown() {
    *instance() = None;
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_shared_write(offset: u32) {
    guarded("f3rt_audio_shared_write", |a| a.shared_write(offset));
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_set_reset(asserted: bool) {
    guarded("f3rt_audio_set_reset", |a| a.set_reset(asserted));
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_set_sound_bank(page: u32) {
    guarded("f3rt_audio_set_sound_bank", |a| a.set_sound_bank(page));
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_advance(main_cycles: u32) {
    guarded("f3rt_audio_advance", |a| a.advance(main_cycles));
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_finish_frame() {
    guarded("f3rt_audio_finish_frame", |a| a.finish_frame());
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_sample_rate() -> u32 {
    guarded("f3rt_audio_sample_rate", |a| a.sample_rate())
}

/// # Safety
/// `interleaved_stereo` must point to at least `max_frames * 2` writable `int16_t`, as the header
/// requires.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn f3rt_audio_render(
    interleaved_stereo: *mut i16,
    max_frames: usize,
) -> usize {
    guarded("f3rt_audio_render", |a| {
        // SAFETY: the caller vouches for 2 * max_frames writable samples, which is the C contract.
        let out = unsafe { std::slice::from_raw_parts_mut(interleaved_stereo, max_frames * 2) };
        a.render(out, max_frames)
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
    guarded("f3rt_audio_render_ready", |a| {
        // SAFETY: the caller vouches for 2 * max_frames writable samples, which is the C contract.
        let out = unsafe { std::slice::from_raw_parts_mut(interleaved_stereo, max_frames * 2) };
        a.render_ready(out, max_frames)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn f3rt_audio_performance_stats() -> f3rt_audio_stats {
    guarded("f3rt_audio_performance_stats", |a| {
        let stats = a.performance_stats();
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
    guarded("f3rt_audio_save_state", |a| {
        // SAFETY: the caller vouches for F3RT_AUDIO_STATE_SIZE writable bytes.
        let out = unsafe { bytes_mut(dst, F3RT_AUDIO_STATE_SIZE as usize) };
        a.save_state(out);
    });
}

/// # Safety
/// `src` must point to `F3RT_AUDIO_STATE_SIZE` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn f3rt_audio_load_state(src: *const u8) -> f3rt_audio_status {
    guarded("f3rt_audio_load_state", |a| {
        // SAFETY: the caller vouches for F3RT_AUDIO_STATE_SIZE readable bytes.
        let input = unsafe { bytes(src, F3RT_AUDIO_STATE_SIZE as usize) };
        match a.load_state(input) {
            Ok(()) => F3RT_AUDIO_OK,
            Err(_) => F3RT_AUDIO_INVALID_STATE,
        }
    })
}
