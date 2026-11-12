//! Mechanical Rust translation of the hardware-accurate reference audio backend
//! (`runtime/audio/reference/` and the MAME chip cores in `runtime/third_party/audio/`).
//! Test-only: not linked into the C++ build.

pub mod board;
pub mod engine;
pub mod es5505;
pub mod es5510;
pub mod mb87078;
pub mod mc68681;
pub mod state_io;

#[cfg(test)]
mod tests;
