#pragma once
// Runtime discovery log (`--discovery-log PATH`): host-only, observe-only record of game routines
// that the video HLE does not know about yet. Never part of machine or snapshot state.
//
// Categories (one deduplicated entry per (category, key), written and flushed on first sight):
//   sprite-stray    sprite-RAM write outside every emit unit from a PC not in [video.frame_writers]
//                   (the "UNACCOUNTED" PCs of f3rt-tool sprite-check). Needs a game with emit units.
//   video-write     graphics/control RAM write from a PC the game's video code does not list as a known
//                   producer, per layer. Sprite RAM is left to
//                   sprite-stray when the game has emit units.
//   video-fallback  game renderer fell back to the FDP/oracle frame, with the component and reason.
// With `--discovery-all` (set_log_all) known producers are logged too (tagged `known=1`) and sprite RAM
// writes are logged as video-write even when emit units account for them: an observed writer map.
// Format: docs/developer/WORKFLOWS.md ("Discovery log").
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
enum class VideoLayer : uint8_t { Control, Sprites, Pf0, Pf1, Pf2, Pf3, Pf2Alt, Pf3Alt, Text, Lines, Count };
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

    // Sprite RAM belongs to the emit-unit accounting (sprite-stray) when the game has units.
    void set_sprites_accounted_by_units(bool on) { sprites_by_units_ = on; }
    // Also log known producers (tagged known=1) and unit-accounted sprite writes.
    void set_log_all(bool on) {
        log_all_ = on;
        if (on) line("# ALL observed writers: known producers are listed with known=1, sprite RAM included");
    }

    // Machine::write8 for graphics RAM and control registers (`address` is a full 24-bit bus address).
    void video_write(uint32_t pc, uint32_t address);
    // SpriteUnits: byte write into sprite RAM outside any unit from a PC outside frame_writers.
    void sprite_stray(uint32_t pc, uint32_t address);
    // GameVideo: oracle fallback frame. `component`/`reason` are string literals; `frame` is 1-based.
    void video_fallback(const char *component, const char *reason, uint64_t frame);

    // Appends the summary (counts, unit replay aborts) and closes the file. Idempotent.
    void finish(const Machine &machine);

private:
    enum class Category : uint8_t { SpriteStray, VideoWrite, VideoFallback };
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

    std::FILE *file_ = nullptr;
    const Machine &machine_;
    std::chrono::steady_clock::time_point start_;
    bool sprites_by_units_ = false, log_all_ = false, finished_ = false;
    // (category, layer, pc) -> entry. Node-stable, so the one-entry caches below stay valid.
    std::unordered_map<uint64_t, Entry> entries_;
    std::vector<std::unique_ptr<Entry>> fallbacks_;
    std::array<Entry *, size_t(VideoLayer::Count)> last_video_{};
    Entry *last_stray_ = nullptr;
};
} // namespace f3rt
