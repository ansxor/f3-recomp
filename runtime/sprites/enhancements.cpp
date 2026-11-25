#include "sprites/enhancements.hpp"
#include "f3rt/game_video.hpp"
#include "renderer/game/flicker_hold.hpp"
#include "renderer/game/full_detail.hpp"
#include <ostream>
#include <stdexcept>
#include <string>

namespace f3rt {

EnhancementSet::EnhancementSet() : m_inner(rust_sprites::new_enhancement_set()) {
    if (std::string_view(F3RT_DEFAULT_SET) == "commandw") {
        m_inner->push_entry("full-detail", rust_sprites::EnhancementKind::FullDetail);
        m_entries.push_back({"full-detail", "full_detail", [] {
                                 return std::make_unique<FullDetailStage>(int(geometry::FIRST_LINE),
                                                                          int(geometry::HEIGHT));
                             }});
    }
    for (const auto &hold : game_config::flicker_holds) {
        m_inner->push_entry(hold.name, rust_sprites::EnhancementKind::FlickerHold);
        m_entries.push_back({hold.name, "flicker_hold",
                             [hold] { return std::make_unique<FlickerHoldStage>(hold); }});
    }
}

EnhancementSet &EnhancementSet::override_setting(std::string_view setting) {
    try {
        m_inner->override_setting(rust::Str(setting.data(), setting.size()));
    } catch (const rust::Error &error) {
        throw std::invalid_argument(error.what());
    }
    return *this;
}

EnhancementSet &EnhancementSet::set(std::string_view name, bool enabled) {
    try {
        m_inner->set(rust::Str(name.data(), name.size()), enabled);
    } catch (const rust::Error &error) {
        throw std::invalid_argument(error.what());
    }
    return *this;
}

bool EnhancementSet::enabled(std::string_view name, VideoBackend backend) const {
    return m_inner->is_enabled(rust::Str(name.data(), name.size()), backend == VideoBackend::Gpu);
}

void EnhancementSet::list(std::ostream &out) const { out << m_inner->list(); }

RenderEnhancements EnhancementSet::render(VideoBackend backend) const {
    std::vector<std::unique_ptr<SpriteStage>> stages;
    for (const auto &entry : m_entries) {
        if (enabled(entry.name, backend)) {
            stages.push_back(entry.make_stage());
        }
    }
    return RenderEnhancements(std::move(stages));
}

}
