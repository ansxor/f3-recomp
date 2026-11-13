//! `audio/pcm_ring.hpp`: the worker's PCM ring and the frame boundary of `finish_frame`. The C++
//! class owns its mutex; here the owner shares the ring as `Arc<Mutex<PcmRing>>` with the worker,
//! so the ring itself is plain data.

use crate::hle::core::PcmSink;

pub const CAPACITY: usize = 32768;

pub struct PcmRing {
    frames: [f32; CAPACITY * 2],
    read: u64,
    write: u64,
    released: u64,
    pending: u64,
}

impl Default for PcmRing {
    fn default() -> Self {
        Self::new()
    }
}

impl PcmRing {
    pub fn new() -> PcmRing {
        PcmRing { frames: [0.0; CAPACITY * 2], read: 0, write: 0, released: 0, pending: 0 }
    }

    pub fn available(&self) -> usize {
        (self.write - self.read) as usize
    }

    pub fn drain(&mut self, out: &mut [f32], max_frames: usize) -> usize {
        self.take_f32(out, max_frames)
    }

    pub fn drain_i16(&mut self, out: &mut [i16], max_frames: usize) -> usize {
        self.take_i16(out, max_frames, false)
    }

    pub fn drain_released(&mut self, out: &mut [i16], max_frames: usize) -> usize {
        self.take_i16(out, max_frames, true)
    }

    pub fn close_frame(&mut self, frame_end: u64) {
        self.released = self.pending;
        self.pending = frame_end;
    }

    pub fn discard(&mut self) {
        // `m_read = m_pending = m_write`.
        self.read = self.write;
        self.pending = self.write;
        self.released = self.write;
    }

    fn ready(&self, released: bool) -> u64 {
        let mut ready = self.write - self.read;
        if released {
            ready = ready.min(self.released.saturating_sub(self.read));
        }
        ready
    }

    /// `take<false, float>`: the blocking float drain never holds anything back.
    fn take_f32(&mut self, out: &mut [f32], max_frames: usize) -> usize {
        let count = (max_frames as u64).min(self.ready(false)) as usize;
        for n in 0..count {
            for c in 0..2usize {
                let value = self.frames[(self.read % CAPACITY as u64) as usize * 2 + c];
                out[n * 2 + (c ^ 1)] = value;
            }
            self.read += 1;
        }
        count
    }

    fn take_i16(&mut self, out: &mut [i16], max_frames: usize, released: bool) -> usize {
        let count = (max_frames as u64).min(self.ready(released)) as usize;
        for n in 0..count {
            for c in 0..2usize {
                let value = self.frames[(self.read % CAPACITY as u64) as usize * 2 + c];
                let scaled = value * 32768.0f32;
                let clamped = scaled.clamp(-32768.0f32, 32767.0f32);
                out[n * 2 + (c ^ 1)] = clamped as i16;
            }
            self.read += 1;
        }
        count
    }
}

impl PcmSink for PcmRing {
    fn write(&mut self, stereo: &[f32], frames: usize) {
        for i in 0..frames {
            let pos = (self.write % CAPACITY as u64) as usize * 2;
            self.frames[pos] = stereo[i * 2];
            self.frames[pos + 1] = stereo[i * 2 + 1];
            self.write += 1;
        }
        if self.write - self.read > CAPACITY as u64 {
            self.read = self.write - CAPACITY as u64;
        }
    }
}
