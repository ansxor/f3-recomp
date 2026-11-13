//! `audio/worker_queue.hpp`: the synth worker thread and its bounded input queue.
//!
//! The C++ queue holds a `Worker&` that lives in the same owner; here the worker is a raw pointer for
//! the same reason the services use one (it is a field of the owning structure, so its address is
//! stable and the queue must not borrow it). `ThreadRole::AudioSynth` behaves exactly like
//! `std::thread` on this platform, and the worker sets FTZ/DAZ for itself first.
//!
//! A failure inside `Worker::apply` (a panic, which is the Rust form of the C++ exception) is kept
//! and re-raised on the caller's thread by the next `post`/`flush`, as the C++ `std::exception_ptr`
//! round trip does; the caller then aborts with the same message.

use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::{self, JoinHandle};

/// The C++ `Applies` concept: `worker.apply(message)`.
pub trait Applies<M> {
    fn apply(&mut self, message: &M);
}

/// The C++ `Worker&` member: a non-owning pointer the queue hands to its thread.
struct WorkerPtr<W>(*mut W);

// SAFETY: only the queue's thread dereferences the pointer, and the owner joins that thread before
// the worker is dropped (the owner declares the queue before the worker, so it drops first).
unsafe impl<W> Send for WorkerPtr<W> {}

struct Shared<M, const CAPACITY: usize> {
    state: Mutex<State<M, CAPACITY>>,
    wake: Condvar,
    completed: Condvar,
}

struct State<M, const CAPACITY: usize> {
    slots: [M; CAPACITY],
    pushed: u64,
    popped: u64,
    done: u64,
    stopping: bool,
    failure: Option<String>,
}

pub struct WorkerQueue<M, W, const CAPACITY: usize> {
    shared: Arc<Shared<M, CAPACITY>>,
    thread: Option<JoinHandle<()>>,
    marker: std::marker::PhantomData<fn() -> W>,
}

impl<M, W, const CAPACITY: usize> WorkerQueue<M, W, CAPACITY>
where
    M: Copy + Default + Send + 'static,
    W: Applies<M> + 'static,
{
    /// Starts the synth thread over `*worker`, which must outlive the queue.
    pub fn new(worker: *mut W) -> WorkerQueue<M, W, CAPACITY> {
        let shared = Arc::new(Shared {
            state: Mutex::new(State {
                slots: [M::default(); CAPACITY],
                pushed: 0,
                popped: 0,
                done: 0,
                stopping: false,
                failure: None,
            }),
            wake: Condvar::new(),
            completed: Condvar::new(),
        });
        let worker = WorkerPtr(worker);
        let thread_shared = Arc::clone(&shared);
        let thread_worker = WorkerPtr(worker.0);
        let thread = thread::Builder::new()
            .spawn(move || run::<M, W, CAPACITY>(thread_shared, thread_worker))
            .expect("HLE synth worker thread");
        WorkerQueue { shared, thread: Some(thread), marker: std::marker::PhantomData }
    }

    pub fn post(&self, message: M) {
        let mut state = self.lock();
        state = self
            .shared
            .completed
            .wait_while(state, |s| {
                !(s.pushed.wrapping_sub(s.popped) < CAPACITY as u64 || s.failure.is_some())
            })
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let failure = state.failure.clone();
        drop(state);
        if let Some(failure) = failure {
            panic!("{}", failure);
        }
        let mut state = self.lock();
        let index = (state.pushed % CAPACITY as u64) as usize;
        state.slots[index] = message;
        state.pushed += 1;
        drop(state);
        self.shared.wake.notify_one();
    }

    pub fn flush(&self) {
        let mut state = self.lock();
        let target = state.pushed;
        state = self
            .shared
            .completed
            .wait_while(state, |s| !(s.done >= target || s.failure.is_some()))
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let failure = state.failure.clone();
        drop(state);
        if let Some(failure) = failure {
            panic!("{}", failure);
        }
    }
}

impl<M, W, const CAPACITY: usize> WorkerQueue<M, W, CAPACITY> {
    fn lock(&self) -> MutexGuard<'_, State<M, CAPACITY>> {
        self.shared.state.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
    }
}

impl<M, W, const CAPACITY: usize> Drop for WorkerQueue<M, W, CAPACITY> {
    fn drop(&mut self) {
        {
            let mut state = self.lock();
            state.stopping = true;
        }
        self.shared.wake.notify_all();
        self.shared.completed.notify_all();
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

fn run<M, W, const CAPACITY: usize>(shared: Arc<Shared<M, CAPACITY>>, worker: WorkerPtr<W>)
where
    M: Copy + Default + Send + 'static,
    W: Applies<M> + 'static,
{
    crate::hle::core::Core::enable_flush_denormals();
    let body = catch_unwind(AssertUnwindSafe(|| {
        loop {
            let (message, ticket);
            {
                let mut state =
                    shared.state.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
                state = shared
                    .wake
                    .wait_while(state, |s| !(s.stopping || s.popped != s.pushed))
                    .unwrap_or_else(std::sync::PoisonError::into_inner);
                if state.stopping && state.popped == state.pushed {
                    return;
                }
                message = state.slots[(state.popped % CAPACITY as u64) as usize];
                state.popped += 1;
                ticket = state.popped;
                drop(state);
                shared.completed.notify_all();
            }
            // SAFETY: the queue owns the thread that dereferences this pointer and its owner joins
            // it before the worker storage goes away.
            unsafe { (*worker.0).apply(&message) };
            {
                let mut state =
                    shared.state.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
                state.done = ticket;
            }
            shared.completed.notify_all();
        }
    }));
    if let Err(payload) = body {
        let mut state = shared.state.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        state.failure = Some(crate::panic_message(payload.as_ref()));
        drop(state);
        shared.completed.notify_all();
        shared.wake.notify_all();
    }
}
