// Kirameki Star Road (kirameki) game-specific video code.
//
// - video_writer_known: known store-PC lists per scene component, used by
//   `--discovery-log` to suppress documented writers.
//
// The sprite, tile, text and line-RAM decoders are shared and live in runtime/.
#include "debug/discovery.hpp"
#include "f3rt/emit_unit.hpp"
#include <cstdint>

namespace f3rt {

namespace {

// Boot clear and initialization store PCs
constexpr uint32_t KIRAMEKI_PC_BOOT_CONTROL_CLEAR_15C = 0x15c;
constexpr uint32_t KIRAMEKI_PC_BOOT_VRAM_CLEAR_16E = 0x16e;
constexpr PcRange KIRAMEKI_PC_BOOT_LINERAM_INIT{0x180, 0x1d0};

// Control registers (0x660000..0x66001a) scroll updates
constexpr PcRange KIRAMEKI_PC_CONTROL_SCROLL_UPDATES{0x630, 0x6e0};

// Sprite list compilers and command writers
constexpr PcRange KIRAMEKI_PC_SPRITES_COMPILER_300{0x390, 0x430};
constexpr uint32_t KIRAMEKI_PC_SPRITES_COMMAND_5BC = 0x5bc;
constexpr PcRange KIRAMEKI_PC_SPRITES_COMPILER_4600{0x4650, 0x46a0};
constexpr PcRange KIRAMEKI_PC_SPRITES_COMPILER_13200{0x13200, 0x13230};

// Text RAM writers
constexpr uint32_t KIRAMEKI_PC_TEXT_CLEAR_461E = 0x461e;
constexpr PcRange KIRAMEKI_PC_TEXT_STRINGS_4C00{0x4c00, 0x4e00};
constexpr PcRange KIRAMEKI_PC_TEXT_STRINGS_13F00{0x13fe0, 0x14120};

// Playfield tile copy and clear routines
constexpr PcRange KIRAMEKI_PC_TILES_COPY_13600{0x13600, 0x13630};
constexpr PcRange KIRAMEKI_PC_TILES_COPY_13A00{0x13a80, 0x13af0};

// Line RAM scanline effect writers
constexpr PcRange KIRAMEKI_PC_LINES_EFFECT_3A400{0x3a4c0, 0x3a570};

bool sprites_covered_write(uint32_t pc) {
    if (pc == KIRAMEKI_PC_BOOT_VRAM_CLEAR_16E || pc == KIRAMEKI_PC_SPRITES_COMMAND_5BC)
        return true;
    return contains(KIRAMEKI_PC_SPRITES_COMPILER_300, pc) ||
           contains(KIRAMEKI_PC_SPRITES_COMPILER_4600, pc) ||
           contains(KIRAMEKI_PC_SPRITES_COMPILER_13200, pc);
}

bool tiles_covered_write(uint32_t pc) {
    if (pc == KIRAMEKI_PC_BOOT_VRAM_CLEAR_16E) return true;
    return contains(KIRAMEKI_PC_TILES_COPY_13600, pc) ||
           contains(KIRAMEKI_PC_TILES_COPY_13A00, pc);
}

bool text_covered_write(uint32_t pc) {
    if (pc == KIRAMEKI_PC_BOOT_VRAM_CLEAR_16E || pc == KIRAMEKI_PC_TEXT_CLEAR_461E)
        return true;
    return contains(KIRAMEKI_PC_TEXT_STRINGS_4C00, pc) ||
           contains(KIRAMEKI_PC_TEXT_STRINGS_13F00, pc);
}

bool lines_covered_write(uint32_t pc) {
    if (pc == KIRAMEKI_PC_BOOT_VRAM_CLEAR_16E) return true;
    return contains(KIRAMEKI_PC_BOOT_LINERAM_INIT, pc) ||
           contains(KIRAMEKI_PC_LINES_EFFECT_3A400, pc);
}

bool control_covered_write(uint32_t pc) {
    if (pc == KIRAMEKI_PC_BOOT_CONTROL_CLEAR_15C) return true;
    return contains(KIRAMEKI_PC_CONTROL_SCROLL_UPDATES, pc);
}

} // namespace

bool video_writer_known(VideoLayer layer, uint32_t pc) {
    switch (layer) {
    case VideoLayer::Control:
        return control_covered_write(pc);
    case VideoLayer::Sprites:
        return sprites_covered_write(pc);
    case VideoLayer::Pf0:
    case VideoLayer::Pf1:
    case VideoLayer::Pf2:
    case VideoLayer::Pf3:
    case VideoLayer::Pf2Alt:
    case VideoLayer::Pf3Alt:
        return tiles_covered_write(pc);
    case VideoLayer::Text:
        return text_covered_write(pc);
    case VideoLayer::Lines:
        return lines_covered_write(pc);
    default:
        return false;
    }
}

} // namespace f3rt
