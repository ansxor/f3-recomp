#pragma once
#include "f3rt/input.hpp"
#include <array>
#include <cstdint>

namespace f3rt::n3ds {
using ControlKeys = std::array<uint32_t, LOCAL_CONTROL_COUNT>;

ControlKeys default_keys();
LocalInputWord map_input(uint32_t held, const ControlKeys &keys);
} // namespace f3rt::n3ds
