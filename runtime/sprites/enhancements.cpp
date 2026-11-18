#include "sprites/enhancements.hpp"
#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <string>

namespace f3rt {

EnhancementSet::EnhancementSet() {
    for (const auto *behaviour : registered_sprite_behaviours())
        m_entries.push_back({behaviour});
    for (const auto &hold : game_config::flicker_holds)
        m_entries.push_back({&hold});
}

EnhancementSet &EnhancementSet::override_setting(std::string_view setting) {
    const auto equals = setting.find('=');
    const auto value = equals == setting.npos ? "" : setting.substr(equals + 1);
    if (value != "on" && value != "off")
        throw std::invalid_argument("--enhance expects NAME=on|off");
    return set(setting.substr(0, equals), value == "on");
}

EnhancementSet &EnhancementSet::set(std::string_view name, bool enabled) {
    const auto entry = std::ranges::find(m_entries, name, &Entry::name);
    if (entry == m_entries.end())
        throw std::invalid_argument("Unknown enhancement '" + std::string(name) + "'");
    entry->enabled = enabled;
    return *this;
}

std::string_view EnhancementSet::Entry::kind() const {
    return std::holds_alternative<const SpriteBehaviour *>(target) ? "behaviour" : "flicker_hold";
}

std::string_view EnhancementSet::Entry::name() const {
    return std::visit([](const auto *value) -> std::string_view { return value->name; }, target);
}

EnhancementSet &EnhancementSet::enable_only_behaviours(std::span<const std::string> names) {
    for (const auto &name : names) {
        if (std::ranges::none_of(m_entries, [&](const Entry &entry) {
                return std::holds_alternative<const SpriteBehaviour *>(entry.target) &&
                       entry.name() == name;
            }))
            throw std::invalid_argument("Unknown --behaviour '" + name + "'");
    }
    for (auto &entry : m_entries)
        if (std::holds_alternative<const SpriteBehaviour *>(entry.target))
            entry.enabled = std::ranges::find(names, entry.name()) != names.end();
    return *this;
}

void EnhancementSet::list(std::ostream &out) const {
    for (const auto &entry : m_entries) out << entry.kind() << ' ' << entry.name() << '\n';
}

std::vector<const SpriteBehaviour *> EnhancementSet::behaviours() const {
    std::vector<const SpriteBehaviour *> result;
    for (const auto &entry : m_entries)
        if (const auto *behaviour = std::get_if<const SpriteBehaviour *>(&entry.target);
            entry.enabled && behaviour)
            result.push_back(*behaviour);
    return result;
}

std::vector<game_config::FlickerHold> EnhancementSet::flicker_holds() const {
    std::vector<game_config::FlickerHold> result;
    for (const auto &entry : m_entries)
        if (const auto *hold = std::get_if<const game_config::FlickerHold *>(&entry.target);
            entry.enabled && hold)
            result.push_back(**hold);
    return result;
}

}
