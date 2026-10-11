// Land Maker (landmakrj) 2P versus mode trigger hook and differential state sync.
// Evidence:
//   - Trigger latch (0x401f6e): docs/notes/routines/landmakrj-0008edde-versus-mode-transition.md
//   - Differential sync set & RNG reseed: docs/notes/raw/tests/landmakrj-differential-sync-experiment.md
//   - Whole gameplay block sync (v4): docs/notes/raw/tests/landmakrj-desync-695b-frame540.md. With the v3 sub-ranges the
//     player-structure residue left by different pre-trigger attract/charsel paths stayed unsynced and leaked into gameplay
//     when the player structures were initialised (DESYNC at 0x402078 / 0x40206c / 0x4020f4..f7).
//   - Credits & coin subsystem (0x400090..0x40009f, 16 B): docs/notes/raw/tests/landmakrj-differential-sync-experiment.md
//   - Operator settings & options (0x4000cb..0x4000d7, 13 B): coinage, free play, difficulty/2P enable, cabinet, coin mode, continue, demo sound, free-play config, options
//   - Screen flip (0x40013e) intentionally excluded: presentation-only orientation latch.
#include "f3rt/game_netplay.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/rom.hpp"
#include <numeric>
#include <stdexcept>

namespace f3rt {
namespace {

struct RamRange {
    uint32_t addr;
    size_t size;
};

constexpr RamRange sync_ranges[] = {
    {0x400090,  16}, // 0x400090..0x40009f (16 B): Credits (0x400090), partial coins (0x400092/0x40009e), coin debounce (0x400096/0x40009a)
    {0x4000cb,  13}, // 0x4000cb..0x4000d7 (13 B): Coinage, free play, difficulty/2P enable, cabinet, coin mode, continue, demo sound, free play config, options
    {0x401f40, 496}, // 0x401f40..0x40212f (496 B): whole gameplay block: mode block, both player structures, piece queues, RNG seeds, task latch
    {0x40fc58,   2}, // 0x40fc58..0x40fc59 (2 B): Match rounds setting word
};

constexpr size_t calculate_sync_payload_size() {
    size_t total = 0;
    for (const auto &r : sync_ranges) {
        total += r.size;
    }
    return total;
}

constexpr uint8_t sync_blob_version = 0x04;
constexpr size_t sync_payload_size = calculate_sync_payload_size(); // 527 bytes
constexpr size_t total_sync_blob_size = 1 + sync_payload_size;     // 528 bytes

[[maybe_unused]] inline bool in_sync_ranges(uint32_t a) {
    for (const auto &r : sync_ranges) {
        if (a >= r.addr && a < r.addr + r.size) {
            return true;
        }
    }
    return false;
}

bool versus_active(Machine &m) {
    return m.read16(0x401f6e) == 1;
}

bool lobby_trigger(Machine &m) {
    return (m.read8(0x407902) == 1 && m.read16(0x401f66) == 0xffff) || m.read16(0x401f6e) != 0;
}

enum class HoldPath { None, Menu, Charsel };
static HoldPath hold_path = HoldPath::None;
// 0x407903 flash counter pinned during the Menu hold: the flash overlay is drawn only when counter <= 0x20 and
// (counter & 4) == 0, and the screen exits at > 0x40. 0x30 draws no overlay and never reaches the exit.
constexpr uint8_t hold_flash_pin = 0x30;
static uint16_t hold_timer = 0x04b0;
static uint8_t hold_407902 = 0;
static uint8_t hold_407904 = 0;

void lobby_hold_begin(Machine &m) {
    if (m.read16(0x401f6e) == 0) {
        hold_path = HoldPath::Menu;
        hold_timer = m.read16(0x407908);
    } else {
        hold_path = HoldPath::Charsel;
        hold_timer = m.read16(0x407908);
        hold_407902 = m.read8(0x407902);
        hold_407904 = m.read8(0x407904);
    }
}

void lobby_hold_frame(Machine &m) {
    if (hold_path == HoldPath::Menu) {
        m.write8(0x407902, 1);
        m.write8(0x407903, hold_flash_pin);
        m.write16(0x407908, hold_timer);
        m.write16(0x401f6e, 0);
    } else if (hold_path == HoldPath::Charsel) {
        m.write16(0x407908, hold_timer);
        m.write8(0x407902, hold_407902);
        m.write8(0x407904, hold_407904);
    }
}

void lobby_cancel(Machine &m) {
    if (hold_path == HoldPath::Menu) {
        m.write8(0x407902, 0);
        m.write8(0x407903, 0);
        m.write16(0x407908, 0x04b0);
        m.write16(0x401f66, 0);
        m.write16(0x401f6e, 0);
    }
    hold_path = HoldPath::None;
}

size_t capture_sync(Machine &m, std::span<uint8_t> dst) {
    if (dst.size() < total_sync_blob_size) {
        throw std::runtime_error("capture_sync destination buffer too small");
    }
    dst[0] = sync_blob_version;
    size_t offset = 1;
    for (const auto &r : sync_ranges) {
        for (size_t i = 0; i < r.size; ++i) {
            dst[offset++] = m.read8(r.addr + uint32_t(i));
        }
    }
    return total_sync_blob_size;
}

void apply_sync(Machine &m, std::span<const uint8_t> src, uint64_t seed) {
    (void)seed;
    if (src.size() != total_sync_blob_size || src[0] != sync_blob_version) {
        throw std::runtime_error("Invalid sync blob size or version");
    }
    size_t offset = 1;
    for (const auto &r : sync_ranges) {
        for (size_t i = 0; i < r.size; ++i) {
            m.write8(r.addr + uint32_t(i), src[offset++]);
        }
    }
}

// splitmix32: standard splitmix64 finalizer truncated to 32 bits
// Evidence: docs/notes/raw/tests/landmakrj-differential-sync-experiment.md (Spec 5/6C)
inline uint32_t splitmix32(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return uint32_t(x ^ (x >> 31));
}

void on_match_frame(Machine &m, uint64_t seed, uint32_t match_frame) {
    uint64_t key = seed ^ (uint64_t(match_frame) * 0x100000001b3ULL);
    m.write32(0x400826, splitmix32(key));
}

// sync_hash: CRC32 over the synchronized gameplay RAM state, ungated (every report, from match frame 1).
// Evidence: docs/notes/raw/tests/landmakrj-differential-sync-experiment.md and
//           docs/notes/raw/tests/landmakrj-desync-695b-frame540.md (full-block sync).
// The hashed bytes are, in this order:
//   1. the 496 B gameplay block 0x401f40..0x40212f minus five cosmetic/pre-init bytes (491 B):
//        - 0x401f85: attract-mode timer (-$607c(a5)) in inter-structure padding
//        - 0x401fa1: attract-mode timer (-$6060(a5)) in inter-structure padding
//        - 0x402033: P1 sprite/palette frame animation tick counter (0..40 modulo 41)
//        - 0x4020db: P2 sprite/palette frame animation tick counter (0..40 modulo 41)
//        - 0x4020f7: trailing match timer (-$5f0c(a5), attract residue cleared by sub_093462)
//   2. the other synced ranges: credits (16 B), operator settings (13 B), match rounds word (2 B).
// Total 522 B. The whole block is synced (v4), so the player-structure residue that used to leak into gameplay when
// the old two-phase gate flipped (0x40206c, 0x402078, 0x4020f4..f7) is equal on both peers and no gate is needed.
// `phase` is always 1; it stays in the interface for bundles written by older builds (phase 2 = gated 491 B block).
constexpr uint32_t gameplay_block_begin = 0x401f40;
constexpr uint32_t gameplay_block_end = 0x40212f; // inclusive
constexpr size_t hash_region_max = sync_payload_size; // 527 >= 522 hashed bytes

inline bool gameplay_excluded(uint32_t a) {
    return a == 0x401f85 || a == 0x401fa1 || a == 0x402033 || a == 0x4020db || a == 0x4020f7;
}

// Calls f(addr) for every hashed byte in hash order.
template <typename F>
inline void for_each_hashed_address(F &&f) {
    for (uint32_t a = gameplay_block_begin; a <= gameplay_block_end; ++a) {
        if (!gameplay_excluded(a)) f(a);
    }
    for (const auto &r : sync_ranges) {
        if (r.addr == gameplay_block_begin) continue;
        for (size_t i = 0; i < r.size; ++i) f(r.addr + uint32_t(i));
    }
}

size_t sync_hash_region(Machine &m, std::span<uint8_t> dst, uint8_t &phase) {
    phase = 1;
    if (dst.size() < hash_region_max) throw std::runtime_error("sync_hash_region buffer too small");
    size_t offset = 0;
    for_each_hashed_address([&](uint32_t a) { dst[offset++] = m.read8(a); });
    return offset;
}

size_t sync_hash_addresses(uint8_t phase, std::span<uint32_t> dst) {
    size_t offset = 0;
    if (phase == 2) {
        // Legacy gated block written by builds before v4.
        for (uint32_t a = gameplay_block_begin; a <= gameplay_block_end && offset < dst.size(); ++a) {
            if (!gameplay_excluded(a)) dst[offset++] = a;
        }
        return offset;
    }
    for_each_hashed_address([&](uint32_t a) {
        if (offset < dst.size()) dst[offset++] = a;
    });
    return offset;
}

uint32_t sync_hash(Machine &m) {
    uint8_t buf[hash_region_max];
    uint8_t phase = 0;
    const size_t n = sync_hash_region(m, buf, phase);
    return f3rt::crc32(buf, n);
}

const GameNetplay netplay_hook{
    .in_versus = &versus_active,
    .capture_sync = &capture_sync,
    .apply_sync = &apply_sync,
    .on_match_frame = &on_match_frame,
    .sync_hash = &sync_hash,
    .sync_hash_region = &sync_hash_region,
    .sync_hash_addresses = &sync_hash_addresses,
    .lobby_trigger = &lobby_trigger,
    .lobby_hold_begin = &lobby_hold_begin,
    .lobby_hold_frame = &lobby_hold_frame,
    .lobby_cancel = &lobby_cancel,
};

} // namespace

const GameNetplay *game_netplay() { return &netplay_hook; }

} // namespace f3rt
