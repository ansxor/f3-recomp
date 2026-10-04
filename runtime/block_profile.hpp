#pragma once
#include "f3rt/rom.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <tuple>
#include <vector>

namespace f3rt {
// Profiling is process-local observational data, deliberately outside snapshots.
// Separate output paths are required for concurrent collectors; merge afterwards.
class BlockProfileSession {
public:
    BlockProfileSession(const RomSet &roms, const std::filesystem::path &output);
    ~BlockProfileSession();
    BlockProfileSession(const BlockProfileSession &) = delete;
    BlockProfileSession &operator=(const BlockProfileSession &) = delete;
    void tick();
    void flush();
    void record_miss(unsigned region, uint32_t crc, uint32_t address);
    const std::filesystem::path &path() const { return output; }
private:
    using Identity = std::pair<unsigned, uint32_t>;
    using Key = std::tuple<unsigned, uint32_t, uint32_t>;
    void load();
    std::filesystem::path output;
    int lock_fd = -1;
    std::array<uint32_t, 2> crcs{}, sizes{};
    std::array<std::vector<uint64_t>, 2> counts;
    std::map<Identity, std::pair<uint32_t, uint32_t>> identities;
    std::map<Key, uint64_t> hits, misses;
    std::chrono::steady_clock::time_point next_flush;
};
}
