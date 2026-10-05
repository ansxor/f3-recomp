#pragma once
#include <array>
#include <cstdint>

namespace f3rt {
class Machine;
// Active-high U/D/L/R, B1/B2/B3, start, coin, service, test, then B4/B5/B6.
using LocalInputWord = uint16_t;
constexpr unsigned local_player_count = 4;
constexpr unsigned local_control_count = 14;
constexpr LocalInputWord local_input_mask = 0x3fff;
// Apply once per logical input frame; dial counters live in the machine snapshot.
void apply_local_inputs(Machine &, const std::array<LocalInputWord, local_player_count> &);
}
