pub const BLOCK_FRAMES: usize = 256;

/// Physical PCB wiring from OTIS (ES5505) to ESP (ES5510) and DAC.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(usize)]
pub enum OtisBus {
    NcL = 0,
    NcR = 1,
    DryL = 2,
    DryR = 3,
    DelayL = 4,
    DelayR = 5,
    ReverbL = 6,
    ReverbR = 7,
}

impl OtisBus {
    pub fn index(self) -> usize {
        self as usize
    }

    /// Left channel of the bus pair selected by a voice's `pair` register, or `None` when muted.
    pub fn pair_left(pair: u8) -> Option<usize> {
        match pair {
            1 => Some(OtisBus::DryL.index()),
            2 => Some(OtisBus::DelayL.index()),
            3 => Some(OtisBus::ReverbL.index()),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(usize)]
pub enum StereoChannel {
    Left = 0,
    Right = 1,
}

impl StereoChannel {
    pub fn index(self) -> usize {
        self as usize
    }
}

/// 64-byte aligned storage owning interleaved frames.
#[repr(align(64))]
pub struct AudioBlock<const CHANNELS: usize, const MAX_FRAMES: usize, const SAMPLES: usize> {
    pub samples: [f32; SAMPLES],
    pub active_frames: usize,
}

impl<const CHANNELS: usize, const MAX_FRAMES: usize, const SAMPLES: usize>
    AudioBlock<CHANNELS, MAX_FRAMES, SAMPLES>
{
    pub const fn new() -> AudioBlock<CHANNELS, MAX_FRAMES, SAMPLES> {
        AudioBlock {
            samples: [0.0; SAMPLES],
            active_frames: 0,
        }
    }

    pub fn at(&self, frame: usize, channel: usize) -> f32 {
        self.samples[frame * CHANNELS + channel]
    }

    pub fn at_bus(&self, frame: usize, bus: OtisBus) -> f32 {
        self.at(frame, bus.index())
    }

    pub fn set(&mut self, frame: usize, channel: usize, value: f32) {
        self.samples[frame * CHANNELS + channel] = value;
    }

    pub fn set_channel(&mut self, frame: usize, channel: StereoChannel, value: f32) {
        self.set(frame, channel.index(), value);
    }

    pub fn size(&self) -> usize {
        self.active_frames
    }

    pub fn resize(&mut self, frames: usize) {
        self.active_frames = frames.min(MAX_FRAMES);
    }

    pub fn active_samples_mut(&mut self) -> &mut [f32] {
        &mut self.samples[..self.active_frames * CHANNELS]
    }
}

impl<const CHANNELS: usize, const MAX_FRAMES: usize, const SAMPLES: usize> Default
    for AudioBlock<CHANNELS, MAX_FRAMES, SAMPLES>
{
    fn default() -> Self {
        Self::new()
    }
}

pub const BUS_SAMPLES: usize = 8 * BLOCK_FRAMES;
pub const STEREO_SAMPLES: usize = 2 * BLOCK_FRAMES;

pub type BusBlock = AudioBlock<8, BLOCK_FRAMES, BUS_SAMPLES>;
pub type StereoBlock = AudioBlock<2, BLOCK_FRAMES, STEREO_SAMPLES>;

/// 64-byte aligned planar (non-interleaved) channel buffers for FAUST DSP computation.
#[repr(align(64))]
pub struct PlanarAudioBlock<const CHANNELS: usize, const MAX_FRAMES: usize, T> {
    pub storage: [[T; MAX_FRAMES]; CHANNELS],
}

impl<const CHANNELS: usize, const MAX_FRAMES: usize, T> PlanarAudioBlock<CHANNELS, MAX_FRAMES, T> {
    pub fn channel(&self, channel: usize) -> &[T] {
        &self.storage[channel]
    }
}

impl<const CHANNELS: usize, const MAX_FRAMES: usize, T: Copy + Default>
    PlanarAudioBlock<CHANNELS, MAX_FRAMES, T>
{
    pub fn new() -> PlanarAudioBlock<CHANNELS, MAX_FRAMES, T> {
        PlanarAudioBlock {
            storage: [[T::default(); MAX_FRAMES]; CHANNELS],
        }
    }
}

impl<const CHANNELS: usize, const MAX_FRAMES: usize, T: Copy>
    PlanarAudioBlock<CHANNELS, MAX_FRAMES, T>
{
    pub fn at(&self, channel: usize, frame: usize) -> T {
        self.storage[channel][frame]
    }

    pub fn set(&mut self, channel: usize, frame: usize, value: T) {
        self.storage[channel][frame] = value;
    }
}

impl<const CHANNELS: usize, const MAX_FRAMES: usize> PlanarAudioBlock<CHANNELS, MAX_FRAMES, f32> {
    /// Deinterleaves and scales a chunk of OTIS bus frames into planar DSP inputs.
    pub fn load_buses(
        &mut self,
        buses: &BusBlock,
        offset: usize,
        frames: usize,
        gain: [f32; 2],
        dry_only: bool,
    ) {
        let count = MAX_FRAMES.min(frames).min(buses.size().saturating_sub(offset));
        let [gain_l, gain_r] = gain;
        let copy_pair = |dst: &mut Self, dsp_ch: usize, bus_ch: usize| {
            for frame in 0..count {
                dst.storage[dsp_ch][frame] = buses.at(offset + frame, bus_ch) * gain_l;
                dst.storage[dsp_ch + 1][frame] = buses.at(offset + frame, bus_ch + 1) * gain_r;
            }
        };

        copy_pair(self, 0, OtisBus::DryL.index());
        if !dry_only {
            if CHANNELS >= 4 {
                copy_pair(self, 2, OtisBus::DelayL.index());
            }
            if CHANNELS >= 6 {
                copy_pair(self, 4, OtisBus::ReverbL.index());
            }
        }
    }
}

impl<const CHANNELS: usize, const MAX_FRAMES: usize, T: Copy + Default> Default
    for PlanarAudioBlock<CHANNELS, MAX_FRAMES, T>
{
    fn default() -> Self {
        Self::new()
    }
}
