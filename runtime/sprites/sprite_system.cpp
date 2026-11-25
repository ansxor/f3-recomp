#include "sprites/sprite_system.hpp"
#include "f3rt/machine.hpp"
#include "sprites/enhancements.hpp"
#include <ostream>

#if __has_include("sprite_groups.hpp")
#include "sprite_groups.hpp"
#else
namespace f3rt::sprite_groups {
inline constexpr SpriteGroupTable table{};
}
#endif

namespace f3rt {

const SpriteGroupTable &SpriteSystem::game_table() { return sprite_groups::table; }

std::unique_ptr<SpriteSystem> make_game_sprites(Machine &machine) {
    const EnhancementSet enhancements;
    return make_game_sprites(machine, enhancements);
}

std::unique_ptr<SpriteSystem> make_game_sprites(Machine &machine,
                                                const EnhancementSet &enhancements) {
    const auto &group_table = SpriteSystem::game_table();
    if (group_table.writes.empty()) return nullptr;
    SpriteSystem::Options options;
    options.full_detail = enhancements.enabled("full-detail", VideoBackend::Cpu) ||
                          enhancements.enabled("full-detail", VideoBackend::Gpu);
    return std::make_unique<SpriteSystem>(machine, group_table, options);
}

SpriteSystem::SpriteSystem(Machine &machine, const SpriteGroupTable &table, Options options)
    : m_tracker(machine, table) {
    if (options.full_detail) m_tracker.enable_commandw_full_detail();
}

void SpriteSystem::note_write(uint32_t address, unsigned width) {
    m_tracker.note_write(address, width);
}

SpritePresentation SpriteSystem::presentation() {
    m_tracker.count_ambiguity();
    return {m_tracker.groups_active() ? m_tracker.slots() : std::span<const SpriteGroupSlot>{}};
}

void SpriteSystem::frame_end() { m_tracker.frame_end(); }

void SpriteSystem::reset() { m_tracker.reset(); }

}
