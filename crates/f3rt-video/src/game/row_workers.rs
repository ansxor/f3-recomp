//! Scoped workers for disjoint scanout chunks.

use std::sync::{LazyLock, Mutex};
use std::thread;
use std::time::Instant;

/// Helper threads beyond the submitting thread (`MAX_COMPOSE_HELPERS`).
pub const MAX_COMPOSE_HELPERS: usize = 3;
/// Rows per composition chunk (`COMPOSE_CHUNK_ROWS`).
pub const COMPOSE_CHUNK_ROWS: usize = 8;

/// One participation record (`ParticipantTrace`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ParticipantTrace {
    pub joined: bool,
    pub chunks: u32,
    pub start_us: u32,
    pub end_us: u32,
}

/// One dispatch record (`DispatchTrace`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DispatchTrace {
    pub participants: [ParticipantTrace; 1 + MAX_COMPOSE_HELPERS],
    pub total_us: u32,
}

/// Called around the submitting thread's share of a run (`WorkWindow`).
pub trait WorkWindow {
    fn begin(&mut self);
    fn end(&mut self);
}

/// Chunk counts per participant, index 0 = submitting thread.
pub type ChunkCounts = [u32; 1 + MAX_COMPOSE_HELPERS];

fn helper_count() -> usize {
    let hardware = thread::available_parallelism()
        .map(|count| count.get())
        .unwrap_or(0);
    if hardware == 0 {
        1
    } else {
        MAX_COMPOSE_HELPERS.min(hardware - 1)
    }
}

/// Chunk scheduler over the visible scanout window.
pub struct RowWorkers {
    helpers: usize,
    trace: Mutex<DispatchTrace>,
}

impl Default for RowWorkers {
    fn default() -> Self {
        Self::new()
    }
}

impl RowWorkers {
    pub fn new() -> Self {
        Self {
            helpers: helper_count(),
            trace: Mutex::new(DispatchTrace::default()),
        }
    }

    /// Run the chunks concurrently; the submitting thread also pulls work.
    pub fn run(
        &self,
        count: usize,
        body: &(dyn Fn(usize) + Sync),
        window: Option<&mut dyn WorkWindow>,
    ) {
        let items: Vec<(usize, ())> = (0..count).map(|chunk| (chunk, ())).collect();
        self.dispatch(items, &|chunk, ()| body(chunk), window);
    }

    /// Run `body(chunk, item)` for owned per-chunk work. This is the parallel-safe entry point:
    /// each item owns the buffers for its chunk, so helpers never share mutable frame memory.
    pub fn run_owned<T: Send>(
        &self,
        items: Vec<(usize, T)>,
        body: impl Fn(usize, T) + Sync,
        window: Option<&mut dyn WorkWindow>,
    ) {
        self.dispatch(items, &body, window);
    }

    pub fn chunk_counts(&self) -> ChunkCounts {
        let trace = self.trace.lock().expect("trace lock");
        let mut counts = [0u32; 1 + MAX_COMPOSE_HELPERS];
        for (count, participant) in counts.iter_mut().zip(trace.participants.iter()) {
            *count = participant.chunks;
        }
        counts
    }

    pub fn trace(&self) -> DispatchTrace {
        *self.trace.lock().expect("trace lock")
    }

    fn dispatch<T: Send>(
        &self,
        items: Vec<(usize, T)>,
        body: &(impl Fn(usize, T) + Sync),
        mut window: Option<&mut dyn WorkWindow>,
    ) {
        let total = items.len();
        let start = Instant::now();
        let micros = || u32::try_from(start.elapsed().as_micros()).unwrap_or(u32::MAX);
        let queue = Mutex::new(items.into_iter());
        let trace = Mutex::new(DispatchTrace::default());
        let helpers = self.helpers.min(total.saturating_sub(1));

        let pull = |slot: usize| {
            let start_us = micros();
            let mut chunks = 0u32;
            loop {
                let next = queue.lock().expect("chunk queue lock").next();
                let Some((chunk, item)) = next else { break };
                body(chunk, item);
                chunks += 1;
            }
            let end_us = micros();
            trace.lock().expect("trace lock").participants[slot] = ParticipantTrace {
                joined: true,
                chunks,
                start_us,
                end_us,
            };
        };

        thread::scope(|scope| {
            for slot in 1..=helpers {
                scope.spawn(move || pull(slot));
            }
            if let Some(window) = window.as_mut() {
                window.begin();
            }
            pull(0);
            if let Some(window) = window.as_mut() {
                window.end();
            }
        });

        let mut record = trace.into_inner().expect("trace lock");
        record.total_us = micros();
        *self.trace.lock().expect("trace lock") = record;
    }
}

static COMPOSE_WORKERS: LazyLock<RowWorkers> = LazyLock::new(RowWorkers::new);

/// The shared composition pool (`compose_workers()`).
pub fn compose_workers() -> &'static RowWorkers {
    &COMPOSE_WORKERS
}

pub fn compose_chunk_count(first_line: u32, end_line: u32) -> usize {
    ((end_line - first_line) as usize).div_ceil(COMPOSE_CHUNK_ROWS)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    struct RecordingWindow {
        begins: AtomicUsize,
        ends: AtomicUsize,
    }

    impl WorkWindow for RecordingWindow {
        fn begin(&mut self) {
            self.begins.fetch_add(1, Ordering::Relaxed);
        }
        fn end(&mut self) {
            self.ends.fetch_add(1, Ordering::Relaxed);
        }
    }

    #[test]
    fn every_chunk_runs_once_and_trace_matches() {
        let workers = RowWorkers::new();
        let seen: Vec<AtomicUsize> = (0..23).map(|_| AtomicUsize::new(0)).collect();
        workers.run(
            23,
            &|chunk| {
                seen[chunk].fetch_add(1, Ordering::Relaxed);
            },
            None,
        );
        assert!(seen.iter().all(|count| count.load(Ordering::Relaxed) == 1));
        let counts = workers.chunk_counts();
        assert_eq!(counts.iter().sum::<u32>(), 23);
        let trace = workers.trace();
        assert!(trace.participants[0].joined);
        assert_eq!(trace.participants[0].chunks, counts[0]);
    }

    #[test]
    fn owned_items_land_in_their_chunk() {
        let workers = RowWorkers::new();
        let slots: Vec<AtomicUsize> = (0..17).map(|_| AtomicUsize::new(usize::MAX)).collect();
        let items: Vec<(usize, usize)> = (0..17).map(|chunk| (chunk, chunk * 3)).collect();
        {
            let mut window = RecordingWindow {
                begins: AtomicUsize::new(0),
                ends: AtomicUsize::new(0),
            };
            workers.run_owned(
                items,
                |chunk, value| {
                    // Disjoint writes: each chunk owns one slot.
                    assert_eq!(value, chunk * 3);
                    slots[chunk].store(value, Ordering::Relaxed);
                },
                Some(&mut window),
            );
            assert_eq!(window.begins.load(Ordering::Relaxed), 1);
            assert_eq!(window.ends.load(Ordering::Relaxed), 1);
        }
        for (chunk, slot) in slots.iter().enumerate() {
            assert_eq!(slot.load(Ordering::Relaxed), chunk * 3);
        }
    }

    #[test]
    fn chunk_count_covers_the_window() {
        assert_eq!(compose_chunk_count(24, 256), 29);
        assert_eq!(compose_chunk_count(0, 200), 25);
        assert_eq!(compose_chunk_count(40, 240), 25);
    }
}
