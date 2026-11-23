use super::block::OtisBus;
use super::synthesis::VOICE_BLOCK_FRAMES;
use super::voice::{Filter, GainSegment, SampleAdvance, Voice, interpolate_sample, interpolated_gain};
use super::voice_filter::VoiceFilterState;

pub const GAIN_SCALE: f32 = 1.0 / 524288.0;

/// The OTIS sample words, read as signed halves of their buffer and wrapped with the power-of-two
/// bank mask.
#[derive(Clone, Copy)]
pub struct SampleWords<'a> {
    words: &'a [u16],
    mask: usize,
    limit: f64,
}

impl<'a> SampleWords<'a> {
    pub fn new(words: &'a [u16]) -> SampleWords<'a> {
        SampleWords {
            words,
            // An empty sample ROM wraps the mask to usize::MAX instead of panicking in a debug build;
            // such a voice is always `outside` and never reads.
            mask: words.len().wrapping_sub(1),
            limit: words.len() as f64,
        }
    }

    fn signed(&self, index: usize) -> f64 {
        f64::from(self.words[index] as i16)
    }

    #[inline(always)]
    pub fn interpolate(&self, position: f64) -> f32 {
        let index = word_index(position);
        interpolate_sample(
            self.signed(index),
            self.signed(index + 1),
            position - index as f64,
        )
    }

    #[inline(always)]
    pub fn interpolate_voice(&self, voice: &Voice) -> f32 {
        let index = word_index(voice.position);
        let fraction = voice.position - index as f64;
        let first = if !voice.r#loop && voice.reverse && index < voice.start as usize {
            0.0
        } else {
            self.signed(index & self.mask)
        };
        let second = if !voice.r#loop && !voice.reverse && index + 1 >= voice.end as usize {
            0.0
        } else {
            self.signed((index + 1) & self.mask)
        };
        interpolate_sample(first, second, fraction)
    }

    #[inline(always)]
    pub fn outside(&self, position: f64) -> bool {
        !(0.0..self.limit).contains(&position)
    }

    /// How many frames, up to `cap`, the voice can read without reaching a window edge or the end of
    /// the bank.
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
        if left < cap as f64 + 1.0 {
            left as usize - 1
        } else {
            cap
        }
    }
}

#[inline(always)]
fn word_index(position: f64) -> usize {
    position as usize
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

#[derive(Clone, Copy, Default, Debug)]
pub struct StereoGain {
    pub left: f32,
    pub right: f32,
}

impl Voice {
    /// How the next `rows` frames render: one at a time while anything is changing, or as a run with
    /// a constant or a linearly ramping gain.
    #[inline(always)]
    pub fn plan_run(&self, rows: usize, samples: &SampleWords) -> VoiceRun {
        let ramping = self.gain_frames > 0;
        let mut frames = 0;
        if !self.sample_finished
            && self.filter.ramp == 0
            && (!ramping
                || (self.left_segment.frames > 0
                    && self.right_segment.frames > 0
                    && self.gain_frames > 1))
        {
            let mut cap = rows;
            if ramping {
                cap = cap.min(self.left_segment.frames as usize);
                cap = cap.min(self.right_segment.frames as usize);
                cap = cap.min((self.gain_frames - 1) as usize);
            }
            frames = samples.room(self, cap);
        }
        if frames < 1 {
            return VoiceRun {
                kind: VoiceRunKind::Single,
                frames: 1,
            };
        }
        VoiceRun {
            kind: if ramping {
                VoiceRunKind::Ramping
            } else {
                VoiceRunKind::Constant
            },
            frames,
        }
    }

    #[inline(always)]
    pub fn current_gain(&mut self) -> StereoGain {
        if self.gain_dirty {
            self.left_gain = interpolated_gain(self.left);
            self.right_gain = interpolated_gain(self.right);
            self.gain_dirty = false;
        }
        StereoGain {
            left: self.left_gain * GAIN_SCALE,
            right: self.right_gain * GAIN_SCALE,
        }
    }

    #[inline(always)]
    pub fn step_gain(&mut self) {
        if self.gain_frames == 0 {
            return;
        }
        self.left += self.step_left;
        self.right += self.step_right;
        self.gain_frames -= 1;
        if self.gain_frames == 0 {
            self.left = self.target_left;
            self.right = self.target_right;
            self.gain_dirty = true;
        } else {
            self.left_gain = self.left_segment.step(self.left, self.step_left);
            self.right_gain = self.right_segment.step(self.right, self.step_right);
            self.gain_dirty = false;
        }
    }
}

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
        ConstantGain {
            gain: voice.current_gain(),
        }
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
        let GainSegment {
            value: left_value,
            slope: left_slope,
            ..
        } = voice.left_segment;
        let GainSegment {
            value: right_value,
            slope: right_slope,
            ..
        } = voice.right_segment;
        RampingGain {
            left: voice.left,
            right: voice.right,
            left_value,
            right_value,
            left_slope,
            right_slope,
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
        self.state.step::<MODE>(x, self.coef1, self.coef2, self.pole)
    }
}

fn bus_left_channel(pair: u8) -> Option<usize> {
    OtisBus::pair_left(pair)
}

pub struct Chunk<'a> {
    rows: &'a mut [[f32; 8]],
    left: usize,
}

impl Chunk<'_> {
    pub fn frames(&self) -> usize {
        self.rows.len()
    }

    #[inline(always)]
    fn mix(&mut self, row: usize, gain: StereoGain, y: f32) {
        let frame = &mut self.rows[row];
        frame[self.left] += gain.left * y;
        frame[self.left + 1] += gain.right * y;
    }
}

pub struct VoiceBus<'a> {
    buses: &'a mut [f32],
    discard: &'a mut [[f32; 8]; VOICE_BLOCK_FRAMES],
    left: Option<usize>,
}

impl<'a> VoiceBus<'a> {
    pub fn new(
        buses: &'a mut [f32],
        discard: &'a mut [[f32; 8]; VOICE_BLOCK_FRAMES],
        pair: u8,
    ) -> VoiceBus<'a> {
        VoiceBus {
            buses,
            discard,
            left: bus_left_channel(pair),
        }
    }

    #[inline(always)]
    pub fn chunk(&mut self, first_frame: usize, frames: usize) -> Chunk<'_> {
        match self.left {
            Some(left) => Chunk {
                rows: self.buses[first_frame * 8..(first_frame + frames) * 8]
                    .as_chunks_mut::<8>()
                    .0,
                left,
            },
            None => {
                let rows = &mut self.discard[..frames];
                rows.fill([0.0; 8]);
                Chunk { rows, left: 0 }
            }
        }
    }
}

pub struct VoiceSpan<'a, const MODE: u32> {
    voice: &'a mut Voice,
    samples: SampleWords<'a>,
    filter: VoiceFilterState,
}

impl<'a, const MODE: u32> VoiceSpan<'a, MODE> {
    pub fn new(
        voice: &'a mut Voice,
        samples: SampleWords<'a>,
        memory: &VoiceFilterState,
    ) -> VoiceSpan<'a, MODE> {
        VoiceSpan {
            voice,
            samples,
            filter: *memory,
        }
    }

    #[inline(never)]
    pub fn render(
        mut self,
        bus: &mut VoiceBus,
        frame_start: usize,
        frame_end: usize,
    ) -> VoiceFilterState {
        let mut first = frame_start;
        while first < frame_end {
            let count = VOICE_BLOCK_FRAMES.min(frame_end - first);
            let mut chunk = bus.chunk(first, count);
            let mut row = 0;
            while row < chunk.frames() {
                let run = self.voice.plan_run(chunk.frames() - row, &self.samples);
                match run.kind {
                    VoiceRunKind::Single => {
                        row += self.render_frame(&mut chunk, row);
                    }
                    VoiceRunKind::Constant => {
                        let gain = ConstantGain::new(self.voice);
                        row += self.render_run(&mut chunk, row, run.frames, gain);
                    }
                    VoiceRunKind::Ramping => {
                        let gain = RampingGain::new(self.voice);
                        row += self.render_run(&mut chunk, row, run.frames, gain);
                    }
                }
            }
            first += VOICE_BLOCK_FRAMES;
        }
        self.filter.flush_subnormals();
        self.filter
    }

    #[inline(always)]
    fn render_run<G: RunGain>(
        &mut self,
        chunk: &mut Chunk,
        row: usize,
        frames: usize,
        mut gain: G,
    ) -> usize {
        let step = if self.voice.reverse {
            -self.voice.increment
        } else {
            self.voice.increment
        };
        let samples = self.samples;
        let mut filter = VoiceFilter::<MODE>::new(&self.filter, &self.voice.filter);
        let mut position = self.voice.position;
        let mut input = samples.interpolate(position);
        position += step;
        for out in row..row + frames - 1 {
            let next = samples.interpolate(position);
            position += step;
            chunk.mix(out, gain.next(), filter.step(input));
            input = next;
        }
        chunk.mix(row + frames - 1, gain.next(), filter.step(input));
        self.filter = filter.state;
        self.voice.position = position;
        gain.store(self.voice, frames);
        frames
    }

    #[inline(always)]
    fn render_frame(&mut self, chunk: &mut Chunk, row: usize) -> usize {
        self.voice.step_gain();
        if self.voice.sample_finished {
            self.emit(chunk, row, 0.0, StereoGain::default());
            return 1;
        }
        if self.samples.outside(self.voice.position) {
            self.voice.active = false;
            self.silence(chunk, row);
            return chunk.frames() - row;
        }
        let input = self.samples.interpolate_voice(self.voice);
        self.voice.filter.advance();
        let gain = self.voice.current_gain();
        let ended = self.voice.advance_position() == SampleAdvance::Ended;
        self.emit(chunk, row, input, gain);
        if !ended {
            return 1;
        }
        self.voice.end_sample();
        self.silence(chunk, row + 1);
        chunk.frames() - row
    }

    #[inline(always)]
    fn emit(&mut self, chunk: &mut Chunk, row: usize, input: f32, gain: StereoGain) {
        let mut filter = VoiceFilter::<MODE>::new(&self.filter, &self.voice.filter);
        chunk.mix(row, gain, filter.step(input));
        self.filter = filter.state;
    }

    #[inline(always)]
    fn silence(&mut self, chunk: &mut Chunk, from_row: usize) {
        let mut filter = VoiceFilter::<MODE>::new(&self.filter, &self.voice.filter);
        for row in from_row..chunk.frames() {
            chunk.mix(row, StereoGain::default(), filter.step(0.0));
        }
        self.filter = filter.state;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn outside_rejects_everything_but_the_bank() {
        let words = [0u16; 8];
        let samples = SampleWords::new(&words);
        for position in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY, -0.001, 8.0, 9.5] {
            assert!(samples.outside(position), "{position}");
        }
        for position in [0.0, 0.5, 7.999] {
            assert!(!samples.outside(position), "{position}");
        }
    }

    #[test]
    fn an_empty_bank_is_always_outside() {
        let samples = SampleWords::new(&[]);
        assert!(samples.outside(0.0));
    }

    #[test]
    fn word_index_truncates_and_saturates() {
        assert_eq!(word_index(0.0), 0);
        assert_eq!(word_index(3.99), 3);
        assert_eq!(word_index(f64::NAN), 0);
        assert_eq!(word_index(-5.0), 0);
        assert_eq!(word_index(f64::INFINITY), usize::MAX);
        assert_eq!(word_index(1e300), usize::MAX);
    }
}
