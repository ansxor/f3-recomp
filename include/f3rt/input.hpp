#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace f3rt {
class Machine;
// Active-high U/D/L/R, B1/B2/B3, start, coin, service, test, then B4/B5/B6.
using LocalInputWord = uint16_t;
constexpr unsigned local_player_count = 4;
constexpr unsigned local_control_count = 14;
constexpr LocalInputWord local_input_mask = 0x3fff;
// Apply once per logical input frame; dial counters live in the machine snapshot.
void apply_local_inputs(Machine &, const std::array<LocalInputWord, local_player_count> &);

// Scripted local input (`--inputs FILE`): host-only, deterministic in the emulated frame number.
// One rule per line, `#` comments; frames are 1-based like dumps and the discovery log, ranges inclusive:
//   FRAME[-END|+COUNT] [pN] CONTROL[+CONTROL...]              hold controls (up down left right b1..b6
//                                                              start coin service test) for those frames
//   FRAME[-END|+COUNT] [pN] mash [seed=N] [period=N] [keys=A+B] seeded random presses/releases, the
//                                                              gameplay-regression LCG (defaults: seed 1,
//                                                              period 6, keys up..b3)
//   FRAME[-END|+COUNT] poke ADDR.b|.w|.l=VALUE               write main RAM (0x400000..0x41ffff, big-endian)
//                                                              at the start of each of those frames (a range
//                                                              rewrites it every frame; the game may still
//                                                              overwrite it during the frame)
// END may be `end`. Active rules are OR'd per player; numbers are decimal or 0x hex.
class InputScript {
public:
    // Throws std::runtime_error naming `source` and the line on malformed input.
    static InputScript parse(std::string_view text, const std::string &source);
    static InputScript load(const std::string &path);
    // Words for emulated frame `frame` (1-based: Machine::frame + 1 before run_frame).
    // Mash state advances with the frame; going backwards replays from the start.
    std::array<LocalInputWord, local_player_count> words(uint64_t frame);
    // Applies the poke rules active at `frame` to main RAM (call right before run_frame).
    void poke(Machine &machine, uint64_t frame) const;
    size_t rules() const { return rules_.size(); }

private:
    struct Rule {
        uint64_t begin = 0, end = 0;
        unsigned player = 0;
        LocalInputWord hold = 0;
        bool mash = false;
        unsigned poke_size = 0; // bytes; 0 = not a poke
        uint32_t poke_address = 0, poke_value = 0;
        uint64_t seed = 1, rng = 1, next = 0; // next: first frame whose mash step has not run
        unsigned period = 6;
        std::vector<unsigned> keys;
        LocalInputWord held = 0;
    };
    std::vector<Rule> rules_;
};
}
