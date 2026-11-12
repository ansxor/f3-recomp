//! `audio/hle/synthesis.hpp` + `synthesis.cpp`: the render-time voice state and the per-voice render
//! entry points.
//!
//! `with_filter_mode` has no Rust equivalent: the per-mode body is the `render_span` helper, and the
//! register mode picks its monomorphisation.

use std::sync::Arc;

use super::block::MutableAudioBlockSpan;
use super::sound_bank::SoundBank;
use super::voice::Voice;
use super::voice_filter::{self, VoiceFilterState};
use super::voice_render::{SampleWords, VoiceBus, VoiceSpan};
use super::voice_service::VoiceService;

pub const VOICE_BLOCK_FRAMES: usize = 64;

pub struct VoiceRenderState {
    pub filters: [VoiceFilterState; 32],
    pub filter_epoch: [u32; 32],
    pub frame_ticks: Vec<u64>,
    /// Scratch rows for voices with no OTIS bus pair. The C++ `alignas(64)` is a cache-line hint only.
    pub discard: [f32; VOICE_BLOCK_FRAMES * 8],
    pub tick: u64,
    pub tick_remainder: u32,
}

impl Default for VoiceRenderState {
    fn default() -> VoiceRenderState {
        VoiceRenderState::new()
    }
}

impl VoiceRenderState {
    pub fn new() -> VoiceRenderState {
        VoiceRenderState {
            filters: std::array::from_fn(|_| VoiceFilterState::default()),
            filter_epoch: [0; 32],
            frame_ticks: Vec::with_capacity(1024),
            discard: [0.0; VOICE_BLOCK_FRAMES * 8],
            tick: 0,
            tick_remainder: 0,
        }
    }
}

/// Restarts the frame -> tick map. The filter memory of the slots stays.
pub fn reset_render_state(state: &mut VoiceRenderState, tick: u64) {
    state.tick = tick;
    state.tick_remainder = 0;
}

/// One monomorphisation of the `with_filter_mode` body: renders the span with `Mode`'s filter.
#[allow(clippy::too_many_arguments)]
fn render_span<const MODE: u32>(
    voice: &mut Voice,
    samples: SampleWords,
    memory: &VoiceFilterState,
    bus: &VoiceBus,
    voices: &mut VoiceService,
    frame_start: usize,
    frame_end: usize,
) -> VoiceFilterState {
    VoiceSpan::<MODE>::new(voice, samples, memory).render(bus, voices, frame_start, frame_end)
}

/// Renders one voice into its OTIS bus pair over [frame_start, frame_end) of `buses`.
pub fn render_voice(
    voices: &mut VoiceService,
    voice: &mut Voice,
    bank: Arc<SoundBank>,
    state: &mut VoiceRenderState,
    buses: MutableAudioBlockSpan<8>,
    frame_start: usize,
    frame_end: usize,
) {
    // filter_memory(): the slot's filter memory is dropped when the voice's filter epoch moved.
    let slot = voice.slot as usize;
    if state.filter_epoch[slot] != voice.filter_epoch {
        state.filters[slot].clear();
        state.filter_epoch[slot] = voice.filter_epoch;
    }
    let discard: *mut f32 = state.discard.as_mut_ptr();
    let memory: &mut VoiceFilterState = &mut state.filters[slot];
    let bus = VoiceBus::new(buses, voice.pair, discard);
    let samples = SampleWords::new(bank.samples);
    let next = match voice_filter::filter_mode(voice.filter.mode) {
        0 => render_span::<0>(voice, samples, memory, &bus, voices, frame_start, frame_end),
        1 => render_span::<1>(voice, samples, memory, &bus, voices, frame_start, frame_end),
        2 => render_span::<2>(voice, samples, memory, &bus, voices, frame_start, frame_end),
        _ => render_span::<3>(voice, samples, memory, &bus, voices, frame_start, frame_end),
    };
    *memory = next;
}

/// Renders every active voice of the pool, in slot order.
pub fn render_voices(
    voices: &mut VoiceService,
    bank: Arc<SoundBank>,
    state: &mut VoiceRenderState,
    buses: MutableAudioBlockSpan<8>,
    frame_start: usize,
    frame_end: usize,
) {
    // SAFETY: the pool is a fixed array owned by the service; the C++ iterates the same slots while
    // handing the service itself, and the voice it holds, to each render.
    let pool: *mut Voice = &mut voices.pool()[0];
    for slot in 0..32 {
        let voice: &mut Voice = unsafe { &mut *pool.add(slot) };
        if !voice.active {
            continue;
        }
        render_voice(
            voices,
            voice,
            Arc::clone(&bank),
            state,
            buses,
            frame_start,
            frame_end,
        );
    }
}
