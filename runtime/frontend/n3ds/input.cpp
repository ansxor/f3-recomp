#include "input.hpp"
#include <3ds.h>

namespace f3rt::n3ds {
ControlKeys default_keys() {
    return {uint32_t(KEY_DUP | KEY_CPAD_UP),     uint32_t(KEY_DDOWN | KEY_CPAD_DOWN),
            uint32_t(KEY_DLEFT | KEY_CPAD_LEFT), uint32_t(KEY_DRIGHT | KEY_CPAD_RIGHT),
            uint32_t(KEY_A),                     uint32_t(KEY_B),
            uint32_t(KEY_Y),                     uint32_t(KEY_START),
            uint32_t(KEY_SELECT),                0,
            0,                                   uint32_t(KEY_X),
            uint32_t(KEY_L),                     uint32_t(KEY_R)};
}
LocalInputWord map_input(uint32_t held, const ControlKeys &keys) {
    LocalInputWord word = 0;
    for (unsigned control = 0; control < keys.size(); ++control)
        if (keys[control] && (held & keys[control]))
            word = LocalInputWord(word | (LocalInputWord(1) << control));
    return word;
}
} // namespace f3rt::n3ds
