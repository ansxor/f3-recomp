#pragma once
#include "f3rt/renderer/game/enhancements.hpp"
#include "sprites/behaviour.hpp"
#include <functional>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace f3rt {

class EnhancementSet {
public:
    struct RenderEntry {
        std::string_view name;
        std::string_view kind;
        std::vector<VideoBackend> backends;
        std::function<std::unique_ptr<SpriteStage>()> make_stage;
    };

    struct Entry {
        std::variant<const SpriteBehaviour *, RenderEntry> target;
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
    RenderEnhancements render(VideoBackend backend) const;

private:
    std::vector<Entry> m_entries;
};

}
