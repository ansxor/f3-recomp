//! The worker's PCM ring and the video-frame boundary that `drain_released` honours. The owner
//! shares the ring with the worker as `Arc<Mutex<PcmRing>>`, so the ring itself is plain data.

use crate::hle::core::PcmSink;

pub const CAPACITY: usize = 32768;

pub struct PcmRing {
    // Boxed: 256 KiB would overflow small thread stacks.
    frames: Box<[f32; CAPACITY * 2]>,
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
        let frames = vec![0.0f32; CAPACITY * 2]
            .into_boxed_slice()
            .try_into()
            .unwrap_or_else(|_| unreachable!("vec length is CAPACITY * 2"));
        PcmRing {
            frames,
            read: 0,
            write: 0,
            released: 0,
            pending: 0,
        }
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

    /// The blocking float drain never holds anything back.
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn drain_swaps_channels() {
        let mut ring = PcmRing::new();
        ring.write(&[0.25, -0.5], 1);
        let mut out = [0.0f32; 2];
        assert_eq!(ring.drain(&mut out, 8), 1);
        assert_eq!(out, [-0.5, 0.25]);
    }

    #[test]
    fn drain_i16_scales_and_clamps() {
        let mut ring = PcmRing::new();
        ring.write(&[1.0, -1.0, 2.0, -2.0, 0.5, 0.0], 3);
        let mut out = [0i16; 6];
        assert_eq!(ring.drain_i16(&mut out, 3), 3);
        assert_eq!(out, [-32768, 32767, -32768, 32767, 0, 16384]);
    }

    #[test]
    fn released_drain_holds_back_the_open_frame() {
        let mut ring = PcmRing::new();
        ring.write(&[0.0; 8], 4);
        let mut out = [0i16; 8];
        ring.close_frame(2);
        assert_eq!(ring.drain_released(&mut out, 4), 0);
        ring.close_frame(4);
        assert_eq!(ring.drain_released(&mut out, 4), 2);
        assert_eq!(ring.drain_released(&mut out, 4), 0);
        ring.close_frame(4);
        assert_eq!(ring.drain_released(&mut out, 4), 2);
    }

    #[test]
    fn overflow_drops_oldest_frames() {
        let mut ring = PcmRing::new();
        let stereo: Vec<f32> = (0..CAPACITY + 1).flat_map(|n| [n as f32, 0.0]).collect();
        ring.write(&stereo, CAPACITY + 1);
        assert_eq!(ring.available(), CAPACITY);
        let mut out = [0.0f32; 2];
        ring.drain(&mut out, 1);
        assert_eq!(out[1], 1.0);
    }

    #[test]
    fn discard_empties_everything() {
        let mut ring = PcmRing::new();
        ring.write(&[0.0; 4], 2);
        ring.close_frame(2);
        ring.discard();
        assert_eq!(ring.available(), 0);
        let mut out = [0i16; 4];
        assert_eq!(ring.drain_released(&mut out, 2), 0);
    }
}
