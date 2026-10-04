#include "f3rt/game_video.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/video.hpp"
#include "game_tiles.hpp"
#include <array>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace f3rt {
struct GameVideo::Impl {
    explicit Impl(Machine &value) : machine(value) {}
    Machine &machine;
    GameTiles tiles;
    std::array<uint64_t, 4> frames{}, mismatches{};
};

GameVideo::GameVideo(Machine &machine) : impl_(std::make_unique<Impl>(machine)) { reset(); }
GameVideo::~GameVideo() = default;
void GameVideo::reset() {
    impl_->tiles.reset();
    impl_->frames.fill(0);
    impl_->mismatches.fill(0);
}
void GameVideo::observe() {
    auto &m = impl_->machine;
    GameMemory memory{m.roms.main, m.ram};
    impl_->tiles.observe(memory, m.cpu);
}
void GameVideo::observe_write(uint32_t pc, uint32_t address) {
    impl_->tiles.observe_write(pc, address);
}

void GameVideo::compare_playfields(uint64_t frame, unsigned layer_mask) {
    if (!layer_mask || (layer_mask & ~15u)) throw std::runtime_error("Video layer mask must select PF0..PF3 (bits 0..3)");
    auto &m = impl_->machine;
    const auto assets = m.video->playfield_tiles();
    for (unsigned layer = 0; layer < 4; ++layer) {
        if (!(layer_mask & (1u << layer))) continue;
        if (!impl_->tiles.supported(layer)) {
            std::ostringstream error;
            error << "Game PF" << layer << " unsupported producer at frame " << frame
                  << " PC 0x" << std::hex << impl_->tiles.unsupported_pc(layer);
            throw std::runtime_error(error.str());
        }
        uint64_t mismatches = 0;
        int first_x = -1, first_y = -1;
        for (int y = 0; y < 512; ++y) {
            const auto oracle = m.video->inspect_playfield_line(layer, y, m.graphics);
            for (int x = 0; x < 1024; ++x) {
                const auto game = impl_->tiles.playfield_pixel(layer, x, y, m.video->flipscreen(), assets);
                const bool game_visible = (game.flags & 0x10) != 0;
                const bool oracle_visible = (oracle.flags[x] & 0x10) != 0;
                if (game_visible != oracle_visible || (game_visible &&
                    (game.palette != oracle.palette[x] || game.flags != oracle.flags[x]))) {
                    if (!mismatches) { first_x = x; first_y = y; }
                    ++mismatches;
                }
            }
        }
        ++impl_->frames[layer];
        impl_->mismatches[layer] += mismatches;
        if (mismatches) {
            std::ostringstream error;
            error << "Game PF" << layer << " frame " << frame << ": " << mismatches
                  << " indexed pixel mismatches; first (" << first_x << ',' << first_y << ')';
            throw std::runtime_error(error.str());
        }
    }
}

void GameVideo::report(std::ostream &output) const {
    for (unsigned layer = 0; layer < 4; ++layer) {
        if (!impl_->frames[layer]) continue;
        output << "VIDEO layer=pf" << layer << " domain=1024x512-indexed-texture"
               << " sampled_frames=" << impl_->frames[layer]
               << " compared_pixels=" << impl_->frames[layer] * 1024 * 512
               << " pixel_mismatches=" << impl_->mismatches[layer] << '\n';
    }
}
} // namespace f3rt

extern "C" void f3_landmakr_video_hook(f3_cpu *cpu) {
    auto &machine = *static_cast<f3rt::Machine *>(cpu->runtime);
    if (machine.game_video) machine.game_video->observe();
}
