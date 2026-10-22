#pragma once

// Per-game description of how the main CPU turns a sound code into mailbox packets.
//
// Shared tool code never names a game: each game supplies its profile in
// games/<game>/sound/profile.cpp, which registers a SoundProfile for its ROM set id. The tools
// look a profile up by the set they run.

#include <array>
#include <cstdint>
#include <string_view>

namespace f3rt::runner {

// All addresses are main-CPU ROM/RAM addresses of routines and variables the game itself uses.
// A zero address means the game has no such step.
struct SoundProfile {
    std::string_view set;   // ROM set id (RomSet::name)
    std::string_view title; // display name of the game
    // Highest code the request routine accepts (higher codes produce no packets). Codes 1..last_code are
    // the sound test's code space.
    uint32_t last_code;
    // void request(word code): word argument on the stack, writes packets to the ring buffer.
    uint32_t request;
    // d0 = code -> d1 = category; the sound test runs the prelude for categories <= 1.
    uint32_t category;
    // d0 = code -> d0, d1 = the two setup codes the sound test requests before the code.
    uint32_t prelude;
    // RAM longword holding the base of the selector pointer table (one descriptor pointer per code).
    uint32_t selector_base_ptr;
    // RAM words the sound test keeps its BANK value in, written in this order after the bank
    // switch itself.
    std::array<uint32_t, 2> bank_words;
};

// Registers a profile at static initialization; `profile` must outlive the process.
class SoundProfileRegistrar {
public:
    explicit SoundProfileRegistrar(const SoundProfile &profile);
};

// nullptr when no registered profile has this set id.
const SoundProfile *find_sound_profile(std::string_view set);

} // namespace f3rt::runner
