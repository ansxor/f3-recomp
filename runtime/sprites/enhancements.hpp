#pragma once
#include "generated_config/enhancements.hpp"
#include "sprites/behaviour.hpp"
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace f3rt {

class EnhancementSet {
public:
    struct Entry {
        std::variant<const SpriteBehaviour *, const game_config::FlickerHold *> target;
        std::string_view kind() const;
        std::string_view name() const;
        bool enabled = true;
    };

    EnhancementSet();
    std::span<const Entry> entries() const { return m_entries; }
    EnhancementSet &override_setting(std::string_view setting);
    EnhancementSet &set(std::string_view name, bool enabled);
    EnhancementSet &enable_only_behaviours(std::span<const std::string> names);
    void list(std::ostream &out) const;
    std::vector<const SpriteBehaviour *> behaviours() const;
    std::vector<game_config::FlickerHold> flicker_holds() const;

private:
    std::vector<Entry> m_entries;
};

}
