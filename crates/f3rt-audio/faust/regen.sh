#!/usr/bin/env bash
# Regenerates crates/f3rt-audio/src/hle/faust/generated/*.rs from the .dsp sources next to this file,
# with the same DSPs, the same class names and the same -double option as the f3rt_faust_dsp() calls
# in CMakeLists.txt, but through the Rust backend and faust/architecture.rs instead of
# runtime/audio/hle/faust/architecture.hpp.
#
# The version matters: the checked-in C++ headers were produced by FAUST 2.88.0, whose code
# scheduling the Rust modules must match for the audio to stay bit-exact. FAUST 2.85.9, for
# example, emits different floating-point expression orders. Set FAUST to a 2.88.0 binary, e.g.
#   FAUST=/path/to/faust-2.88.0/build/bin/faust crates/f3rt-audio/faust/regen.sh
set -euo pipefail

faust=${FAUST:-faust}
expected=2.88.0

if ! version=$("$faust" --version 2>/dev/null | head -1); then
    echo "regen: '$faust' is not runnable; set FAUST to a FAUST $expected binary" >&2
    exit 1
fi
if [[ $version != *" $expected" ]]; then
    echo "regen: '$faust' reports '$version', but the checked-in modules require FAUST $expected" >&2
    exit 1
fi

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
cd "$root"

# name:class, the DSP list and class names of the f3rt_faust_dsp() registrations.
dsp() {
    local name=$1 class=$2
    echo "faust: $name"
    "$faust" -lang rust \
        -a crates/f3rt-audio/faust/architecture.rs \
        -cn "$class" -double \
        -o "crates/f3rt-audio/src/hle/faust/generated/$name.rs" \
        "crates/f3rt-audio/faust/$name.dsp"
}

dsp reverb ReverbDsp
dsp chorus ChorusDsp
dsp echo EchoDsp
dsp hall HallDsp
dsp dc_blocker DcBlockerDsp
