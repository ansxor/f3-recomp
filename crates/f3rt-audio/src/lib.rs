//! The F3 sound board HLE engine and its C ABI (`include/f3rt/abi/audio.h`).
//!
//! `abi` exposes a single-owner process-wide `Audio` instance over `audio`, which mediates between
//! main-CPU mailbox writes and the HLE synth worker thread (`hle::core::Core`).

pub mod abi;
pub mod audio;
pub mod bytes;
pub mod hle;
pub mod memory_map;
pub mod pcm_ring;
pub mod shared_mailbox;
pub mod snapshot;
pub mod worker_queue;

pub(crate) fn panic_message(payload: &(dyn std::any::Any + Send)) -> String {
    if let Some(message) = payload.downcast_ref::<&str>() {
        (*message).to_owned()
    } else if let Some(message) = payload.downcast_ref::<String>() {
        message.clone()
    } else {
        "unknown exception".to_owned()
    }
}
