// Land Maker (landmakrj) game-specific video code.
//
// - With F3RT_VIDEO_WRITE_LOG: the known store-PC lists per scene component
//   (observe_game_video_write).
//
// The sprite, tile, text and line-RAM decoders are shared and live in runtime/.
#ifdef F3RT_VIDEO_WRITE_LOG
#include "renderer/game/video_log.hpp"
#include <array>
#endif

namespace f3rt {

#ifdef F3RT_VIDEO_WRITE_LOG
namespace {

constexpr uint32_t pf_begin = 0x10000; // PF0 data

// Land Maker's tile-block copy/erase loops and the per-layer clear stores.
bool tiles_covered_write(uint32_t pc) {
    switch (pc) {
    case 0x55fc: case 0x5646: case 0x56d6:
    case 0x5a2e: case 0x5a6a: case 0x5aa6: case 0x5b00:
    case 0x9bd08: case 0x9bd0a: case 0x9bd0c: case 0x9bd0e:
    case 0x9ec66: case 0x9ec6a: case 0x9ec6e: case 0x9ec72:
        return true;
    default:
        return false;
    }
}

bool text_covered_write(uint32_t pc) {
    switch (pc) {
    case 0x570e: case 0x5712: case 0x5756: case 0x5758: case 0x578e:
    case 0x57bc: case 0x57be: case 0x580c: case 0x580e:
    case 0x583a: case 0x5840: case 0x5846: case 0x58c2:
    case 0x59d0: case 0x5a08: case 0x5bf8: case 0x5c20:
    case 0x5b8a: case 0x5b9a: case 0x5bb6: case 0x5bc6: case 0x5bd8:
    case 0x8de60: case 0x8e0b2: case 0x8e0c2: case 0x8e0e6: case 0x8e0f4:
    case 0x8e9d2: case 0x9b53a: case 0x9b562: case 0x9b564: case 0x9b586:
    case 0xa1176: case 0xa117c: case 0xa1184: case 0xa118c:
    case 0xa1194: case 0xa119c: case 0xa11a4: case 0xa11ac:
        return true;
    default:
        return false;
    }
}

// Store PCs Land Maker uses to build the sprite display list. Ranges mirror the
// retired hook coverage: init_sprites, clear_spriteram, sprite_scroll,
// sprite_command, list terminator and the single/grid/scaled/object compilers.
bool sprites_covered_write(uint32_t pc) {
    if (pc >= 0x41d0 && pc <= 0x4380) return true;
    if (pc >= 0x43b0 && pc <= 0x43de) return true; // sprite_scroll
    if (pc >= 0x43e0 && pc <= 0x43fe) return true; // sprite_command
    if (pc >= 0x4422 && pc <= 0x447e) return true; // list terminator
    if (pc >= 0x4688 && pc <= 0x46be) return true; // single sprite compiler
    if (pc >= 0x46c0 && pc <= 0x480a) return true; // grid sprite compiler
    if (pc >= 0x480c && pc <= 0x4a36) return true; // scaled grid compiler
    if (pc >= 0xa8f38 && pc <= 0xa8f82) return true; // obj_grid
    if (pc >= 0xa8f84 && pc <= 0xa8fce) return true; // obj_3tile
    if (pc >= 0xa9036 && pc <= 0xa9076) return true; // obj_grid flip_x
    if (pc >= 0xa90f4 && pc <= 0xa913a) return true; // obj_4tile
    if (pc >= 0xa913c && pc <= 0xa93a2) return true; // obj_scaled
    return false;
}

bool lines_covered_write(uint32_t pc) {
    static constexpr struct Range { uint32_t start, end; } ranges[] = {
        {0x00136e, 0x00145e}, // Display register uploader (0x660000..0x66001e)
        {0x005a2e, 0x005a42}, // PF0 clear rowscroll (0x62a000)
        {0x005a6a, 0x005a7e}, // PF1 clear rowscroll (0x62a200)
        {0x005aa6, 0x005ad8}, // PF2 clear rowscroll, zoom, colscroll
        {0x005b00, 0x005b32}, // PF3 clear rowscroll, zoom, colscroll
        {0x005d30, 0x005d6c}, // Line RAM profile init (0x624000..0x62b000, 0x620000)
        {0x010044, 0x01007c}, // Boot control registers (0x660000..0x66001a)
        {0x0100ba, 0x0100c4}, // Boot line RAM clear
        {0x08cfd2, 0x08cfda}, // Alpha blend save (0x626200)
        {0x08cff2, 0x08cff8}, // Alpha blend restore (0x626200)
        {0x0914b8, 0x0914c2}, // Water effect select init
        {0x09152e, 0x091538}, // Water effect select init
        {0x0915dc, 0x091610}, // Water effect main lines
        {0x09187a, 0x091882}, // Water clip window animation
        {0x09218a, 0x09218a}, // Selection transition sprite priorities
        {0x098dc8, 0x098dc8}, // Full-screen alpha profile (0x626200)
        {0x099b72, 0x099b74}, // Attract sprite modes and priorities
        {0x099fc2, 0x099fca}, // Attract reverse/normal blend transition
        {0x09a25c, 0x09a25c}, // Attract alpha fade from task D2
        {0x09a2ac, 0x09a2b0}, // Attract PF1/PF3 blend restoration
        {0x09a300, 0x09a300}, // Second attract alpha fade from task D2
        {0x09a6f4, 0x09a6f4}, // Attract PF2 blend/priority (0x62b400)
        {0x09a8ec, 0x09a8ec}, // Attract sprite priorities (0x627600)
        {0x09accc, 0x09accc}, // Attract PF2 blending on
        {0x09ad4c, 0x09ad4c}, // Attract PF2 blending off
        {0x09d684, 0x09d6a0}, // Gameboard PF2 palette add gradient
        {0x09d75e, 0x09d7aa}, // Gameboard PF2 zoom & rowscroll
        {0x09d7d6, 0x09d812}, // Gameboard PF2 column scroll
        {0x09ecde, 0x09ecee}, // PF0 wavy rowscroll (0x62a000)
    };
    for (const auto &r : ranges)
        if (pc >= r.start && pc <= r.end) return true;
    return false;
}

} // namespace

void observe_game_video_write(uint32_t pc, uint32_t address, uint64_t frame) {
    constexpr uint32_t graphics_base = 0x600000, pf_end = pf_begin + 4 * 0x2000;
    if (address >= 0x600000 && address < 0x610000) {
        if (!sprites_covered_write(pc)) log_unknown_video_write("sprites", pc, address, frame);
    } else if (address >= graphics_base + pf_begin && address < graphics_base + pf_end) {
        constexpr std::array<const char *, 4> layer_names{"pf0", "pf1", "pf2", "pf3"};
        if (!tiles_covered_write(pc))
            log_unknown_video_write(layer_names[(address - graphics_base - pf_begin) / 0x2000], pc, address, frame);
    } else if (address >= 0x61c000 && address < 0x620000) {
        if (!text_covered_write(pc)) log_unknown_video_write("text", pc, address, frame);
    } else if ((address >= 0x620000 && address < 0x630000) || (address >= 0x660000 && address < 0x660040)) {
        if (!lines_covered_write(pc)) log_unknown_video_write("lines", pc, address, frame);
    }
}
#endif

} // namespace f3rt

