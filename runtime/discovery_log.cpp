#include "discovery_log.hpp"
#include "f3rt/machine.hpp"
#include "sprite_units.hpp"
#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace f3rt {
namespace {
constexpr uint32_t graphics_base = 0x600000, control_base = 0x660000, control_end = 0x660040;
constexpr uint32_t pf_begin = 0x10000, text_begin = 0x1c000, lines_begin = 0x20000;

const char *category_name(int category) {
    static constexpr const char *names[] = {"sprite-stray", "video-write", "video-fallback"};
    return names[category];
}

// Fixed Taito F3 graphics RAM layout (shared by every game): sprites, four 0x2000 playfield maps,
// two alternate map banks, text, then line RAM. Returns Count outside graphics/control RAM.
VideoLayer classify(uint32_t address) {
    if (address - control_base < control_end - control_base) return VideoLayer::Control;
    const uint32_t offset = address - graphics_base;
    if (offset >= 0x40000) return VideoLayer::Count;
    if (offset < pf_begin) return VideoLayer::Sprites;
    if (offset < text_begin) {
        constexpr VideoLayer maps[] = {VideoLayer::Pf0, VideoLayer::Pf1, VideoLayer::Pf2,
                                       VideoLayer::Pf3, VideoLayer::Pf2Alt, VideoLayer::Pf3Alt};
        return maps[(offset - pf_begin) / 0x2000];
    }
    return offset < lines_begin ? VideoLayer::Text : VideoLayer::Lines;
}
} // namespace

const char *video_layer_name(VideoLayer layer) {
    static constexpr const char *names[] = {"control", "sprites", "pf0", "pf1", "pf2", "pf3",
                                            "pf2-alt", "pf3-alt", "text", "lines"};
    return names[size_t(layer)];
}

DiscoveryLog::DiscoveryLog(const std::string &path, const Machine &machine, const std::string &description)
    : machine_(machine), start_(std::chrono::steady_clock::now()) {
    file_ = std::fopen(path.c_str(), "w");
    if (!file_) throw std::runtime_error("Cannot create discovery log " + path);
    line("# f3rt discovery log v1 " + description);
    line("# NEW lines are written when a (category, key) is first seen; REPEAT lines when its hit count reaches a power of 10.");
    line("# frame is 1-based (the emulated frame during which it happened); t is wall seconds since start; counts are byte writes.");
}

DiscoveryLog::~DiscoveryLog() {
    if (file_) std::fclose(file_);
}

double DiscoveryLog::elapsed() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
}

void DiscoveryLog::line(const std::string &text) {
    std::fputs(text.c_str(), file_);
    std::fputc('\n', file_);
    std::fflush(file_); // entries are rare (once per unique key); a crash or quit never loses them
}

// Short identity of an entry (category implied): what a REPEAT line repeats.
std::string DiscoveryLog::key_of(const Entry &entry) const {
    char buffer[160];
    if (entry.category == Category::VideoFallback)
        std::snprintf(buffer, sizeof buffer, "component=%s reason=\"%s\"", entry.component, entry.reason);
    else if (entry.category == Category::SpriteStray) std::snprintf(buffer, sizeof buffer, "pc=0x%06x", entry.pc);
    else std::snprintf(buffer, sizeof buffer, "layer=%s pc=0x%06x", video_layer_name(entry.layer), entry.pc);
    return buffer;
}

std::string DiscoveryLog::describe(const Entry &entry) const {
    char buffer[256];
    if (entry.category == Category::VideoFallback) {
        std::snprintf(buffer, sizeof buffer, "component=%s reason=\"%s\" frame=%llu t=%.2f", entry.component, entry.reason,
                      static_cast<unsigned long long>(entry.frame), entry.seconds);
        return buffer;
    }
    if (entry.category == Category::SpriteStray)
        std::snprintf(buffer, sizeof buffer, "pc=0x%06x frame=%llu t=%.2f addr=0x%06x", entry.pc,
                      static_cast<unsigned long long>(entry.frame), entry.seconds, entry.address);
    else
        std::snprintf(buffer, sizeof buffer, "layer=%s pc=0x%06x frame=%llu t=%.2f addr=0x%06x",
                      video_layer_name(entry.layer), entry.pc, static_cast<unsigned long long>(entry.frame),
                      entry.seconds, entry.address);
    return buffer;
}

DiscoveryLog::Entry &DiscoveryLog::pc_entry(Category category, VideoLayer layer, uint32_t pc, uint32_t address) {
    const uint64_t key = uint64_t(category) << 40 | uint64_t(layer) << 32 | pc;
    auto [it, fresh] = entries_.try_emplace(key);
    Entry &entry = it->second;
    if (fresh) {
        entry.category = category;
        entry.layer = layer;
        entry.pc = pc;
        entry.address = address;
        entry.frame = machine_.frame + 1;
        entry.seconds = elapsed();
        entry.known = category == Category::VideoWrite && video_writer_known(layer, pc);
        if (!entry.known || log_all_)
            line(std::string("NEW ") + category_name(int(category)) + ' ' + describe(entry) + (entry.known ? " known=1" : ""));
    }
    return entry;
}

void DiscoveryLog::count_hit(Entry &entry) {
    if (++entry.count != entry.next_report) return;
    entry.next_report *= 10;
    if (!entry.known || log_all_)
        line(std::string("REPEAT ") + category_name(int(entry.category)) + ' ' + key_of(entry) +
             " count=" + std::to_string(entry.count));
}

void DiscoveryLog::video_write(uint32_t pc, uint32_t address) {
    const VideoLayer layer = classify(address);
    if (layer == VideoLayer::Count) return;
    if (layer == VideoLayer::Sprites && sprites_by_units_ && !log_all_) return;
    Entry *&last = last_video_[size_t(layer)];
    if (!last || last->pc != pc) last = &pc_entry(Category::VideoWrite, layer, pc, address);
    count_hit(*last);
}

void DiscoveryLog::sprite_stray(uint32_t pc, uint32_t address) {
    if (!last_stray_ || last_stray_->pc != pc)
        last_stray_ = &pc_entry(Category::SpriteStray, VideoLayer::Sprites, pc, address);
    count_hit(*last_stray_);
}

void DiscoveryLog::video_fallback(const char *component, const char *reason, uint64_t frame) {
    for (auto &entry : fallbacks_) {
        if (entry->component != component || entry->reason != reason) continue;
        count_hit(*entry);
        return;
    }
    auto &entry = fallbacks_.emplace_back(std::make_unique<Entry>());
    entry->category = Category::VideoFallback;
    entry->component = component;
    entry->reason = reason;
    entry->frame = frame;
    entry->seconds = elapsed();
    count_hit(*entry);
    line(std::string("NEW ") + category_name(int(entry->category)) + ' ' + describe(*entry));
}

void DiscoveryLog::finish(const Machine &machine) {
    if (finished_) return;
    finished_ = true;
    std::vector<const Entry *> listed;
    uint64_t suppressed = 0, known_listed = 0;
    for (const auto &[key, entry] : entries_) {
        if (entry.known && !log_all_) ++suppressed;
        else listed.push_back(&entry);
    }
    for (const auto &entry : fallbacks_) listed.push_back(entry.get());
    std::sort(listed.begin(), listed.end(), [](const Entry *a, const Entry *b) {
        return std::tie(a->category, a->layer, a->pc, a->reason) < std::tie(b->category, b->layer, b->pc, b->reason);
    });
    line("# SUMMARY");
    uint64_t totals[3] = {};
    for (const Entry *entry : listed) {
        if (entry->known) ++known_listed;
        else ++totals[size_t(entry->category)];
        line(std::string("SUM ") + category_name(int(entry->category)) + ' ' + describe(*entry) +
             (entry->known ? " known=1" : "") + " count=" + std::to_string(entry->count));
    }
    if (machine.sprite_units) {
        const auto report = machine.sprite_units->report();
        for (const auto &unit : report.units)
            for (size_t reason = 0; reason < unit.aborts.size(); ++reason)
                if (unit.aborts[reason])
                    line("SUM unit-replay-abort unit=" + unit.name + " reason=" +
                         SpriteUnits::abort_name(SpriteUnits::Abort(reason)) + " count=" + std::to_string(unit.aborts[reason]));
        line("SUM unit-stats invocations=" + std::to_string(report.invocations) + " replays=" + std::to_string(report.replays) +
             " aborted=" + std::to_string(report.aborted) + " unmatched_exits=" + std::to_string(report.unmatched_exits));
    }
    line("# TOTAL frames=" + std::to_string(machine.frame) + " sprite-stray=" + std::to_string(totals[0]) +
         " video-write=" + std::to_string(totals[1]) + " video-fallback=" + std::to_string(totals[2]) +
         " known_writer_pcs_suppressed=" + std::to_string(suppressed) + " known_listed=" + std::to_string(known_listed));
    std::printf("DISCOVERY sprite_stray=%llu video_write=%llu video_fallback=%llu known_suppressed=%llu known_listed=%llu\n",
                static_cast<unsigned long long>(totals[0]), static_cast<unsigned long long>(totals[1]),
                static_cast<unsigned long long>(totals[2]), static_cast<unsigned long long>(suppressed),
                static_cast<unsigned long long>(known_listed));
    std::fclose(file_);
    file_ = nullptr;
}
} // namespace f3rt
