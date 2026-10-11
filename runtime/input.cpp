#include "f3rt/input.hpp"
#include "f3rt/machine.hpp"

namespace f3rt {
void apply_local_inputs(Machine &m,
                        const std::array<LocalInputWord, local_player_count> &words) {
    const bool dial = m.roms.name == "arkretrnj" || m.roms.name == "puchicarj";
    const bool kaiser = m.roms.name == "kaiserknj";
    for (unsigned port = 0; port < m.inputs.size(); ++port) {
        if (!dial || (port != 2 && port != 3)) m.inputs[port] = 0xffffffff;
    }
    m.system_inputs = 0xff;
    const unsigned players = kaiser ? 2 : local_player_count;
    for (unsigned slot = 0; slot < players; ++slot) {
        const LocalInputWord word = words[slot];
        const unsigned shift = (slot & 1) * 4;
        m.set_input(slot < 2 ? 1 : 5, uint32_t(word & 0xf) << shift, true);
        uint32_t buttons = (word >> 4) & 7;
        if (!kaiser) buttons |= uint32_t((word >> 11) & 1) << 3;
        m.set_input(slot < 2 ? 0 : 4, buttons << (shift + (slot < 2 ? 0 : 8)), true);
        if (kaiser)
            m.set_input(slot == 0 ? 5 : 4, uint32_t((word >> 11) & 7) << (slot * 8), true);
        m.set_input(0, 0x1000u << slot, word & 0x80);
        if (slot < 3) m.set_input(0, 0x200u << slot, word & 0x200);
        if (word & 0x100) m.system_inputs &= uint8_t(~(0x10u << slot));
        if (word & 0x400) m.system_inputs &= uint8_t(~2u);

        if (dial && slot < 2) {
            const bool left = word & 4, right = word & 8;
            if (left != right) {
                // MAME f3_analog_r: the low counter nibble is on bits 12..15.
                const uint32_t raw = m.inputs[2 + slot];
                uint32_t counter = ((raw >> 12) & 0xf) | ((raw & 0xff) << 4);
                // Native Ark's ROR.W #8 yields counter << 4; two counts match
                // its 32-unit joystick step. The guest still chooses its mode.
                counter = (counter + (right ? 2u : 0u) - (left ? 2u : 0u)) & 0xfff;
                m.inputs[2 + slot] = 0xffff0000 | ((counter & 0xf) << 12) |
                                     ((counter & 0xff0) >> 4);
            }
        }
    }
}
}
