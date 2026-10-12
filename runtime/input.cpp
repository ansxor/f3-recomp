#include "f3rt/input.hpp"
#include "f3rt/machine.hpp"

namespace f3rt {
namespace {

// Bit positions in LocalInputWord (active high). Directions and buttons are contiguous runs.
namespace control {
constexpr unsigned DIRECTIONS = 0; // up, down, left, right: bits 0..3
constexpr unsigned LEFT = 2, RIGHT = 3;
constexpr unsigned BUTTONS_1_3 = 4; // bits 4..6
constexpr unsigned START = 7, COIN = 8, SERVICE = 9, TEST = 10;
constexpr unsigned BUTTONS_4_6 = 11; // bits 11..13
} // namespace control

bool held(LocalInputWord word, unsigned bit) { return (word >> bit) & 1; }
uint32_t bits(LocalInputWord word, unsigned first, unsigned count) {
    return (word >> first) & ((1u << count) - 1);
}

// Machine::inputs[i] is the IO word at 0x4a0000 + 4*i. Its low half holds active-low input lines.
namespace port {
constexpr unsigned BUTTONS_P12 = 0;  // P1/P2 buttons (nibbles 0/1), service 1-3, start 1-4
constexpr unsigned JOYSTICK_P12 = 1; // P1/P2 directions (nibbles 0/1)
constexpr unsigned DIAL_P1 = 2;      // dial ports carry counters, not lines
constexpr unsigned DIAL_P2 = 3;
constexpr unsigned BUTTONS_P34 = 4;  // P3/P4 buttons (nibbles 2/3); six-button: P2 buttons 4-6
constexpr unsigned JOYSTICK_P34 = 5; // P3/P4 directions (nibbles 0/1); six-button: P1 buttons 4-6
constexpr uint32_t RELEASED = 0xffffffff;
} // namespace port

// Line numbers inside port::buttons_p12, indexed by player.
constexpr unsigned FIRST_SERVICE_LINE = 9; // lines 9..11; there is no fourth service line
constexpr unsigned SERVICE_LINE_COUNT = 3;
constexpr unsigned FIRST_START_LINE = 12; // lines 12..15

// Machine::system_inputs (bit 0 is EEPROM data out and never driven here).
namespace system_line {
constexpr unsigned TEST = 1;
constexpr unsigned FIRST_COIN = 4; // bits 4..7, indexed by player
constexpr uint8_t RELEASED = 0xff;
} // namespace system_line

// Each player's directions or buttons occupy one 4-line nibble of a port.
struct Nibble {
    unsigned port, shift;
};
Nibble joystick_nibble(unsigned player) {
    return {player < 2 ? port::JOYSTICK_P12 : port::JOYSTICK_P34, (player & 1) * 4};
}
Nibble button_nibble(unsigned player) {
    return {player < 2 ? port::BUTTONS_P12 : port::BUTTONS_P34, player * 4};
}
// Six-button cabinets have no P3/P4, so P1/P2 buttons 4-6 reuse their lines.
Nibble extra_button_nibble(unsigned player) {
    return player == 0 ? joystick_nibble(2) : button_nibble(2);
}
void press(Machine &m, Nibble n, uint32_t pressed) {
    m.set_input(n.port, pressed << n.shift, true);
}
void press_system(Machine &m, unsigned bit) { m.system_inputs &= uint8_t(~(1u << bit)); }

// MAME f3_analog_r packing of the 12-bit dial counter: counter bits 0..3 on lines 12..15,
// counter bits 4..11 on lines 0..7, upper half all ones.
constexpr uint32_t DIAL_COUNTER_MASK = 0xfff;
// Guest ROR.W #8 yields counter << 4; two counts per frame match its 32-unit joystick step.
// The guest still chooses its own mode.
constexpr uint32_t DIAL_STEP = 2;
uint32_t unpack_dial(uint32_t word) { return ((word >> 12) & 0xf) | ((word & 0xff) << 4); }
uint32_t pack_dial(uint32_t counter) {
    return 0xffff0000 | ((counter & 0xf) << 12) | (counter >> 4);
}
void turn_dial(uint32_t &word, bool left, bool right) {
    if (left == right) return;
    const uint32_t counter = unpack_dial(word);
    word = pack_dial((right ? counter + DIAL_STEP : counter - DIAL_STEP) & DIAL_COUNTER_MASK);
}

} // namespace

void apply_local_inputs(Machine &m, const std::array<LocalInputWord, LOCAL_PLAYER_COUNT> &words) {
    const bool dial = m.roms.input.dial;
    const bool six_button = m.roms.input.six_button;

    // Start from all lines released; dial counters persist across frames.
    for (unsigned p = 0; p < m.inputs.size(); ++p) {
        const bool dial_port = p == port::DIAL_P1 || p == port::DIAL_P2;
        if (!(dial && dial_port)) m.inputs[p] = port::RELEASED;
    }
    m.system_inputs = system_line::RELEASED;

    const unsigned players = six_button ? 2 : LOCAL_PLAYER_COUNT;
    for (unsigned player = 0; player < players; ++player) {
        const LocalInputWord word = words[player];

        press(m, joystick_nibble(player), bits(word, control::DIRECTIONS, 4));

        uint32_t buttons = bits(word, control::BUTTONS_1_3, 3);
        if (six_button) press(m, extra_button_nibble(player), bits(word, control::BUTTONS_4_6, 3));
        else buttons |= uint32_t(held(word, control::BUTTONS_4_6)) << 3;
        press(m, button_nibble(player), buttons);

        if (held(word, control::START))
            m.set_input(port::BUTTONS_P12, 1u << (FIRST_START_LINE + player), true);
        if (player < SERVICE_LINE_COUNT && held(word, control::SERVICE))
            m.set_input(port::BUTTONS_P12, 1u << (FIRST_SERVICE_LINE + player), true);
        if (held(word, control::COIN)) press_system(m, system_line::FIRST_COIN + player);
        if (held(word, control::TEST)) press_system(m, system_line::TEST);

        if (dial && player < 2)
            turn_dial(m.inputs[port::DIAL_P1 + player], held(word, control::LEFT),
                      held(word, control::RIGHT));
    }
}
} // namespace f3rt
