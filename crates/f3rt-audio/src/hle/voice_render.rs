//! `audio/hle/voice_render.hpp`: one voice's sample read, gain ramp and four-pole filter over a span of
//! bus frames.

use super::block::{otis_target_bus, MutableAudioBlockSpan};
use super::synthesis::VOICE_BLOCK_FRAMES;
use super::voice::{
    advance_voice_position, interpolate_sample, interpolated_gain, ramp_gain, Filter, SampleAdvance,
    Voice,
};
use super::voice_filter::{filter_step, VoiceFilterState};
use super::voice_service::VoiceService;

pub const GAIN_SCALE: f32 = 1.0 / 524288.0;

/// The OTIS sample words, read as signed halves of their buffer: the C++ reinterprets the bank's
/// `uint16_t` words as `int16_t` and wraps reads with the power-of-two bank mask.
#[derive(Clone, Copy)]
pub struct SampleWords {
    words: *const i16,
    mask: usize,
    limit: f64,
}

impl SampleWords {
    pub fn new(words: &[u16]) -> SampleWords {
        SampleWords {
            words: words.as_ptr() as *const i16,
            // `words.size() - 1` in the C++ is SIZE_MAX for an empty sample ROM (this ROM set has
            // one); wrapping keeps the same mask instead of panicking in a debug build.
            mask: words.len().wrapping_sub(1),
            limit: words.len() as f64,
        }
    }

    #[inline(always)]
    pub fn interpolate(&self, position: f64) -> f32 {
        // SAFETY: the caller keeps `position` finite and inside the bank (`room`/`outside`), as the
        // C++ `size_t(position)` and `word[]` reads assume. The signed conversion is one instruction;
        // the saturating unsigned cast costs a dozen per sample.
        let i = unsafe { position.to_int_unchecked::<i64>() };
        let word = unsafe { self.words.add(i as usize) };
        unsafe { interpolate_sample(*word as f64, *word.add(1) as f64, position - i as f64) }
    }

    /// The `interpolate(const Voice &)` overload: the loop end wraps to zero outside the sample window.
    #[inline(always)]
    pub fn interpolate_voice(&self, voice: &Voice) -> f32 {
        // SAFETY: `render_frame` calls this after `outside()` rejected negative and out-of-bank positions.
        let signed = unsafe { voice.position.to_int_unchecked::<i64>() };
        let index = signed as usize;
        let fraction = voice.position - signed as f64;
        let first = if !voice.r#loop && voice.reverse && index < voice.start as usize {
            0.0
        } else {
            self.word(index)
        };
        let second = if !voice.r#loop && !voice.reverse && index + 1 >= voice.end as usize {
            0.0
        } else {
            self.word(index + 1)
        };
        interpolate_sample(first, second, fraction)
    }

    #[inline(always)]
    pub fn outside(&self, position: f64) -> bool {
        position < 0.0 || position >= self.limit
    }

    #[inline(always)]
    pub fn room(&self, voice: &Voice, cap: usize) -> usize {
        let mut left = -1.0;
        if !voice.reverse {
            if voice.position >= 0.0
                && voice.position < self.limit - 1.0
                && voice.end < self.limit - 1.0
                && voice.increment > 0.0
            {
                left = (voice.end - voice.position) / voice.increment;
            }
        } else if voice.position >= 0.0
            && voice.start >= 0.0
            && voice.position < self.limit - 1.0
            && voice.increment > 0.0
        {
            left = (voice.position - voice.start) / voice.increment;
        }
        if left < 2.0 {
            return 0;
        }
        if left < cap as f64 + 1.0 { left as usize - 1 } else { cap }
    }

    fn word(&self, at: usize) -> f64 {
        // SAFETY: the bank mask wraps every read into the words the bank owns.
        unsafe { *self.words.add(at & self.mask) as f64 }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum VoiceRunKind {
    Single,
    Constant,
    Ramping,
}

#[derive(Clone, Copy, Debug)]
pub struct VoiceRun {
    pub kind: VoiceRunKind,
    pub frames: usize,
}

#[inline(always)]
pub fn plan_run(voice: &Voice, rows: usize, samples: &SampleWords) -> VoiceRun {
    let ramping = voice.gain_frames > 0;
    let mut frames = 0;
    if !voice.sample_finished
        && voice.filter.ramp == 0
        && (!ramping
            || (voice.left_segment.frames > 0
                && voice.right_segment.frames > 0
                && voice.gain_frames > 1))
    {
        let mut cap = rows;
        if ramping {
            cap = if (voice.left_segment.frames as usize) < cap {
                voice.left_segment.frames as usize
            } else {
                cap
            };
            cap = if (voice.right_segment.frames as usize) < cap {
                voice.right_segment.frames as usize
            } else {
                cap
            };
            cap = if ((voice.gain_frames - 1) as usize) < cap {
                (voice.gain_frames - 1) as usize
            } else {
                cap
            };
        }
        frames = samples.room(voice, cap);
    }
    if frames < 1 {
        return VoiceRun { kind: VoiceRunKind::Single, frames: 1 };
    }
    VoiceRun {
        kind: if ramping { VoiceRunKind::Ramping } else { VoiceRunKind::Constant },
        frames,
    }
}

#[derive(Clone, Copy, Default, Debug)]
pub struct StereoGain {
    pub left: f32,
    pub right: f32,
}

impl StereoGain {
    #[inline(always)]
    /// # Safety
    /// `row` must point to two writable samples of a bus block row.
    pub unsafe fn mix(&self, row: *mut f32, y: f32) {
        // SAFETY: `row` is a chunk row, two contiguous samples of a bus block or of `discard`.
        unsafe {
            *row += self.left * y;
            let right = row.add(1);
            *right += self.right * y;
        }
    }
}

#[inline(always)]
pub fn current_gain(voice: &mut Voice) -> StereoGain {
    if voice.gain_dirty {
        voice.left_gain = interpolated_gain(voice.left);
        voice.right_gain = interpolated_gain(voice.right);
        voice.gain_dirty = false;
    }
    StereoGain { left: voice.left_gain * GAIN_SCALE, right: voice.right_gain * GAIN_SCALE }
}

#[inline(always)]
pub fn step_gain(voice: &mut Voice) {
    if voice.gain_frames == 0 {
        return;
    }
    voice.left += voice.step_left;
    voice.right += voice.step_right;
    voice.gain_frames -= 1;
    if voice.gain_frames == 0 {
        voice.left = voice.target_left;
        voice.right = voice.target_right;
        voice.gain_dirty = true;
    } else {
        voice.left_gain = ramp_gain(voice.left, voice.step_left, &mut voice.left_segment);
        voice.right_gain = ramp_gain(voice.right, voice.step_right, &mut voice.right_segment);
        voice.gain_dirty = false;
    }
}

/// `concept RunGain`: a constant or a ramping stereo gain for one run of frames.
pub trait RunGain {
    fn next(&mut self) -> StereoGain;
    fn store(&mut self, voice: &mut Voice, frames: usize);
}

pub struct ConstantGain {
    gain: StereoGain,
}

impl ConstantGain {
    #[inline(always)]
    pub fn new(voice: &mut Voice) -> ConstantGain {
        ConstantGain { gain: current_gain(voice) }
    }
}

impl RunGain for ConstantGain {
    #[inline(always)]
    fn next(&mut self) -> StereoGain {
        self.gain
    }

    #[inline(always)]
    fn store(&mut self, _voice: &mut Voice, _frames: usize) {}
}

pub struct RampingGain {
    left: f64,
    right: f64,
    left_value: f64,
    right_value: f64,
    left_slope: f64,
    right_slope: f64,
    step_left: f64,
    step_right: f64,
}

impl RampingGain {
    #[inline(always)]
    pub fn new(voice: &Voice) -> RampingGain {
        RampingGain {
            left: voice.left,
            right: voice.right,
            left_value: voice.left_segment.value,
            right_value: voice.right_segment.value,
            left_slope: voice.left_segment.slope,
            right_slope: voice.right_segment.slope,
            step_left: voice.step_left,
            step_right: voice.step_right,
        }
    }
}

impl RunGain for RampingGain {
    #[inline(always)]
    fn next(&mut self) -> StereoGain {
        self.left += self.step_left;
        self.right += self.step_right;
        self.left_value += self.left_slope;
        self.right_value += self.right_slope;
        StereoGain {
            left: self.left_value as f32 * GAIN_SCALE,
            right: self.right_value as f32 * GAIN_SCALE,
        }
    }

    #[inline(always)]
    fn store(&mut self, voice: &mut Voice, frames: usize) {
        voice.left = self.left;
        voice.right = self.right;
        voice.left_segment.frames -= frames as u32;
        voice.left_segment.value = self.left_value;
        voice.right_segment.frames -= frames as u32;
        voice.right_segment.value = self.right_value;
        voice.gain_frames -= frames as u32;
        voice.left_gain = self.left_value as f32;
        voice.right_gain = self.right_value as f32;
        voice.gain_dirty = false;
    }
}

pub struct VoiceFilter<const MODE: u32> {
    pub state: VoiceFilterState,
    pub coef1: f32,
    pub coef2: f32,
    pub pole: f32,
}

impl<const MODE: u32> VoiceFilter<MODE> {
    #[inline(always)]
    pub fn new(memory: &VoiceFilterState, filter: &Filter) -> VoiceFilter<MODE> {
        VoiceFilter {
            state: *memory,
            coef1: filter.a1 as f32,
            coef2: filter.a2 as f32,
            pole: filter.high_pole as f32,
        }
    }

    #[inline(always)]
    pub fn step(&mut self, x: f32) -> f32 {
        filter_step::<MODE>(&mut self.state, x, self.coef1, self.coef2, self.pole)
    }
}

#[derive(Clone, Copy)]
pub struct Chunk {
    pub rows: *mut f32,
    pub first_frame: usize,
    pub frames: usize,
}

impl Chunk {
    #[inline(always)]
    pub fn row(&self, r: usize) -> *mut f32 {
        // SAFETY: `r` names a frame the chunk covers; a row is eight interleaved samples.
        unsafe { self.rows.add(r * 8) }
    }

    #[inline(always)]
    pub fn frame(&self, r: usize) -> usize {
        self.first_frame + r
    }
}

pub struct VoiceBus {
    bus: *mut f32,
    discard: *mut f32,
}

impl VoiceBus {
    /// `discard` is the render state's scratch block; the C++ span's extent is `VOICE_BLOCK_FRAMES * 8`.
    pub fn new(mut buses: MutableAudioBlockSpan<8>, pair: u8, discard: *mut f32) -> VoiceBus {
        let mut bus: *mut f32 = std::ptr::null_mut();
        if let Some(select) = otis_target_bus(pair) {
            let channel_left = buses.slice(select).channel_left();
            // SAFETY: the left channel of the selected bus pair is inside the block span.
            bus = unsafe { buses.data().add(channel_left) };
        }
        VoiceBus { bus, discard }
    }

    #[inline(always)]
    pub fn chunk(&self, first_frame: usize, frames: usize) -> Chunk {
        if !self.bus.is_null() {
            // SAFETY: the block span covers the frames the render walks.
            return Chunk {
                rows: unsafe { self.bus.add(first_frame * 8) },
                first_frame,
                frames,
            };
        }
        // std::fill_n(m_discard.data(), frames * 8, 0.f)
        for i in 0..frames * 8 {
            // SAFETY: `discard` holds VOICE_BLOCK_FRAMES * 8 floats and frames <= VOICE_BLOCK_FRAMES.
            unsafe { *self.discard.add(i) = 0.0 };
        }
        Chunk { rows: self.discard, first_frame, frames }
    }
}

pub struct VoiceSpan<'a, const MODE: u32> {
    voice: &'a mut Voice,
    samples: SampleWords,
    filter: VoiceFilterState,
}

impl<'a, const MODE: u32> VoiceSpan<'a, MODE> {
    pub fn new(
        voice: &'a mut Voice,
        samples: SampleWords,
        memory: &VoiceFilterState,
    ) -> VoiceSpan<'a, MODE> {
        VoiceSpan { voice, samples, filter: *memory }
    }

    #[inline(never)]
    pub fn render(
        mut self,
        bus: &VoiceBus,
        voices: &mut VoiceService,
        frame_ticks: &[u64],
        frame_start: usize,
        frame_end: usize,
    ) -> VoiceFilterState {
        let mut first = frame_start;
        while first < frame_end {
            let count = if VOICE_BLOCK_FRAMES < frame_end - first {
                VOICE_BLOCK_FRAMES
            } else {
                frame_end - first
            };
            let chunk = bus.chunk(first, count);
            let mut row = 0;
            while row < chunk.frames {
                let run = plan_run(&*self.voice, chunk.frames - row, &self.samples);
                match run.kind {
                    VoiceRunKind::Single => {
                        row += self.render_frame(&chunk, row, voices, frame_ticks);
                    }
                    VoiceRunKind::Constant => {
                        let gain = ConstantGain::new(&mut *self.voice);
                        row += self.render_run(&chunk, row, run.frames, gain, voices);
                    }
                    VoiceRunKind::Ramping => {
                        let gain = RampingGain::new(&*self.voice);
                        row += self.render_run(&chunk, row, run.frames, gain, voices);
                    }
                }
            }
            first += VOICE_BLOCK_FRAMES;
        }
        self.filter
    }

    #[inline(always)]
    fn render_run<G: RunGain>(
        &mut self,
        chunk: &Chunk,
        row: usize,
        frames: usize,
        mut gain: G,
        voices: &mut VoiceService,
    ) -> usize {
        let step = if self.voice.reverse { -self.voice.increment } else { self.voice.increment };
        let samples = self.samples;
        let mut filter = VoiceFilter::<MODE>::new(&self.filter, &self.voice.filter);
        let mut out = chunk.row(row);
        let mut position = self.voice.position;
        let mut input = samples.interpolate(position);
        position += step;
        let mut k = 1;
        while k < frames {
            let next = samples.interpolate(position);
            position += step;
            unsafe { gain.next().mix(out, filter.step(input)) };
            input = next;
            // SAFETY: `row + k` stays inside the chunk for k < frames.
            out = unsafe { out.add(8) };
            k += 1;
        }
        unsafe { gain.next().mix(out, filter.step(input)) };
        self.filter = filter.state;
        self.voice.position = position;
        gain.store(&mut *self.voice, frames);
        voices.set_render_frame(chunk.frame(row + frames - 1));
        frames
    }

    #[inline(always)]
    fn render_frame(
        &mut self,
        chunk: &Chunk,
        row: usize,
        voices: &mut VoiceService,
        frame_ticks: &[u64],
    ) -> usize {
        let frame = chunk.frame(row);
        voices.set_render_frame(frame);
        step_gain(&mut *self.voice);
        if self.voice.sample_finished {
            self.emit(chunk.row(row), 0.0, StereoGain::default());
            return 1;
        }
        if self.samples.outside(self.voice.position) {
            self.voice.active = false;
            self.silence(chunk, row);
            return chunk.frames - row;
        }
        let input = self.samples.interpolate_voice(&*self.voice);
        self.voice.filter.advance();
        let gain = current_gain(&mut *self.voice);
        let ended = matches!(
            advance_voice_position(
                &mut self.voice.position,
                &mut self.voice.reverse,
                self.voice.increment,
                self.voice.start,
                self.voice.end,
                self.voice.end - self.voice.start,
                self.voice.r#loop,
                self.voice.bidirectional,
            ),
            SampleAdvance::Ended
        );
        self.emit(chunk.row(row), input, gain);
        if !ended {
            return 1;
        }
        voices.end_sample(&mut *self.voice, frame_ticks[frame]);
        self.silence(chunk, row + 1);
        chunk.frames - row
    }

    #[inline(always)]
    fn emit(&mut self, row: *mut f32, input: f32, gain: StereoGain) {
        let mut filter = VoiceFilter::<MODE>::new(&self.filter, &self.voice.filter);
        unsafe { gain.mix(row, filter.step(input)) };
        self.filter = filter.state;
    }

    #[inline(always)]
    fn silence(&mut self, chunk: &Chunk, from_row: usize) {
        let mut filter = VoiceFilter::<MODE>::new(&self.filter, &self.voice.filter);
        let mut row = from_row;
        while row < chunk.frames {
            unsafe { StereoGain::default().mix(chunk.row(row), filter.step(0.0)) };
            row += 1;
        }
        self.filter = filter.state;
    }
}
