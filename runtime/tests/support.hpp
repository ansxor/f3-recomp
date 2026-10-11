#pragma once
#include "f3rt/machine.hpp"

namespace f3test {
// Minimal ROM set: SSP 41fff0, PC 100, `moveq #42,d0; bra self`, one OTIS sample word at 0x12345.
f3rt::RomSet fixture();
}
