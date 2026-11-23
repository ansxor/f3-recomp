use super::block::BusBlock;
use super::events::{MAIN_CLOCK, SAMPLE_RATE};
use super::voice::Voice;
use super::voice_filter::{self, VoiceFilterState};
use super::voice_render::{SampleWords, VoiceBus, VoiceSpan};

pub const VOICE_BLOCK_FRAMES: usize = 64;

pub struct VoiceRenderState {
    pub filters: [VoiceFilterState; 32],
    pub filter_epoch: [u32; 32],
    pub frame_ticks: Vec<u64>,
    pub discard: [[f32; 8]; VOICE_BLOCK_FRAMES],
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
            discard: [[0.0; 8]; VOICE_BLOCK_FRAMES],
            tick: 0,
            tick_remainder: 0,
        }
    }

    /// Restarts the frame -> tick map. The filter memory of the slots stays.
    pub fn reset(&mut self, tick: u64) {
        self.tick = tick;
        self.tick_remainder = 0;
    }

    /// Maps the next `frames` output frames to the main-clock tick each starts at, and advances the
    /// map past them.
    pub fn fill_frame_ticks(&mut self, frames: usize) {
        if self.frame_ticks.len() < frames {
            self.frame_ticks.resize(frames, 0);
        }
        for slot in &mut self.frame_ticks[..frames] {
            *slot = self.tick;
            self.tick_remainder += MAIN_CLOCK;
            self.tick += u64::from(self.tick_remainder / SAMPLE_RATE);
            self.tick_remainder %= SAMPLE_RATE;
        }
    }

    /// Renders every active voice, in slot order, into the bus rows `frame_start..frame_end`.
    pub fn render_voices(
        &mut self,
        voices: &mut [Voice; 32],
        samples: &[u16],
        buses: &mut BusBlock,
        frame_start: usize,
        frame_end: usize,
    ) {
        for voice in voices.iter_mut().filter(|voice| voice.active) {
            let slot = usize::from(voice.slot);
            if self.filter_epoch[slot] != voice.filter_epoch {
                self.filters[slot].clear();
                self.filter_epoch[slot] = voice.filter_epoch;
            }
            let mut bus = VoiceBus::new(&mut buses.samples, &mut self.discard, voice.pair);
            let samples = SampleWords::new(samples);
            let memory = &mut self.filters[slot];
            *memory = match voice_filter::filter_mode(voice.filter.mode) {
                0 => render_span::<0>(voice, samples, memory, &mut bus, frame_start, frame_end),
                1 => render_span::<1>(voice, samples, memory, &mut bus, frame_start, frame_end),
                2 => render_span::<2>(voice, samples, memory, &mut bus, frame_start, frame_end),
                _ => render_span::<3>(voice, samples, memory, &mut bus, frame_start, frame_end),
            };
        }
    }
}

fn render_span<const MODE: u32>(
    voice: &mut Voice,
    samples: SampleWords,
    memory: &VoiceFilterState,
    bus: &mut VoiceBus,
    frame_start: usize,
    frame_end: usize,
) -> VoiceFilterState {
    VoiceSpan::<MODE>::new(voice, samples, memory).render(bus, frame_start, frame_end)
}
