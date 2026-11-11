//! The F3 sound board: a mechanical Rust translation of `runtime/audio/` and `runtime/audio/hle/`,
//! one module per C++ file, plus `abi`, the implementation of `include/f3rt/abi/audio.h`.
//!
//! The C++ and this crate are the same engine: same structures, same fields, same algorithms, same
//! operation order (floating point must stay bit-exact), same single-owner-thread model. Failures the
//! C++ reports with an exception become a panic, which the ABI layer turns into the process-aborting
//! message the C++ ABI shim prints.

pub mod abi;
pub mod audio;
pub mod bytes;
pub mod hle;
pub mod memory_map;
pub mod output_gain;
pub mod pcm_ring;
pub mod shared_mailbox;
pub mod snapshot;
pub mod worker_queue;

/// The message of a caught panic payload, which is what the C++ ABI layer printed from `what()`.
pub(crate) fn panic_message(payload: &(dyn std::any::Any + Send)) -> String {
    if let Some(message) = payload.downcast_ref::<&str>() {
        (*message).to_owned()
    } else if let Some(message) = payload.downcast_ref::<String>() {
        message.clone()
    } else {
        "unknown exception".to_owned()
    }
}
