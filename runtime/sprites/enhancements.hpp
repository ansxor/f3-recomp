#pragma once
#include "f3rt/renderer/game/enhancements.hpp"
#include "f3rt_sprites_cxx/sprites.h"
#include <functional>
#include <iosfwd>
#include <span>
#include <string_view>
#include <vector>

namespace f3rt {

class EnhancementSet {
public:
    struct Entry {
        std::string_view name;
        std::string_view kind;
        std::function<std::unique_ptr<SpriteStage>()> make_stage;
    };

    EnhancementSet();
    EnhancementSet(const EnhancementSet &) = delete;
    EnhancementSet &operator=(const EnhancementSet &) = delete;
    EnhancementSet(EnhancementSet &&) noexcept = default;
    EnhancementSet &operator=(EnhancementSet &&) noexcept = default;

    std::span<const Entry> entries() const { return m_entries; }
    EnhancementSet &override_setting(std::string_view setting);
    EnhancementSet &set(std::string_view name, bool enabled);
    bool enabled(std::string_view name, VideoBackend backend = VideoBackend::Gpu) const;
    void list(std::ostream &out) const;
    RenderEnhancements render(VideoBackend backend) const;

private:
    rust::Box<rust_sprites::EnhancementSet> m_inner;
    std::vector<Entry> m_entries;
};

}
