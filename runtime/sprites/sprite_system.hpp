#pragma once
#include "f3rt/sprite_group.hpp"
#include "sprites/groups.hpp"
#include "sprites/presentation.hpp"
#include <cstdint>
#include <iosfwd>
#include <memory>

namespace f3rt {
class EnhancementSet;
class Machine;

class SpriteSystem {
public:
    struct Options {
        bool full_detail = false;
    };

    SpriteSystem(Machine &machine, const SpriteGroupTable &table)
        : SpriteSystem(machine, table, Options{}) {}
    SpriteSystem(Machine &machine, const SpriteGroupTable &table, Options options);
    SpriteSystem(const SpriteSystem &) = delete;
    SpriteSystem &operator=(const SpriteSystem &) = delete;

    static const SpriteGroupTable &game_table();

    bool active() const { return m_tracker.groups_active(); }
    bool groups_active() const { return m_tracker.groups_active(); }

    SpritePresentation presentation();

    const SpriteGroupTracker &tracker() const { return m_tracker; }
    SpriteGroupTracker &tracker() { return m_tracker; }

    void note_write(uint32_t address, unsigned width);
    void frame_end();
    void reset();

    void write_report(std::ostream &out) const { m_tracker.write_report(out); }
    void write_summary(std::ostream &out) const { m_tracker.write_summary(out); }

private:
    SpriteGroupTracker m_tracker;
};

std::unique_ptr<SpriteSystem> make_game_sprites(Machine &machine);
std::unique_ptr<SpriteSystem> make_game_sprites(Machine &machine,
                                                const EnhancementSet &enhancements);

}
