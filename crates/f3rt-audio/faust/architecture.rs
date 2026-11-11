// FAUST Rust architecture for f3rt DSP modules. faust/regen.sh runs:
//   faust -lang rust -a crates/f3rt-audio/faust/architecture.rs -cn <Class> [-double] <file>.dsp
// The generated module lives in crate::hle::faust::generated and takes the FAUST interfaces
// (FaustDsp, UI, Meta, FaustFloat, F32, F64, ParamIndex) from crate::hle::faust::support, mirroring
// runtime/audio/hle/faust/architecture.hpp + support.hpp.
//
// `use super::*` is the Rust counterpart of the C++ architecture's `#include "support.hpp"`: the
// generated file is declared in hle::faust::generated, which re-exports hle::faust's items.
#![allow(non_snake_case)]
#![allow(non_camel_case_types)]
#![allow(dead_code)]
#![allow(unused_variables)]
#![allow(unused_mut)]
#![allow(unused_parens)]
#![allow(non_upper_case_globals)]
// The generated body is machine output; its lints are suppressed the way architecture.hpp pushes
// and pops -Wunused-parameter around the class body.
#![allow(clippy::all)]

use super::*;

<<includeIntrinsic>>

<<includeclass>>
