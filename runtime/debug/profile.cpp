#include "profile.hpp"
#include "f3rt/block_profile.h"
#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

extern "C" {
uint64_t *f3_profile_main_counts = nullptr;
uint64_t *f3_profile_sound_counts = nullptr;
int f3_profile_indirect_enabled = 0;
void f3_profile_indirect(uint32_t site, uint32_t target);
}
namespace {
f3rt::BlockProfileSession *active = nullptr;
f3rt::IndirectWatch *active_indirect = nullptr;
constexpr uint32_t BASES[] = {0, 0xc00000};
const char *names[] = {"main", "sound"};
uint64_t add(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
uint64_t number(const std::string &text, int radix) {
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, radix);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() ||
        (radix == 16 && (text.size() != 8 || value > UINT32_MAX)))
        throw std::runtime_error("Invalid block profile number: " + text);
    return value;
}
void sync_file(const std::filesystem::path &path) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("Cannot open profile for sync: " + path.string());
    const int result = fsync(fd);
    close(fd);
    if (result) throw std::runtime_error("Cannot sync profile: " + path.string());
}
} // namespace
namespace f3rt {
BlockProfileSession::BlockProfileSession(const RomSet &roms, const std::filesystem::path &path)
    : m_output(path.empty() ? path : std::filesystem::absolute(path)) {
    if (m_output.empty()) return;
#if !defined(F3_PROFILE_INSTRUMENT) && !defined(F3_PROFILE_SLIM_ENABLED)
    throw std::runtime_error("--profile-out requires -DF3_PROFILE_INSTRUMENT=ON (or a slim build "
                             "for cold-hit recording)");
#endif
    if (active)
        throw std::runtime_error("Only one block profile session may be active per process");
    if (!m_output.parent_path().empty())
        std::filesystem::create_directories(m_output.parent_path());
    const auto lock_path = m_output.string() + ".lock";
    m_lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (m_lock_fd < 0) throw std::runtime_error("Cannot lock profile: " + lock_path);
    try {
        if (flock(m_lock_fd, LOCK_EX | LOCK_NB))
            throw std::runtime_error(
                "Profile is in use; choose a separate --profile-out and merge: " +
                m_output.string());
        const std::vector<uint8_t> *programs[] = {&roms.main, &roms.sound};
        for (unsigned region = 0; region < 2; ++region) {
            const auto &bytes = *programs[region];
            if (bytes.empty() || bytes.size() > UINT32_MAX || bytes.size() & 1)
                throw std::runtime_error("Invalid profile ROM size");
            m_crcs[region] = crc32(bytes.data(), bytes.size());
            m_sizes[region] = uint32_t(bytes.size());
#if defined(F3_PROFILE_INSTRUMENT) && F3_PROFILE_INSTRUMENT
            m_counts[region].resize(bytes.size() / 2);
#endif
        }
        if (std::filesystem::exists(m_output)) load();
        for (unsigned region = 0; region < 2; ++region) {
            const Identity identity{region, m_crcs[region]};
            const std::pair bounds{BASES[region], m_sizes[region]};
            const auto [entry, inserted] = m_identities.emplace(identity, bounds);
            if (!inserted && entry->second != bounds)
                throw std::runtime_error("Profile ROM bounds disagree with loaded ROM");
        }
#if defined(F3_PROFILE_INSTRUMENT) && F3_PROFILE_INSTRUMENT
        for (auto row = m_hits.begin(); row != m_hits.end();) {
            const auto [region, crc, address] = row->first;
            if (crc == m_crcs[region]) {
                m_counts[region][(address - bases[region]) / 2] = row->second;
                row = m_hits.erase(row);
            } else ++row;
        }
#endif
        flush(); // An initial atomic file also proves the destination is writable.
#if defined(F3_PROFILE_INSTRUMENT) && F3_PROFILE_INSTRUMENT
        f3_profile_main_counts = m_counts[0].data();
        f3_profile_sound_counts = m_counts[1].data();
#endif
        active = this;
        m_next_flush = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    } catch (...) {
        close(m_lock_fd);
        m_lock_fd = -1;
        throw;
    }
}
BlockProfileSession::~BlockProfileSession() {
    if (m_output.empty()) return;
    if (active == this) {
        active = nullptr;
        f3_profile_main_counts = f3_profile_sound_counts = nullptr;
    }
    try {
        flush();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "PROFILE FLUSH FAILED: %s\n", error.what());
    }
    if (m_lock_fd >= 0) close(m_lock_fd);
}
void BlockProfileSession::load() {
    std::ifstream input(m_output);
    std::string line;
    if (!input || !std::getline(input, line) || line != "F3-BLOCK-PROFILE 1")
        throw std::runtime_error("Unsupported block profile version: " + m_output.string());
    while (std::getline(input, line)) {
        std::istringstream record(line);
        std::string kind, region_name, crc_text, address_text, value_text, extra;
        if (!(record >> kind)) continue;
        if (!(record >> region_name >> crc_text >> address_text >> value_text) || record >> extra)
            throw std::runtime_error("Malformed block profile record");
        unsigned region;
        if (region_name == "main") region = 0;
        else if (region_name == "sound") region = 1;
        else throw std::runtime_error("Unknown block profile CPU");
        const auto crc = uint32_t(number(crc_text, 16));
        const auto address = uint32_t(number(address_text, 16));
        if (kind == "rom") {
            const auto size = uint32_t(number(value_text, 16));
            if (!size || (address | size) & 1 ||
                uint64_t(address) + size > uint64_t(UINT32_MAX) + 1 ||
                !m_identities.emplace(Identity{region, crc}, std::pair{address, size}).second)
                throw std::runtime_error("Invalid or duplicate block profile ROM");
        } else {
            if (kind != "hit" && kind != "miss")
                throw std::runtime_error("Unknown block profile record");
            const auto count = number(value_text, 10);
            if (!count) throw std::runtime_error("Zero block profile count");
            auto &destination = kind == "hit" ? m_hits : m_misses;
            auto &value = destination[Key{region, crc, address}];
            value = add(value, count);
        }
    }
    if (!input.eof()) throw std::runtime_error("Block profile read failed");
    for (const auto *rows : {&m_hits, &m_misses})
        for (const auto &[key, count] : *rows) {
            (void)count;
            const auto [region, crc, address] = key;
            const auto identity = m_identities.find({region, crc});
            if (identity == m_identities.end())
                throw std::runtime_error("Profile address has no ROM identity");
            const auto [base, size] = identity->second;
            if (address & 1 || address < base || uint64_t(address) >= uint64_t(base) + size)
                throw std::runtime_error("Profile address outside aligned ROM");
        }
}
void BlockProfileSession::flush() {
    if (m_output.empty()) return;
    const std::filesystem::path temporary = m_output.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        file << "F3-BLOCK-PROFILE 1\n" << std::setfill('0');
        if (!file)
            throw std::runtime_error("Block profile open failed: " + temporary.string() + ": " +
                                     std::strerror(errno));
        for (const auto &[identity, bounds] : m_identities) {
            const auto [region, crc] = identity;
            file << "rom " << names[region] << ' ' << std::hex << std::setw(8) << crc << ' '
                 << std::setw(8) << bounds.first << ' ' << std::setw(8) << bounds.second << '\n';
        }
        auto row = [&](const char *kind, unsigned region, uint32_t crc, uint32_t address,
                       uint64_t count) {
            file << kind << ' ' << names[region] << ' ' << std::hex << std::setw(8) << crc << ' '
                 << std::setw(8) << address << ' ' << std::dec << count << '\n';
        };
        for (const auto &[key, count] : m_hits) {
            const auto [region, crc, address] = key;
            row("hit", region, crc, address, count);
        }
        for (unsigned region = 0; region < 2; ++region)
            for (size_t index = 0; index < m_counts[region].size(); ++index)
                if (m_counts[region][index])
                    row("hit", region, m_crcs[region], BASES[region] + uint32_t(index * 2),
                        m_counts[region][index]);
        for (const auto &[key, count] : m_misses) {
            const auto [region, crc, address] = key;
            row("miss", region, crc, address, count);
        }
        file.flush();
        if (!file)
            throw std::runtime_error("Block profile write failed: " + temporary.string() + ": " +
                                     std::strerror(errno));
    }
    sync_file(temporary);
    std::filesystem::rename(temporary, m_output);
    if (m_indirect_watch) {
        try {
            m_indirect_watch->flush();
        } catch (const std::exception &error) {
            std::fprintf(stderr, "INDIRECT LOG FLUSH FAILED: %s\n", error.what());
        }
    }
}
void BlockProfileSession::tick() {
    if (!m_output.empty() && std::chrono::steady_clock::now() >= m_next_flush) {
        flush();
        m_next_flush = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }
}
void BlockProfileSession::record_miss(unsigned region, uint32_t crc, uint32_t address) {
    const auto identity = m_identities.find({region, crc});
    if (identity != m_identities.end()) {
        const auto [base, size] = identity->second;
        if (!(address & 1) && address >= base && uint64_t(address) < uint64_t(base) + size) {
            auto &count = m_misses[Key{region, crc, address}];
            count = add(count, 1);
            flush();
        }
    }
}
uint64_t BlockProfileSession::main_count(uint32_t address) const {
    const size_t index = address >> 1;
    return index < m_counts[0].size() ? m_counts[0][index] : 0;
}

EntryWatch::EntryWatch(const std::filesystem::path &path, const BlockProfileSession &session,
                       std::vector<uint32_t> addresses)
    : m_session(session), m_addresses(std::move(addresses)) {
#if !defined(F3_PROFILE_INSTRUMENT) || !F3_PROFILE_INSTRUMENT
    throw std::runtime_error("--watch-entries needs a -DF3_PROFILE_INSTRUMENT=ON build");
#endif
    if (session.path().empty())
        throw std::runtime_error(
            "--watch-entries needs --profile-out FILE (the counters live there)");
    for (const uint32_t address : m_addresses)
        if (address & 1)
            throw std::runtime_error(
                "--watch-entries addresses must be even instruction addresses");
    m_file = std::fopen(path.c_str(), "w");
    if (!m_file) throw std::runtime_error("Cannot create entry watch log " + path.string());
    std::fputs("# f3rt entry watch v1: ENTRY lines list frames (1-based) in which a watched "
               "instruction executed\n",
               m_file);
    for (const uint32_t address : m_addresses) {
        m_last.push_back(m_session.main_count(address));
        std::fprintf(m_file, "# watch pc=0x%06x\n", address);
    }
    std::fflush(m_file);
}

EntryWatch::~EntryWatch() {
    if (m_file) std::fclose(m_file);
}

void EntryWatch::frame(uint64_t frame) {
    bool wrote = false;
    for (size_t i = 0; i < m_addresses.size(); ++i) {
        const uint64_t total = m_session.main_count(m_addresses[i]);
        if (total == m_last[i]) continue;
        std::fprintf(m_file, "ENTRY pc=0x%06x frame=%llu hits=%llu total=%llu\n", m_addresses[i],
                     static_cast<unsigned long long>(frame),
                     static_cast<unsigned long long>(total - m_last[i]),
                     static_cast<unsigned long long>(total));
        m_last[i] = total;
        wrote = true;
    }
    if (wrote) std::fflush(m_file);
}

IndirectWatch::IndirectWatch(const std::filesystem::path &path, BlockProfileSession &session)
    : m_session(session), m_path(path.empty() ? path : std::filesystem::absolute(path)) {
#if !defined(F3_PROFILE_INSTRUMENT) || !F3_PROFILE_INSTRUMENT
    throw std::runtime_error("--indirect-log needs a -DF3_PROFILE_INSTRUMENT=ON build");
#endif
    if (session.path().empty())
        throw std::runtime_error(
            "--indirect-log needs --profile-out FILE (the counters live there)");
    if (m_path.empty()) throw std::runtime_error("--indirect-log needs a non-empty FILE");
    if (!m_path.parent_path().empty()) std::filesystem::create_directories(m_path.parent_path());
    active_indirect = this;
    f3_profile_indirect_enabled = 1;
    m_session.register_indirect_watch(this);
    flush(); // An initial atomic file also proves the destination is writable.
}

IndirectWatch::~IndirectWatch() {
    m_session.register_indirect_watch(nullptr);
    f3_profile_indirect_enabled = 0;
    active_indirect = nullptr;
    try {
        flush();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "INDIRECT LOG FLUSH FAILED: %s\n", error.what());
    }
}

void IndirectWatch::record(uint32_t site, uint32_t target) {
    const uint64_t key = (static_cast<uint64_t>(site) << 32) | target;
    auto &count = m_counts[key];
    if (count != UINT64_MAX) ++count;
}

void IndirectWatch::flush() {
    if (m_path.empty()) return;
    const std::filesystem::path temporary = m_path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file)
            throw std::runtime_error("Cannot open indirect log: " + temporary.string() + ": " +
                                     std::strerror(errno));
        file << "# f3rt indirect targets v1\n";
        struct Item {
            uint32_t site;
            uint32_t target;
            uint64_t count;
        };
        std::vector<Item> items;
        items.reserve(m_counts.size());
        for (const auto &[key, count] : m_counts) {
            items.push_back({static_cast<uint32_t>(key >> 32),
                             static_cast<uint32_t>(key & 0xffffffffu), count});
        }
        std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
            if (a.site != b.site) return a.site < b.site;
            return a.target < b.target;
        });
        char buf[128];
        for (const auto &item : items) {
            std::snprintf(buf, sizeof(buf), "INDIRECT site=0x%06x target=0x%06x count=%llu\n",
                          item.site, item.target, static_cast<unsigned long long>(item.count));
            file << buf;
        }
        file.flush();
        if (!file)
            throw std::runtime_error("Indirect log write failed: " + temporary.string() + ": " +
                                     std::strerror(errno));
    }
    sync_file(temporary);
    std::filesystem::rename(temporary, m_path);
}
} // namespace f3rt
extern "C" void f3_profile_indirect(uint32_t site, uint32_t target) {
    if (active_indirect) active_indirect->record(site, target);
}
extern "C" [[noreturn]] void f3_profile_cold_abort(unsigned region, uint32_t crc,
                                                   uint32_t address) {
    const auto log_path = active ? active->path().string() + ".cold-hits" : "f3-cold-hits.log";
    char message[384];
    std::snprintf(message, sizeof(message),
                  "F3 PROFILE SLIM ABORT: removed %s block address=0x%08x ROM_CRC=0x%08x; "
                  "re-profile with a full -DF3_PROFILE_INSTRUMENT=ON build and --profile-out FILE",
                  region < 2 ? names[region] : "unknown", address, crc);
    std::fprintf(stderr, "%s\n", message);
    FILE *log = std::fopen(log_path.c_str(), "a");
    if (log) {
        const bool failed =
            std::fprintf(log, "%s\n", message) < 0 || std::fflush(log) || fsync(fileno(log));
        std::fclose(log);
        if (failed) std::fprintf(stderr, "COLD-HIT RECORD FAILED: %s\n", log_path.c_str());
        else std::fprintf(stderr, "Cold hit recorded: %s\n", log_path.c_str());
    } else std::fprintf(stderr, "COLD-HIT RECORD FAILED: %s\n", log_path.c_str());
    if (active) {
        try {
            active->record_miss(region, crc, address);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "COLD-HIT PROFILE FAILED: %s\n", error.what());
        }
    }
    throw std::runtime_error(message);
}
