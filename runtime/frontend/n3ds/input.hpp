#pragma once
#include "f3rt/input.hpp"
#include <array>
#include <cstdint>

namespace f3rt::n3ds {
// One hid key mask per local control, in input.hpp bit order:
// U, D, L, R, B1, B2, B3, start, coin, service, test, B4, B5, B6.
using ControlKeys = std::array<uint32_t, LOCAL_CONTROL_COUNT>;

// Physical buttons plus the circle-pad pseudo keys, as hidKeysHeld() reports them.
ControlKeys default_keys();
// Pure mapping so it can be exercised without hid: any pressed mask sets its control bit.
LocalInputWord map_input(uint32_t held, const ControlKeys &keys);
} // namespace f3rt::n3ds
