//! `audio/hle/block.hpp`: interleaved 8-channel bus blocks, the non-owning views over them and the
//! planar buffers the FAUST DSPs read and write.
//!
//! The views are raw pointer + frame count, exactly like the C++ `AudioBlockSpan`; the owning blocks
//! carry their samples inline. `PlanarAudioBlock` does not cache the channel pointer array the C++
//! keeps (`T* ptrs[Channels]`): caching self-pointers is unsound across a move in Rust, so `data()`
//! and `ptr()` rebuild it from the storage at each use, which has the same observable behaviour.

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

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum BusSelect {
    All,
    Dry,
    Delay,
    Reverb,
}

pub fn otis_target_bus(pair: u8) -> Option<BusSelect> {
    match pair {
        1 => Some(BusSelect::Dry),
        2 => Some(BusSelect::Delay),
        3 => Some(BusSelect::Reverb),
        _ => None,
    }
}

/// `std::dynamic_extent`: the count argument of `subspan` meaning "to the end".
pub const DYNAMIC_EXTENT: usize = usize::MAX;

/// Non-owning read-only 2D view over interleaved frame data.
#[derive(Clone, Copy)]
pub struct AudioBlockSpan<const CHANNELS: usize> {
    data: *const f32,
    frames: usize,
}

impl<const CHANNELS: usize> Default for AudioBlockSpan<CHANNELS> {
    fn default() -> Self {
        AudioBlockSpan { data: std::ptr::null(), frames: 0 }
    }
}

impl<const CHANNELS: usize> AudioBlockSpan<CHANNELS> {
    pub const fn new(data: *const f32, frames: usize) -> AudioBlockSpan<CHANNELS> {
        AudioBlockSpan { data, frames }
    }

    /// Explicit span over an interleaved sample buffer.
    pub fn from_samples(samples: &[f32]) -> AudioBlockSpan<CHANNELS> {
        AudioBlockSpan::new(samples.as_ptr(), samples.len() / CHANNELS)
    }

    pub fn at(&self, f: usize, ch: usize) -> f32 {
        unsafe { *self.data.add(f * CHANNELS + ch) }
    }

    pub fn at_channel(&self, f: usize, ch: StereoChannel) -> f32 {
        self.at(f, ch.index())
    }

    pub fn at_bus(&self, f: usize, ch: OtisBus) -> f32 {
        self.at(f, ch.index())
    }

    pub fn frame_ptr(&self, f: usize) -> *const f32 {
        unsafe { self.data.add(f * CHANNELS) }
    }

    pub fn data(&self) -> *const f32 {
        self.data
    }

    pub fn size(&self) -> usize {
        self.frames
    }

    pub fn empty(&self) -> bool {
        self.frames == 0
    }

    pub fn subspan(&self, offset: usize, count: usize) -> AudioBlockSpan<CHANNELS> {
        let clamped_offset = offset.min(self.frames);
        let clamped_count = if count == DYNAMIC_EXTENT {
            self.frames - clamped_offset
        } else {
            count.min(self.frames - clamped_offset)
        };
        AudioBlockSpan::new(unsafe { self.data.add(clamped_offset * CHANNELS) }, clamped_count)
    }

    /// `bus * gain`.
    pub fn scaled(&self, gain: [f32; 2]) -> ScaledBusSlice {
        ScaledBusSlice { slice: BusSlice { span: self.as_bus(), select: BusSelect::All }, gain }
    }

    fn as_bus(&self) -> BusSpan {
        BusSpan::new(self.data.cast(), self.frames)
    }
}

/// The bus views only exist for the 8-channel block (the C++ `requires(Channels == 8)`).
impl AudioBlockSpan<8> {
    pub fn dry(&self) -> BusSlice {
        BusSlice { span: *self, select: BusSelect::Dry }
    }

    pub fn delay(&self) -> BusSlice {
        BusSlice { span: *self, select: BusSelect::Delay }
    }

    pub fn reverb(&self) -> BusSlice {
        BusSlice { span: *self, select: BusSelect::Reverb }
    }

    pub fn slice(&self, sel: BusSelect) -> BusSlice {
        BusSlice { span: *self, select: sel }
    }
}

/// Non-owning mutable 2D view over interleaved frame data.
#[derive(Clone, Copy)]
pub struct MutableAudioBlockSpan<const CHANNELS: usize> {
    data: *mut f32,
    frames: usize,
}

impl<const CHANNELS: usize> Default for MutableAudioBlockSpan<CHANNELS> {
    fn default() -> Self {
        MutableAudioBlockSpan { data: std::ptr::null_mut(), frames: 0 }
    }
}

impl<const CHANNELS: usize> MutableAudioBlockSpan<CHANNELS> {
    pub const fn new(data: *mut f32, frames: usize) -> MutableAudioBlockSpan<CHANNELS> {
        MutableAudioBlockSpan { data, frames }
    }

    pub fn from_samples(samples: &mut [f32]) -> MutableAudioBlockSpan<CHANNELS> {
        let frames = samples.len() / CHANNELS;
        MutableAudioBlockSpan::new(samples.as_mut_ptr(), frames)
    }

    pub fn at(&self, f: usize, ch: usize) -> f32 {
        unsafe { *self.data.add(f * CHANNELS + ch) }
    }

    pub fn at_channel(&self, f: usize, ch: StereoChannel) -> f32 {
        self.at(f, ch.index())
    }

    pub fn at_bus(&self, f: usize, ch: OtisBus) -> f32 {
        self.at(f, ch.index())
    }

    pub fn set(&self, f: usize, ch: usize, value: f32) {
        unsafe { *self.data.add(f * CHANNELS + ch) = value };
    }

    pub fn set_channel(&self, f: usize, ch: StereoChannel, value: f32) {
        self.set(f, ch.index(), value);
    }

    pub fn set_bus(&self, f: usize, ch: OtisBus, value: f32) {
        self.set(f, ch.index(), value);
    }

    pub fn add(&self, f: usize, ch: usize, delta: f32) {
        unsafe { *self.data.add(f * CHANNELS + ch) += delta };
    }

    pub fn frame_ptr(&self, f: usize) -> *mut f32 {
        unsafe { self.data.add(f * CHANNELS) }
    }

    pub fn data(&self) -> *mut f32 {
        self.data
    }

    pub fn size(&self) -> usize {
        self.frames
    }

    pub fn empty(&self) -> bool {
        self.frames == 0
    }

    pub fn subspan(&self, offset: usize, count: usize) -> MutableAudioBlockSpan<CHANNELS> {
        let clamped_offset = offset.min(self.frames);
        let clamped_count = if count == DYNAMIC_EXTENT {
            self.frames - clamped_offset
        } else {
            count.min(self.frames - clamped_offset)
        };
        MutableAudioBlockSpan::new(unsafe { self.data.add(clamped_offset * CHANNELS) }, clamped_count)
    }

    pub fn as_const(&self) -> AudioBlockSpan<CHANNELS> {
        AudioBlockSpan::new(self.data, self.frames)
    }

    /// `bus * gain`.
    pub fn scaled(&self, gain: [f32; 2]) -> ScaledBusSlice {
        ScaledBusSlice { slice: BusSlice { span: self.as_bus(), select: BusSelect::All }, gain }
    }

    fn as_bus(&self) -> BusSpan {
        BusSpan::new(self.data.cast_const(), self.frames)
    }

    /// `operator+=`: adds `src` into the first `min(self, src)` frames, channel by channel.
    pub fn add_assign_span(&mut self, src: AudioBlockSpan<CHANNELS>) {
        let n = self.size().min(src.size());
        for f in 0..n {
            for ch in 0..CHANNELS {
                self.add(f, ch, src.at(f, ch));
            }
        }
    }
}

/// The mutable bus views only exist for the 8-channel block.
impl MutableAudioBlockSpan<8> {
    pub fn dry(&mut self) -> MutableBusSlice {
        MutableBusSlice { span: *self, select: BusSelect::Dry }
    }

    pub fn delay(&mut self) -> MutableBusSlice {
        MutableBusSlice { span: *self, select: BusSelect::Delay }
    }

    pub fn reverb(&mut self) -> MutableBusSlice {
        MutableBusSlice { span: *self, select: BusSelect::Reverb }
    }

    pub fn slice(&mut self, sel: BusSelect) -> MutableBusSlice {
        MutableBusSlice { span: *self, select: sel }
    }

    pub fn dry_const(&self) -> BusSlice {
        BusSlice { span: self.as_const(), select: BusSelect::Dry }
    }

    pub fn delay_const(&self) -> BusSlice {
        BusSlice { span: self.as_const(), select: BusSelect::Delay }
    }

    pub fn reverb_const(&self) -> BusSlice {
        BusSlice { span: self.as_const(), select: BusSelect::Reverb }
    }

    pub fn slice_const(&self, sel: BusSelect) -> BusSlice {
        BusSlice { span: self.as_const(), select: sel }
    }
}

/// 64-byte aligned storage owning interleaved frames. The sample count is the third parameter
/// because Rust cannot spell the product of two const parameters in an array length.
#[repr(align(64))]
pub struct AudioBlock<const CHANNELS: usize, const MAX_FRAMES: usize, const SAMPLES: usize> {
    pub samples: [f32; SAMPLES],
    pub active_frames: usize,
}

impl<const CHANNELS: usize, const MAX_FRAMES: usize, const SAMPLES: usize>
    AudioBlock<CHANNELS, MAX_FRAMES, SAMPLES>
{
    pub const fn new() -> AudioBlock<CHANNELS, MAX_FRAMES, SAMPLES> {
        AudioBlock { samples: [0.0; SAMPLES], active_frames: 0 }
    }

    pub fn at(&self, f: usize, ch: usize) -> f32 {
        self.samples[f * CHANNELS + ch]
    }

    pub fn at_channel(&self, f: usize, ch: StereoChannel) -> f32 {
        self.at(f, ch.index())
    }

    pub fn at_bus(&self, f: usize, ch: OtisBus) -> f32 {
        self.at(f, ch.index())
    }

    pub fn set(&mut self, f: usize, ch: usize, value: f32) {
        self.samples[f * CHANNELS + ch] = value;
    }

    pub fn set_channel(&mut self, f: usize, ch: StereoChannel, value: f32) {
        self.set(f, ch.index(), value);
    }

    pub fn set_bus(&mut self, f: usize, ch: OtisBus, value: f32) {
        self.set(f, ch.index(), value);
    }

    pub fn frame_ptr(&self, f: usize) -> *const f32 {
        unsafe { self.samples.as_ptr().add(f * CHANNELS) }
    }

    pub fn frame_ptr_mut(&mut self, f: usize) -> *mut f32 {
        unsafe { self.samples.as_mut_ptr().add(f * CHANNELS) }
    }

    pub fn span(&self) -> AudioBlockSpan<CHANNELS> {
        AudioBlockSpan::new(self.samples.as_ptr(), self.active_frames)
    }

    pub fn span_mut(&mut self) -> MutableAudioBlockSpan<CHANNELS> {
        MutableAudioBlockSpan::new(self.samples.as_mut_ptr(), self.active_frames)
    }

    pub fn subspan(&self, offset: usize, count: usize) -> AudioBlockSpan<CHANNELS> {
        self.span().subspan(offset, count)
    }

    pub fn subspan_mut(&mut self, offset: usize, count: usize) -> MutableAudioBlockSpan<CHANNELS> {
        self.span_mut().subspan(offset, count)
    }

    pub fn data(&mut self) -> *mut f32 {
        self.samples.as_mut_ptr()
    }

    pub fn data_const(&self) -> *const f32 {
        self.samples.as_ptr()
    }

    pub fn size(&self) -> usize {
        self.active_frames
    }

    pub fn capacity(&self) -> usize {
        MAX_FRAMES
    }

    pub fn resize(&mut self, n: usize) {
        self.active_frames = n.min(MAX_FRAMES);
    }

    pub fn fill(&mut self, value: f32) {
        self.samples.fill(value);
    }
}

/// The bus views only exist for the 8-channel block.
impl<const MAX_FRAMES: usize, const SAMPLES: usize> AudioBlock<8, MAX_FRAMES, SAMPLES> {
    pub fn dry(&mut self) -> MutableBusSlice {
        self.span_mut().dry()
    }

    pub fn delay(&mut self) -> MutableBusSlice {
        self.span_mut().delay()
    }

    pub fn reverb(&mut self) -> MutableBusSlice {
        self.span_mut().reverb()
    }

    pub fn slice(&mut self, sel: BusSelect) -> MutableBusSlice {
        self.span_mut().slice(sel)
    }

    pub fn dry_const(&self) -> BusSlice {
        self.span().dry()
    }

    pub fn delay_const(&self) -> BusSlice {
        self.span().delay()
    }

    pub fn reverb_const(&self) -> BusSlice {
        self.span().reverb()
    }

    pub fn slice_const(&self, sel: BusSelect) -> BusSlice {
        self.span().slice(sel)
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
pub type BusSpan = AudioBlockSpan<8>;
pub type StereoSpan = AudioBlockSpan<2>;
pub type MutableStereoSpan = MutableAudioBlockSpan<2>;

#[derive(Clone, Copy)]
pub struct BusSlice {
    pub span: BusSpan,
    pub select: BusSelect,
}

impl BusSlice {
    pub fn dry(self) -> BusSlice {
        BusSlice { span: self.span, select: BusSelect::Dry }
    }

    pub fn delay(self) -> BusSlice {
        BusSlice { span: self.span, select: BusSelect::Delay }
    }

    pub fn reverb(self) -> BusSlice {
        BusSlice { span: self.span, select: BusSelect::Reverb }
    }

    pub fn slice(self, sel: BusSelect) -> BusSlice {
        BusSlice { span: self.span, select: sel }
    }

    pub fn size(&self) -> usize {
        self.span.size()
    }

    pub fn data(&self) -> *const f32 {
        self.span.data()
    }

    /// `slice * gain`.
    pub fn scaled(&self, gain: [f32; 2]) -> ScaledBusSlice {
        ScaledBusSlice { slice: *self, gain }
    }
}

#[derive(Clone, Copy)]
pub struct MutableBusSlice {
    pub span: MutableAudioBlockSpan<8>,
    pub select: BusSelect,
}

impl MutableBusSlice {
    pub fn dry(&mut self) -> MutableBusSlice {
        MutableBusSlice { span: self.span, select: BusSelect::Dry }
    }

    pub fn delay(&mut self) -> MutableBusSlice {
        MutableBusSlice { span: self.span, select: BusSelect::Delay }
    }

    pub fn reverb(&mut self) -> MutableBusSlice {
        MutableBusSlice { span: self.span, select: BusSelect::Reverb }
    }

    pub fn slice(&mut self, sel: BusSelect) -> MutableBusSlice {
        MutableBusSlice { span: self.span, select: sel }
    }

    pub fn size(&self) -> usize {
        self.span.size()
    }

    pub fn data(&self) -> *mut f32 {
        self.span.data()
    }

    pub fn as_const(&self) -> BusSlice {
        BusSlice { span: self.span.as_const(), select: self.select }
    }

    pub fn channel_left(&self) -> usize {
        match self.select {
            BusSelect::Dry => OtisBus::DryL.index(),
            BusSelect::Delay => OtisBus::DelayL.index(),
            BusSelect::Reverb => OtisBus::ReverbL.index(),
            BusSelect::All => OtisBus::NcL.index(),
        }
    }

    /// `operator+=(MutableBusSlice, StereoSpan)`.
    pub fn add_assign_stereo(&mut self, src: StereoSpan) {
        let n = self.size().min(src.size());
        let ch_l = self.channel_left();
        let ch_r = ch_l + 1;
        for f in 0..n {
            self.span.add(f, ch_l, src.at(f, 0));
            self.span.add(f, ch_r, src.at(f, 1));
        }
    }

    /// `operator+=(MutableBusSlice, const PlanarAudioBlock<2, MaxFrames, T>&)`.
    pub fn add_assign_planar<const MAX_FRAMES: usize, T: Copy + Into<f32>>(
        &mut self,
        src: &PlanarAudioBlock<2, MAX_FRAMES, T>,
    ) {
        let n = self.size().min(MAX_FRAMES);
        let ch_l = self.channel_left();
        let ch_r = ch_l + 1;
        for f in 0..n {
            self.span.add(f, ch_l, src.at(0, f).into());
            self.span.add(f, ch_r, src.at(1, f).into());
        }
    }
}

#[derive(Clone, Copy)]
pub struct ScaledBusSlice {
    pub slice: BusSlice,
    pub gain: [f32; 2],
}

impl ScaledBusSlice {
    pub fn dry(&self) -> ScaledBusSlice {
        ScaledBusSlice { slice: self.slice.dry(), gain: self.gain }
    }

    pub fn delay(&self) -> ScaledBusSlice {
        ScaledBusSlice { slice: self.slice.delay(), gain: self.gain }
    }

    pub fn reverb(&self) -> ScaledBusSlice {
        ScaledBusSlice { slice: self.slice.reverb(), gain: self.gain }
    }
}

/// 64-byte aligned storage owning planar (non-interleaved) channel buffers.
#[repr(align(64))]
pub struct PlanarAudioBlock<const CHANNELS: usize, const MAX_FRAMES: usize, T> {
    pub storage: [[T; MAX_FRAMES]; CHANNELS],
}

impl<const CHANNELS: usize, const MAX_FRAMES: usize, T> PlanarAudioBlock<CHANNELS, MAX_FRAMES, T> {
    /// `T** data()`: the channel pointers, rebuilt from the storage at each call.
    pub fn data(&self) -> [*mut T; CHANNELS] {
        let mut ptrs = [std::ptr::null_mut(); CHANNELS];
        for (i, ptr) in ptrs.iter_mut().enumerate() {
            *ptr = self.ptr(i);
        }
        ptrs
    }

    pub fn ptr(&self, channel: usize) -> *mut T {
        self.storage[channel].as_ptr().cast_mut()
    }

    pub fn channel(&self, channel: usize) -> &[T] {
        &self.storage[channel]
    }

    pub fn channel_mut(&mut self, channel: usize) -> &mut [T] {
        &mut self.storage[channel]
    }

    pub fn capacity(&self) -> usize {
        MAX_FRAMES
    }
}

impl<const CHANNELS: usize, const MAX_FRAMES: usize, T: Copy + Default>
    PlanarAudioBlock<CHANNELS, MAX_FRAMES, T>
{
    pub fn new() -> PlanarAudioBlock<CHANNELS, MAX_FRAMES, T> {
        PlanarAudioBlock { storage: [[T::default(); MAX_FRAMES]; CHANNELS] }
    }
}

impl<const CHANNELS: usize, const MAX_FRAMES: usize, T: Copy>
    PlanarAudioBlock<CHANNELS, MAX_FRAMES, T>
{
    pub fn at(&self, channel: usize, f: usize) -> T {
        self.storage[channel][f]
    }

    pub fn set(&mut self, channel: usize, f: usize, value: T) {
        self.storage[channel][f] = value;
    }
}

impl<const CHANNELS: usize, const MAX_FRAMES: usize>
    PlanarAudioBlock<CHANNELS, MAX_FRAMES, f32>
{
    /// `operator=(const ScaledBusSlice&)`.
    pub fn assign_scaled(&mut self, scaled: &ScaledBusSlice) {
        let n = MAX_FRAMES.min(scaled.slice.size());
        let src = scaled.slice.span;
        let gl = scaled.gain[0];
        let gr = scaled.gain[1];

        let copy_pair = |dst: &mut Self, dsp_ch: usize, bus_ch: usize| {
            for f in 0..n {
                dst.storage[dsp_ch][f] = src.at(f, bus_ch) * gl;
                dst.storage[dsp_ch + 1][f] = src.at(f, bus_ch + 1) * gr;
            }
        };

        match scaled.slice.select {
            BusSelect::All => {
                copy_pair(self, 0, OtisBus::DryL.index());
                if CHANNELS >= 4 {
                    copy_pair(self, 2, OtisBus::DelayL.index());
                }
                if CHANNELS >= 6 {
                    copy_pair(self, 4, OtisBus::ReverbL.index());
                }
            }
            BusSelect::Dry => copy_pair(self, 0, OtisBus::DryL.index()),
            BusSelect::Delay => {
                if CHANNELS >= 4 {
                    copy_pair(self, 2, OtisBus::DelayL.index());
                }
            }
            BusSelect::Reverb => {
                if CHANNELS >= 6 {
                    copy_pair(self, 4, OtisBus::ReverbL.index());
                }
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
