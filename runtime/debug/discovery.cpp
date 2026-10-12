#include "discovery.hpp"
#include "f3rt/memory_map.hpp"
#include "f3rt/machine.hpp"
#include "sprites/units.hpp"
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace f3rt {
namespace {
constexpr uint32_t GRAPHICS_RAM_BASE = GRAPHICS_BASE, CONTROL_RAM_BASE = CONTROL_BASE,
                   CONTROL_RAM_END = CONTROL_EXTENDED_END;
constexpr uint32_t PF_BEGIN = OFFS_PLAYFIELDS, TEXT_BEGIN = OFFS_TEXTRAM_MAP,
                   LINES_BEGIN = OFFS_LINERAM;

const char *category_name(int category) {
    static constexpr const char *NAMES[] = {"sprite-stray", "video-write", "video-fallback"};
    return NAMES[category];
}

// Fixed Taito F3 graphics RAM layout (shared by every game): sprites, four 0x2000 playfield maps,
// two alternate map banks, text, then line RAM. Returns Count outside graphics/control RAM.
VideoLayer classify(uint32_t address) {
    if (address - CONTROL_RAM_BASE < CONTROL_RAM_END - CONTROL_RAM_BASE) return VideoLayer::Control;
    const uint32_t offset = address - GRAPHICS_RAM_BASE;
    if (offset >= GRAPHICS_RAM_SIZE) return VideoLayer::Count;
    if (offset < PF_BEGIN) return VideoLayer::Sprites;
    if (offset < TEXT_BEGIN) {
        constexpr VideoLayer MAPS[] = {VideoLayer::Pf0, VideoLayer::Pf1,    VideoLayer::Pf2,
                                       VideoLayer::Pf3, VideoLayer::Pf2Alt, VideoLayer::Pf3Alt};
        return MAPS[(offset - PF_BEGIN) / PLAYFIELD_LAYER_SIZE];
    }
    return offset < LINES_BEGIN ? VideoLayer::Text : VideoLayer::Lines;
}
} // namespace

const char *video_layer_name(VideoLayer layer) {
    static constexpr const char *NAMES[] = {"control", "sprites", "pf0",     "pf1",  "pf2",
                                            "pf3",     "pf2-alt", "pf3-alt", "text", "lines"};
    return NAMES[size_t(layer)];
}

DiscoveryLog::DiscoveryLog(const std::string &path, const Machine &machine,
                           const std::string &description)
    : m_machine(machine), m_start(std::chrono::steady_clock::now()) {
    m_file = std::fopen(path.c_str(), "w");
    if (!m_file) throw std::runtime_error("Cannot create discovery log " + path);
    line("# f3rt discovery log v1 " + description);
    line("# NEW lines are written when a (category, key) is first seen; REPEAT lines when its hit "
         "count reaches a power of 10.");
    line("# frame is 1-based (the emulated frame during which it happened); t is wall seconds "
         "since start; counts are byte writes.");
}

DiscoveryLog::~DiscoveryLog() {
    if (m_file) std::fclose(m_file);
}

double DiscoveryLog::elapsed() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
}

void DiscoveryLog::line(const std::string &text) {
    std::fputs(text.c_str(), m_file);
    std::fputc('\n', m_file);
    std::fflush(m_file); // entries are rare (once per unique key); a crash or quit never loses them
}

// Short identity of an entry (category implied): what a REPEAT line repeats.
std::string DiscoveryLog::key_of(const Entry &entry) const {
    char buffer[160];
    if (entry.category == Category::VideoFallback)
        std::snprintf(buffer, sizeof buffer, "component=%s reason=\"%s\"", entry.component,
                      entry.reason);
    else if (entry.category == Category::SpriteStray)
        std::snprintf(buffer, sizeof buffer, "pc=0x%06x", entry.pc);
    else
        std::snprintf(buffer, sizeof buffer, "layer=%s pc=0x%06x", video_layer_name(entry.layer),
                      entry.pc);
    return buffer;
}

std::string DiscoveryLog::describe(const Entry &entry) const {
    char buffer[256];
    if (entry.category == Category::VideoFallback) {
        std::snprintf(buffer, sizeof buffer, "component=%s reason=\"%s\" frame=%llu t=%.2f",
                      entry.component, entry.reason, static_cast<unsigned long long>(entry.frame),
                      entry.seconds);
        return buffer;
    }
    if (entry.category == Category::SpriteStray)
        std::snprintf(buffer, sizeof buffer, "pc=0x%06x frame=%llu t=%.2f addr=0x%06x", entry.pc,
                      static_cast<unsigned long long>(entry.frame), entry.seconds, entry.address);
    else
        std::snprintf(buffer, sizeof buffer, "layer=%s pc=0x%06x frame=%llu t=%.2f addr=0x%06x",
                      video_layer_name(entry.layer), entry.pc,
                      static_cast<unsigned long long>(entry.frame), entry.seconds, entry.address);
    return buffer;
}

DiscoveryLog::Entry &DiscoveryLog::pc_entry(Category category, VideoLayer layer, uint32_t pc,
                                            uint32_t address) {
    const uint64_t key = uint64_t(category) << 40 | uint64_t(layer) << 32 | pc;
    auto [it, fresh] = m_entries.try_emplace(key);
    Entry &entry = it->second;
    if (fresh) {
        entry.category = category;
        entry.layer = layer;
        entry.pc = pc;
        entry.address = address;
        entry.frame = m_machine.frame + 1;
        entry.seconds = elapsed();
        entry.known = category == Category::VideoWrite && video_writer_known(layer, pc);
        if (!entry.known || m_log_all)
            line(std::string("NEW ") + category_name(int(category)) + ' ' + describe(entry) +
                 (entry.known ? " known=1" : ""));
    }
    return entry;
}

void DiscoveryLog::count_hit(Entry &entry) {
    if (++entry.count != entry.next_report) return;
    entry.next_report *= 10;
    if (!entry.known || m_log_all)
        line(std::string("REPEAT ") + category_name(int(entry.category)) + ' ' + key_of(entry) +
             " count=" + std::to_string(entry.count));
}

void DiscoveryLog::video_write(uint32_t pc, uint32_t address) {
    const VideoLayer layer = classify(address);
    if (layer == VideoLayer::Count) return;
    if (layer == VideoLayer::Sprites && m_sprites_by_units && !m_log_all) return;
    Entry *&last = m_last_video[size_t(layer)];
    if (!last || last->pc != pc) last = &pc_entry(Category::VideoWrite, layer, pc, address);
    count_hit(*last);
}

void DiscoveryLog::sprite_stray(uint32_t pc, uint32_t address) {
    if (!m_last_stray || m_last_stray->pc != pc)
        m_last_stray = &pc_entry(Category::SpriteStray, VideoLayer::Sprites, pc, address);
    count_hit(*m_last_stray);
}

void DiscoveryLog::video_fallback(const char *component, const char *reason, uint64_t frame) {
    for (auto &entry : m_fallbacks) {
        if (entry->component != component || entry->reason != reason) continue;
        count_hit(*entry);
        return;
    }
    auto &entry = m_fallbacks.emplace_back(std::make_unique<Entry>());
    entry->category = Category::VideoFallback;
    entry->component = component;
    entry->reason = reason;
    entry->frame = frame;
    entry->seconds = elapsed();
    count_hit(*entry);
    line(std::string("NEW ") + category_name(int(entry->category)) + ' ' + describe(*entry));
}

void DiscoveryLog::finish(const Machine &machine) {
    if (m_finished) return;
    m_finished = true;
    std::vector<std::reference_wrapper<const Entry>> listed;
    uint64_t suppressed = 0, known_listed = 0;
    for (const auto &[key, entry] : m_entries) {
        if (entry.known && !m_log_all) ++suppressed;
        else listed.push_back(entry);
    }
    for (const auto &entry : m_fallbacks) listed.push_back(*entry);
    // Order by content, never by address: component/reason are string literals whose addresses
    // differ between builds.
    const auto sort_key = [](const Entry &e) {
        return std::make_tuple(e.category, e.layer, e.pc,
                               std::string_view(e.component ? e.component : ""),
                               std::string_view(e.reason ? e.reason : ""));
    };
    std::sort(listed.begin(), listed.end(),
              [&sort_key](const Entry &a, const Entry &b) { return sort_key(a) < sort_key(b); });
    line("# SUMMARY");
    uint64_t totals[3] = {};
    for (const Entry &entry : listed) {
        if (entry.known) ++known_listed;
        else ++totals[size_t(entry.category)];
        line(std::string("SUM ") + category_name(int(entry.category)) + ' ' + describe(entry) +
             (entry.known ? " known=1" : "") + " count=" + std::to_string(entry.count));
    }
    if (machine.sprite_units) {
        const auto report = machine.sprite_units->report();
        for (const auto &unit : report.units)
            for (size_t reason = 0; reason < unit.aborts.size(); ++reason)
                if (unit.aborts[reason])
                    line("SUM unit-replay-abort unit=" + unit.name +
                         " reason=" + SpriteUnits::abort_name(SpriteUnits::Abort(reason)) +
                         " count=" + std::to_string(unit.aborts[reason]));
        line("SUM unit-stats invocations=" + std::to_string(report.invocations) + " replays=" +
             std::to_string(report.replays) + " aborted=" + std::to_string(report.aborted) +
             " unmatched_exits=" + std::to_string(report.unmatched_exits));
    }
    line("# TOTAL frames=" + std::to_string(machine.frame) + " sprite-stray=" +
         std::to_string(totals[0]) + " video-write=" + std::to_string(totals[1]) +
         " video-fallback=" + std::to_string(totals[2]) + " known_writer_pcs_suppressed=" +
         std::to_string(suppressed) + " known_listed=" + std::to_string(known_listed));
    std::printf(
        "DISCOVERY sprite_stray=%llu video_write=%llu video_fallback=%llu known_suppressed=%llu "
        "known_listed=%llu\n",
        static_cast<unsigned long long>(totals[0]), static_cast<unsigned long long>(totals[1]),
        static_cast<unsigned long long>(totals[2]), static_cast<unsigned long long>(suppressed),
        static_cast<unsigned long long>(known_listed));
    std::fclose(m_file);
    m_file = nullptr;
}
} // namespace f3rt
