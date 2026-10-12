#pragma once
#include "f3rt/rom.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <tuple>
#include <vector>
#include <unordered_map>

namespace f3rt {
class IndirectWatch;
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
    const std::filesystem::path &path() const { return m_output; }
    // Executions of the main-CPU instruction at `address` so far (instrumented builds; 0 otherwise).
    uint64_t main_count(uint32_t address) const;
    void register_indirect_watch(IndirectWatch *watch) { m_indirect_watch = watch; }

private:
    using Identity = std::pair<unsigned, uint32_t>;
    using Key = std::tuple<unsigned, uint32_t, uint32_t>;
    void load();
    std::filesystem::path m_output;
    int m_lock_fd = -1;
    std::array<uint32_t, 2> m_crcs{}, m_sizes{};
    std::array<std::vector<uint64_t>, 2> m_counts;
    std::map<Identity, std::pair<uint32_t, uint32_t>> m_identities;
    std::map<Key, uint64_t> m_hits, m_misses;
    std::chrono::steady_clock::time_point m_next_flush;
    IndirectWatch *m_indirect_watch = nullptr;
};

// `--watch-entries`: per frame, how often each listed main-CPU instruction executed (instrumented
// builds count every generated instruction). Host-only and observe-only like the discovery log.
// One line per (address, frame) with executions, flushed as written:
//   ENTRY pc=0x08e20c frame=4123 hits=1 total=57
class EntryWatch {
public:
    // Throws unless the build is instrumented and `session` records (needs --profile-out).
    EntryWatch(const std::filesystem::path &path, const BlockProfileSession &session,
               std::vector<uint32_t> addresses);
    ~EntryWatch();
    EntryWatch(const EntryWatch &) = delete;
    EntryWatch &operator=(const EntryWatch &) = delete;
    void frame(uint64_t frame); // after the frame ran (1-based, Machine::frame)
private:
    const BlockProfileSession &m_session;
    std::vector<uint32_t> m_addresses;
    std::vector<uint64_t> m_last;
    std::FILE *m_file = nullptr;
};

// `--indirect-log`: records observed targets of computed jmp/jsr sites (instrumented
// build with --profile-out). Flushed atomically on profile flushes and at exit:
//   # f3rt indirect targets v1
//   INDIRECT site=0x08e20c target=0x08e244 count=12
class IndirectWatch {
public:
    IndirectWatch(const std::filesystem::path &path, BlockProfileSession &session);
    ~IndirectWatch();
    IndirectWatch(const IndirectWatch &) = delete;
    IndirectWatch &operator=(const IndirectWatch &) = delete;
    void record(uint32_t site, uint32_t target);
    void flush();

private:
    BlockProfileSession &m_session;
    std::filesystem::path m_path;
    std::unordered_map<uint64_t, uint64_t> m_counts;
};
} // namespace f3rt
