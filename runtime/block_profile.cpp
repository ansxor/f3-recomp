#include "block_profile.hpp"
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
constexpr uint32_t bases[] = {0, 0xc00000};
const char *names[] = {"main", "sound"};
uint64_t add(uint64_t a, uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
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
}
namespace f3rt {
BlockProfileSession::BlockProfileSession(const RomSet &roms, const std::filesystem::path &path)
    : output(path.empty() ? path : std::filesystem::absolute(path)) {
    if (output.empty()) return;
#if !defined(F3_PROFILE_INSTRUMENT) && !defined(F3_PROFILE_SLIM_ENABLED)
    throw std::runtime_error("--profile-out requires -DF3_PROFILE_INSTRUMENT=ON (or a slim build for cold-hit recording)");
#endif
    if (active) throw std::runtime_error("Only one block profile session may be active per process");
    if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
    const auto lock_path = output.string() + ".lock";
    lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (lock_fd < 0) throw std::runtime_error("Cannot lock profile: " + lock_path);
    try {
        if (flock(lock_fd, LOCK_EX | LOCK_NB))
            throw std::runtime_error("Profile is in use; choose a separate --profile-out and merge: " + output.string());
        const std::vector<uint8_t> *programs[] = {&roms.main, &roms.sound};
        for (unsigned region = 0; region < 2; ++region) {
            const auto &bytes = *programs[region];
            if (bytes.empty() || bytes.size() > UINT32_MAX || bytes.size() & 1)
                throw std::runtime_error("Invalid profile ROM size");
            crcs[region] = crc32(bytes.data(), bytes.size());
            sizes[region] = uint32_t(bytes.size());
#if defined(F3_PROFILE_INSTRUMENT) && F3_PROFILE_INSTRUMENT
            counts[region].resize(bytes.size() / 2);
#endif
        }
        if (std::filesystem::exists(output)) load();
        for (unsigned region = 0; region < 2; ++region) {
            const Identity identity{region, crcs[region]};
            const std::pair bounds{bases[region], sizes[region]};
            const auto [entry, inserted] = identities.emplace(identity, bounds);
            if (!inserted && entry->second != bounds)
                throw std::runtime_error("Profile ROM bounds disagree with loaded ROM");
        }
#if defined(F3_PROFILE_INSTRUMENT) && F3_PROFILE_INSTRUMENT
        for (auto row = hits.begin(); row != hits.end();) {
            const auto [region, crc, address] = row->first;
            if (crc == crcs[region]) {
                counts[region][(address - bases[region]) / 2] = row->second;
                row = hits.erase(row);
            } else ++row;
        }
#endif
        flush(); // An initial atomic file also proves the destination is writable.
#if defined(F3_PROFILE_INSTRUMENT) && F3_PROFILE_INSTRUMENT
        f3_profile_main_counts = counts[0].data();
        f3_profile_sound_counts = counts[1].data();
#endif
        active = this;
        next_flush = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    } catch (...) {
        close(lock_fd);
        lock_fd = -1;
        throw;
    }
}
BlockProfileSession::~BlockProfileSession() {
    if (output.empty()) return;
    if (active == this) {
        active = nullptr;
        f3_profile_main_counts = f3_profile_sound_counts = nullptr;
    }
    try { flush(); }
    catch (const std::exception &error) { std::fprintf(stderr, "PROFILE FLUSH FAILED: %s\n", error.what()); }
    if (lock_fd >= 0) close(lock_fd);
}
void BlockProfileSession::load() {
    std::ifstream input(output);
    std::string line;
    if (!input || !std::getline(input, line) || line != "F3-BLOCK-PROFILE 1")
        throw std::runtime_error("Unsupported block profile version: " + output.string());
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
            if (!size || (address | size) & 1 || uint64_t(address) + size > uint64_t(UINT32_MAX) + 1 ||
                !identities.emplace(Identity{region, crc}, std::pair{address, size}).second)
                throw std::runtime_error("Invalid or duplicate block profile ROM");
        } else {
            if (kind != "hit" && kind != "miss") throw std::runtime_error("Unknown block profile record");
            const auto count = number(value_text, 10);
            if (!count) throw std::runtime_error("Zero block profile count");
            auto &destination = kind == "hit" ? hits : misses;
            auto &value = destination[Key{region, crc, address}];
            value = add(value, count);
        }
    }
    if (!input.eof()) throw std::runtime_error("Block profile read failed");
    for (const auto *rows : {&hits, &misses}) for (const auto &[key, count] : *rows) {
        (void)count;
        const auto [region, crc, address] = key;
        const auto identity = identities.find({region, crc});
        if (identity == identities.end()) throw std::runtime_error("Profile address has no ROM identity");
        const auto [base, size] = identity->second;
        if (address & 1 || address < base || uint64_t(address) >= uint64_t(base) + size)
            throw std::runtime_error("Profile address outside aligned ROM");
    }
}
void BlockProfileSession::flush() {
    if (output.empty()) return;
    const std::filesystem::path temporary = output.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        file << "F3-BLOCK-PROFILE 1\n" << std::setfill('0');
        if (!file) throw std::runtime_error("Block profile open failed: " + temporary.string() + ": " + std::strerror(errno));
        for (const auto &[identity, bounds] : identities) {
            const auto [region, crc] = identity;
            file << "rom " << names[region] << ' ' << std::hex << std::setw(8) << crc << ' '
                 << std::setw(8) << bounds.first << ' ' << std::setw(8) << bounds.second << '\n';
        }
        auto row = [&](const char *kind, unsigned region, uint32_t crc, uint32_t address, uint64_t count) {
            file << kind << ' ' << names[region] << ' ' << std::hex << std::setw(8) << crc << ' '
                 << std::setw(8) << address << ' ' << std::dec << count << '\n';
        };
        for (const auto &[key, count] : hits) {
            const auto [region, crc, address] = key;
            row("hit", region, crc, address, count);
        }
        for (unsigned region = 0; region < 2; ++region)
            for (size_t index = 0; index < counts[region].size(); ++index)
                if (counts[region][index]) row("hit", region, crcs[region], bases[region] + uint32_t(index * 2), counts[region][index]);
        for (const auto &[key, count] : misses) {
            const auto [region, crc, address] = key;
            row("miss", region, crc, address, count);
        }
        file.flush();
        if (!file) throw std::runtime_error("Block profile write failed: " + temporary.string() + ": " + std::strerror(errno));
    }
    sync_file(temporary);
    std::filesystem::rename(temporary, output);
    if (indirect_watch_) {
        try { indirect_watch_->flush(); }
        catch (const std::exception &error) { std::fprintf(stderr, "INDIRECT LOG FLUSH FAILED: %s\n", error.what()); }
    }
}
void BlockProfileSession::tick() {
    if (!output.empty() && std::chrono::steady_clock::now() >= next_flush) {
        flush();
        next_flush = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }
}
void BlockProfileSession::record_miss(unsigned region, uint32_t crc, uint32_t address) {
    const auto identity = identities.find({region, crc});
    if (identity != identities.end()) {
        const auto [base, size] = identity->second;
        if (!(address & 1) && address >= base && uint64_t(address) < uint64_t(base) + size) {
            auto &count = misses[Key{region, crc, address}];
            count = add(count, 1);
            flush();
        }
    }
}
uint64_t BlockProfileSession::main_count(uint32_t address) const {
    const size_t index = address >> 1;
    return index < counts[0].size() ? counts[0][index] : 0;
}

EntryWatch::EntryWatch(const std::filesystem::path &path, const BlockProfileSession &session,
                       std::vector<uint32_t> addresses)
    : session_(session), addresses_(std::move(addresses)) {
#if !defined(F3_PROFILE_INSTRUMENT) || !F3_PROFILE_INSTRUMENT
    throw std::runtime_error("--watch-entries needs a -DF3_PROFILE_INSTRUMENT=ON build");
#endif
    if (session.path().empty()) throw std::runtime_error("--watch-entries needs --profile-out FILE (the counters live there)");
    for (const uint32_t address : addresses_)
        if (address & 1) throw std::runtime_error("--watch-entries addresses must be even instruction addresses");
    file_ = std::fopen(path.c_str(), "w");
    if (!file_) throw std::runtime_error("Cannot create entry watch log " + path.string());
    std::fputs("# f3rt entry watch v1: ENTRY lines list frames (1-based) in which a watched instruction executed\n", file_);
    for (const uint32_t address : addresses_) {
        last_.push_back(session_.main_count(address));
        std::fprintf(file_, "# watch pc=0x%06x\n", address);
    }
    std::fflush(file_);
}

EntryWatch::~EntryWatch() {
    if (file_) std::fclose(file_);
}

void EntryWatch::frame(uint64_t frame) {
    bool wrote = false;
    for (size_t i = 0; i < addresses_.size(); ++i) {
        const uint64_t total = session_.main_count(addresses_[i]);
        if (total == last_[i]) continue;
        std::fprintf(file_, "ENTRY pc=0x%06x frame=%llu hits=%llu total=%llu\n", addresses_[i],
                     static_cast<unsigned long long>(frame), static_cast<unsigned long long>(total - last_[i]),
                     static_cast<unsigned long long>(total));
        last_[i] = total;
        wrote = true;
    }
    if (wrote) std::fflush(file_);
}

IndirectWatch::IndirectWatch(const std::filesystem::path &path, BlockProfileSession &session)
    : session_(session), path_(path.empty() ? path : std::filesystem::absolute(path)) {
#if !defined(F3_PROFILE_INSTRUMENT) || !F3_PROFILE_INSTRUMENT
    throw std::runtime_error("--indirect-log needs a -DF3_PROFILE_INSTRUMENT=ON build");
#endif
    if (session.path().empty()) throw std::runtime_error("--indirect-log needs --profile-out FILE (the counters live there)");
    if (path_.empty()) throw std::runtime_error("--indirect-log needs a non-empty FILE");
    if (!path_.parent_path().empty()) std::filesystem::create_directories(path_.parent_path());
    active_indirect = this;
    f3_profile_indirect_enabled = 1;
    session_.register_indirect_watch(this);
    flush(); // An initial atomic file also proves the destination is writable.
}

IndirectWatch::~IndirectWatch() {
    session_.register_indirect_watch(nullptr);
    f3_profile_indirect_enabled = 0;
    active_indirect = nullptr;
    try { flush(); }
    catch (const std::exception &error) { std::fprintf(stderr, "INDIRECT LOG FLUSH FAILED: %s\n", error.what()); }
}

void IndirectWatch::record(uint32_t site, uint32_t target) {
    const uint64_t key = (static_cast<uint64_t>(site) << 32) | target;
    auto &count = counts_[key];
    if (count != UINT64_MAX) ++count;
}

void IndirectWatch::flush() {
    if (path_.empty()) return;
    const std::filesystem::path temporary = path_.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file) throw std::runtime_error("Cannot open indirect log: " + temporary.string() + ": " + std::strerror(errno));
        file << "# f3rt indirect targets v1\n";
        struct Item {
            uint32_t site;
            uint32_t target;
            uint64_t count;
        };
        std::vector<Item> items;
        items.reserve(counts_.size());
        for (const auto &[key, count] : counts_) {
            items.push_back({static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key & 0xffffffffu), count});
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
        if (!file) throw std::runtime_error("Indirect log write failed: " + temporary.string() + ": " + std::strerror(errno));
    }
    sync_file(temporary);
    std::filesystem::rename(temporary, path_);
}
}
extern "C" void f3_profile_indirect(uint32_t site, uint32_t target) {
    if (active_indirect) active_indirect->record(site, target);
}
extern "C" [[noreturn]] void f3_profile_cold_abort(unsigned region, uint32_t crc, uint32_t address) {
    const auto log_path = active ? active->path().string() + ".cold-hits" : "f3-cold-hits.log";
    char message[384];
    std::snprintf(message, sizeof(message),
        "F3 PROFILE SLIM ABORT: removed %s block address=0x%08x ROM_CRC=0x%08x; re-profile with a full -DF3_PROFILE_INSTRUMENT=ON build and --profile-out FILE",
        region < 2 ? names[region] : "unknown", address, crc);
    std::fprintf(stderr, "%s\n", message);
    FILE *log = std::fopen(log_path.c_str(), "a");
    if (log) {
        const bool failed = std::fprintf(log, "%s\n", message) < 0 || std::fflush(log) || fsync(fileno(log));
        std::fclose(log);
        if (failed) std::fprintf(stderr, "COLD-HIT RECORD FAILED: %s\n", log_path.c_str());
        else std::fprintf(stderr, "Cold hit recorded: %s\n", log_path.c_str());
    } else std::fprintf(stderr, "COLD-HIT RECORD FAILED: %s\n", log_path.c_str());
    if (active) {
        try { active->record_miss(region, crc, address); }
        catch (const std::exception &error) { std::fprintf(stderr, "COLD-HIT PROFILE FAILED: %s\n", error.what()); }
    }
    throw std::runtime_error(message);
}
