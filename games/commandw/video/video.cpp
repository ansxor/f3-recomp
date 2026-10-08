// Command War (commandw) game-specific video code.
//
// - With F3RT_VIDEO_WRITE_LOG: observe_game_video_write. No store-PC lists are
//   documented for this game yet, so every writer is reported (once per PC).
//
// The sprite, tile, text and line-RAM decoders are shared and live in runtime/.
#ifdef F3RT_VIDEO_WRITE_LOG
#include "renderer/game/video_log.hpp"
#include <array>
#endif

namespace f3rt {

#ifdef F3RT_VIDEO_WRITE_LOG
void observe_game_video_write(uint32_t pc, uint32_t address, uint64_t frame) {
    constexpr uint32_t graphics_base = 0x600000, pf_begin = 0x10000, text_begin = 0x1c000;
    if (address >= 0x660000 && address < 0x660040) {
        log_unknown_video_write("control", pc, address, frame);
        return;
    }
    if (address < graphics_base || address >= graphics_base + 0x40000) return;
    const uint32_t offset = address - graphics_base;
    if (offset < pf_begin) log_unknown_video_write("sprites", pc, address, frame);
    else if (offset < text_begin) {
        constexpr std::array<const char *, 6> maps{"pf0", "pf1", "pf2", "pf3", "pf2-alt", "pf3-alt"};
        log_unknown_video_write(maps[(offset - pf_begin) / 0x2000], pc, address, frame);
    } else if (offset < 0x20000) log_unknown_video_write("text", pc, address, frame);
    else log_unknown_video_write("lines", pc, address, frame);
}
#endif

} // namespace f3rt

