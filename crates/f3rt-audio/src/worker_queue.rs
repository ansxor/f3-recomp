use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, PoisonError};
use std::thread::{self, JoinHandle};

pub trait Applies<M> {
    fn apply(&mut self, message: &M);
}

struct Shared<M, const CAPACITY: usize> {
    state: Mutex<State<M, CAPACITY>>,
    wake: Condvar,
    completed: Condvar,
}

impl<M, const CAPACITY: usize> Shared<M, CAPACITY> {
    fn lock(&self) -> MutexGuard<'_, State<M, CAPACITY>> {
        self.state.lock().unwrap_or_else(PoisonError::into_inner)
    }
}

struct State<M, const CAPACITY: usize> {
    slots: Box<[M; CAPACITY]>,
    pushed: u64,
    popped: u64,
    done: u64,
    stopping: bool,
    failure: Option<String>,
}

impl<M, const CAPACITY: usize> State<M, CAPACITY> {
    fn raise_failure(&self) {
        if let Some(failure) = &self.failure {
            panic!("{}", failure);
        }
    }
}

pub struct WorkerQueue<M, const CAPACITY: usize> {
    shared: Arc<Shared<M, CAPACITY>>,
    thread: Option<JoinHandle<()>>,
}

impl<M, const CAPACITY: usize> WorkerQueue<M, CAPACITY>
where
    M: Copy + Default + Send + 'static,
{
    pub fn spawn<W: Applies<M> + Send + 'static>(worker: W) -> WorkerQueue<M, CAPACITY> {
        let slots: Box<[M; CAPACITY]> = vec![M::default(); CAPACITY]
            .into_boxed_slice()
            .try_into()
            .unwrap_or_else(|_| unreachable!("vec length is CAPACITY"));
        let shared = Arc::new(Shared {
            state: Mutex::new(State {
                slots,
                pushed: 0,
                popped: 0,
                done: 0,
                stopping: false,
                failure: None,
            }),
            wake: Condvar::new(),
            completed: Condvar::new(),
        });
        let thread_shared = Arc::clone(&shared);
        let thread = thread::Builder::new()
            .spawn(move || run(thread_shared, worker))
            .expect("HLE synth worker thread");
        WorkerQueue {
            shared,
            thread: Some(thread),
        }
    }

    pub fn post(&self, message: M) {
        let state = self.shared.lock();
        let mut state = self
            .shared
            .completed
            .wait_while(state, |s| {
                s.pushed.wrapping_sub(s.popped) >= CAPACITY as u64 && s.failure.is_none()
            })
            .unwrap_or_else(PoisonError::into_inner);
        state.raise_failure();
        let index = (state.pushed % CAPACITY as u64) as usize;
        state.slots[index] = message;
        state.pushed += 1;
        drop(state);
        self.shared.wake.notify_one();
    }

    /// Blocks until every message posted so far has been applied.
    pub fn flush(&self) {
        let state = self.shared.lock();
        let target = state.pushed;
        let state = self
            .shared
            .completed
            .wait_while(state, |s| s.done < target && s.failure.is_none())
            .unwrap_or_else(PoisonError::into_inner);
        state.raise_failure();
    }
}

impl<M, const CAPACITY: usize> Drop for WorkerQueue<M, CAPACITY> {
    fn drop(&mut self) {
        self.shared.lock().stopping = true;
        self.shared.wake.notify_all();
        self.shared.completed.notify_all();
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

fn run<M, W, const CAPACITY: usize>(shared: Arc<Shared<M, CAPACITY>>, mut worker: W)
where
    M: Copy + Default + Send + 'static,
    W: Applies<M>,
{
    let body = catch_unwind(AssertUnwindSafe(|| {
        loop {
            let state = shared.lock();
            let mut state = shared
                .wake
                .wait_while(state, |s| !(s.stopping || s.popped != s.pushed))
                .unwrap_or_else(PoisonError::into_inner);
            if state.stopping && state.popped == state.pushed {
                return;
            }
            let message = state.slots[(state.popped % CAPACITY as u64) as usize];
            state.popped += 1;
            let ticket = state.popped;
            drop(state);
            shared.completed.notify_all();
            worker.apply(&message);
            shared.lock().done = ticket;
            shared.completed.notify_all();
        }
    }));
    if let Err(payload) = body {
        shared.lock().failure = Some(crate::panic_message(payload.as_ref()));
        shared.completed.notify_all();
        shared.wake.notify_all();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Log(Arc<Mutex<Vec<u32>>>);

    impl Applies<u32> for Log {
        fn apply(&mut self, message: &u32) {
            if *message == u32::MAX {
                panic!("synth exploded");
            }
            self.0.lock().unwrap().push(*message);
        }
    }

    #[test]
    fn applies_in_order_and_flush_waits_through_wraparound() {
        let log = Arc::new(Mutex::new(Vec::new()));
        let queue = WorkerQueue::<u32, 4>::spawn(Log(Arc::clone(&log)));
        for n in 0..50 {
            queue.post(n);
        }
        queue.flush();
        assert_eq!(*log.lock().unwrap(), (0..50).collect::<Vec<_>>());
    }

    #[test]
    fn worker_panic_resurfaces_on_flush_with_its_message() {
        let log = Arc::new(Mutex::new(Vec::new()));
        let queue = WorkerQueue::<u32, 4>::spawn(Log(log));
        queue.post(u32::MAX);
        let payload = catch_unwind(AssertUnwindSafe(|| queue.flush())).unwrap_err();
        assert_eq!(crate::panic_message(payload.as_ref()), "synth exploded");
    }

    #[test]
    fn worker_panic_resurfaces_on_post() {
        let log = Arc::new(Mutex::new(Vec::new()));
        let queue = WorkerQueue::<u32, 4>::spawn(Log(log));
        queue.post(u32::MAX);
        let payload = catch_unwind(AssertUnwindSafe(|| {
            for n in 0..64 {
                queue.post(n);
            }
            queue.flush();
        }))
        .unwrap_err();
        assert_eq!(crate::panic_message(payload.as_ref()), "synth exploded");
    }
}
