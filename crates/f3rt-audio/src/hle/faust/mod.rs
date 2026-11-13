//! `runtime/audio/hle/faust/`: the FAUST-generated DSPs and the control registry of support.hpp.
//!
//! `generated/*.rs` are checked in and produced by faust/regen.sh from the same `.dsp` sources and
//! class names the C++ build uses, with the Rust backend (`-lang rust`) and architecture.rs. The
//! re-export makes `use super::*` inside a generated module resolve the FAUST interfaces, as
//! architecture.hpp's `#include "support.hpp"` does for the C++ headers.

pub mod support;
pub use support::*;

pub mod generated {
    pub use super::*;

    pub mod chorus;
    pub mod dc_blocker;
    pub mod echo;
    pub mod hall;
    pub mod reverb;
}
