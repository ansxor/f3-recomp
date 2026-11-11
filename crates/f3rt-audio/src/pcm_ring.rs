//! `audio/pcm_ring.hpp`: the worker's PCM ring and the frame boundary of `finish_frame`. The C++
//! class already owns the mutex and the released counter, so every method here takes `&self`.

use std::sync::Mutex;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::hle::core::PcmSink;

pub const CAPACITY: usize = 32768;

struct Ring {
    frames: [f32; CAPACITY * 2],
    read: u64,
    write: u64,
}

pub struct PcmRing {
    ring: Mutex<Ring>,
    released: AtomicU64,
    /// Written and read by the owner thread only; atomic so `close_frame` can take `&self`.
    pending: AtomicU64,
}

impl Default for PcmRing {
    fn default() -> Self {
        Self::new()
    }
}

impl PcmRing {
    pub fn new() -> PcmRing {
        PcmRing {
            ring: Mutex::new(Ring { frames: [0.0; CAPACITY * 2], read: 0, write: 0 }),
            released: AtomicU64::new(0),
            pending: AtomicU64::new(0),
        }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Ring> {
        self.ring.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    pub fn available(&self) -> usize {
        let ring = self.lock();
        (ring.write - ring.read) as usize
    }

    pub fn drain(&self, out: &mut [f32], max_frames: usize) -> usize {
        self.take_f32(out, max_frames)
    }

    pub fn drain_i16(&self, out: &mut [i16], max_frames: usize) -> usize {
        self.take_i16(out, max_frames, false)
    }

    pub fn drain_released(&self, out: &mut [i16], max_frames: usize) -> usize {
        self.take_i16(out, max_frames, true)
    }

    pub fn close_frame(&self, frame_end: u64) {
        self.released.store(self.pending.load(Ordering::Relaxed), Ordering::Release);
        self.pending.store(frame_end, Ordering::Relaxed);
    }

    pub fn discard(&self) {
        let mut ring = self.lock();
        // `m_read = m_pending = m_write`.
        let write = ring.write;
        ring.read = write;
        self.pending.store(write, Ordering::Relaxed);
        self.released.store(write, Ordering::Release);
    }

    fn ready(&self, ring: &Ring, released: bool) -> u64 {
        let mut ready = ring.write - ring.read;
        if released {
            let released = self.released.load(Ordering::Acquire);
            ready = ready.min(released.saturating_sub(ring.read));
        }
        ready
    }

    /// `take<false, float>`: the blocking float drain never holds anything back.
    fn take_f32(&self, out: &mut [f32], max_frames: usize) -> usize {
        let mut ring = self.lock();
        let ready = self.ready(&ring, false);
        let count = (max_frames as u64).min(ready) as usize;
        for n in 0..count {
            for c in 0..2usize {
                let value = ring.frames[(ring.read % CAPACITY as u64) as usize * 2 + c];
                out[n * 2 + (c ^ 1)] = value;
            }
            ring.read += 1;
        }
        count
    }

    fn take_i16(&self, out: &mut [i16], max_frames: usize, released: bool) -> usize {
        let mut ring = self.lock();
        let ready = self.ready(&ring, released);
        let count = (max_frames as u64).min(ready) as usize;
        for n in 0..count {
            for c in 0..2usize {
                let value = ring.frames[(ring.read % CAPACITY as u64) as usize * 2 + c];
                let scaled = value * 32768.0f32;
                let clamped = scaled.clamp(-32768.0f32, 32767.0f32);
                out[n * 2 + (c ^ 1)] = clamped as i16;
            }
            ring.read += 1;
        }
        count
    }
}

impl PcmSink for PcmRing {
    fn write(&self, stereo: &[f32], frames: usize) {
        let mut ring = self.lock();
        for i in 0..frames {
            let pos = (ring.write % CAPACITY as u64) as usize * 2;
            ring.frames[pos] = stereo[i * 2];
            ring.frames[pos + 1] = stereo[i * 2 + 1];
            ring.write += 1;
        }
        if ring.write - ring.read > CAPACITY as u64 {
            ring.read = ring.write - CAPACITY as u64;
        }
    }
}
