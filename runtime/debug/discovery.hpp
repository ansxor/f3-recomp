#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace f3rt {
class Machine;

// Graphics-RAM / control-register regions a store can target.
enum class VideoLayer : uint8_t {
    Control,
    Sprites,
    Pf0,
    Pf1,
    Pf2,
    Pf3,
    Pf2Alt,
    Pf3Alt,
    Text,
    Lines,
    Count
};
const char *video_layer_name(VideoLayer layer);

// Per-game list of store PCs that already have a documented producer (games/<id>/video/video.cpp).
// Games without game video use the always-false default in runtime/renderer/game/video_generic.cpp.
bool video_writer_known(VideoLayer layer, uint32_t pc);

class DiscoveryLog {
public:
    // Throws when `path` cannot be created.
    DiscoveryLog(const std::string &path, const Machine &machine, const std::string &description);
    ~DiscoveryLog();
    DiscoveryLog(const DiscoveryLog &) = delete;
    DiscoveryLog &operator=(const DiscoveryLog &) = delete;

    void set_log_all(bool on) {
        m_log_all = on;
        if (on)
            line("# ALL observed writers: known producers are listed with known=1");
    }

    // Machine::write8 for graphics RAM and control registers (`address` is a full 24-bit bus address).
    void video_write(uint32_t pc, uint32_t address);
    // GameVideo: oracle fallback frame. `component`/`reason` are string literals; `frame` is 1-based.
    void video_fallback(const char *component, const char *reason, uint64_t frame);

    // Appends the summary (counts, unit replay aborts) and closes the file. Idempotent.
    void finish(const Machine &machine);

private:
    enum class Category : uint8_t { VideoWrite, VideoFallback };
    struct Entry {
        Category category = Category::VideoWrite;
        VideoLayer layer = VideoLayer::Control;
        const char *component = nullptr, *reason = nullptr; // video-fallback only (string literals)
        uint32_t pc = 0, address = 0;
        uint64_t frame = 0, count = 0, next_report = 10;
        double seconds = 0;
        bool known = false; // suppressed by the game's known-writer list
    };
    Entry &pc_entry(Category category, VideoLayer layer, uint32_t pc, uint32_t address);
    void count_hit(Entry &entry);
    std::string describe(const Entry &entry) const;
    std::string key_of(const Entry &entry) const;
    double elapsed() const;
    void line(const std::string &text);

    std::FILE *m_file = nullptr;
    const Machine &m_machine;
    std::chrono::steady_clock::time_point m_start;
    bool m_log_all = false, m_finished = false;
    // (category, layer, pc) -> entry. Node-stable, so the one-entry caches below stay valid.
    std::unordered_map<uint64_t, Entry> m_entries;
    std::vector<std::unique_ptr<Entry>> m_fallbacks;
    std::array<Entry *, size_t(VideoLayer::Count)> m_last_video{};
};
} // namespace f3rt
