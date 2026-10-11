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

// ---- landmakrj work-RAM addresses ----
// Synced ranges
constexpr uint32_t CREDITS_COIN_BEGIN = 0x400090;       // Credits (0x400090), partial coins (0x400092/0x40009e), coin debounce (0x400096/0x40009a)
constexpr size_t CREDITS_COIN_SIZE = 16;                // ..0x40009f
constexpr uint32_t OPERATOR_SETTINGS_BEGIN = 0x4000cb;  // Coinage, free play, difficulty/2P enable, cabinet, coin mode, continue, demo sound, free play config, options
constexpr size_t OPERATOR_SETTINGS_SIZE = 13;           // ..0x4000d7
constexpr uint32_t GAMEPLAY_BLOCK_BEGIN = 0x401f40;     // mode block, both player structures, piece queues, RNG seeds, task latch
constexpr size_t GAMEPLAY_BLOCK_SIZE = 496;             // ..0x40212f
constexpr uint32_t GAMEPLAY_BLOCK_LAST = GAMEPLAY_BLOCK_BEGIN + GAMEPLAY_BLOCK_SIZE - 1; // 0x40212f, inclusive
constexpr uint32_t MATCH_ROUNDS_WORD = 0x40fc58;        // Match rounds setting word
constexpr size_t MATCH_ROUNDS_WORD_SIZE = 2;            // ..0x40fc59

// Gameplay-block bytes excluded from the hash (cosmetic / pre-init)
constexpr uint32_t ATTRACT_TIMER_A = 0x401f85;          // attract-mode timer (-$607c(a5)) in inter-structure padding
constexpr uint32_t ATTRACT_TIMER_B = 0x401fa1;          // attract-mode timer (-$6060(a5)) in inter-structure padding
constexpr uint32_t P1_ANIM_TICK = 0x402033;             // P1 sprite/palette frame animation tick counter (0..40 modulo 41)
constexpr uint32_t P2_ANIM_TICK = 0x4020db;             // P2 sprite/palette frame animation tick counter (0..40 modulo 41)
constexpr uint32_t TRAILING_MATCH_TIMER = 0x4020f7;     // trailing match timer (-$5f0c(a5), attract residue cleared by sub_093462)

// Versus / lobby state
constexpr uint32_t VERSUS_LATCH_WORD = 0x401f6e;        // 1 = versus active, nonzero = lobby trigger
constexpr uint32_t LOBBY_LINK_WORD = 0x401f66;          // 0xffff when the menu requests a link
constexpr uint32_t LOBBY_MENU_STATE_BYTE = 0x407902;
constexpr uint32_t LOBBY_FLASH_COUNTER_BYTE = 0x407903;
constexpr uint32_t LOBBY_AUX_BYTE = 0x407904;
constexpr uint32_t LOBBY_TIMER_WORD = 0x407908;
constexpr uint16_t LOBBY_TIMER_DEFAULT = 0x04b0;

constexpr uint32_t MATCH_RNG_SEED_LONG = 0x400826;      // reseeded every match frame
constexpr uint32_t ROUND_WINS_P1_WORD = 0x401f6a;       // P1 round wins; with P2's gives the round index
constexpr uint32_t ROUND_WINS_P2_WORD = 0x401f6c;

struct RamRange {
    uint32_t addr;
    size_t size;
};

constexpr RamRange sync_ranges[] = {
    {CREDITS_COIN_BEGIN, CREDITS_COIN_SIZE},
    {OPERATOR_SETTINGS_BEGIN, OPERATOR_SETTINGS_SIZE},
    {GAMEPLAY_BLOCK_BEGIN, GAMEPLAY_BLOCK_SIZE},
    {MATCH_ROUNDS_WORD, MATCH_ROUNDS_WORD_SIZE},
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
    return m.read16(VERSUS_LATCH_WORD) == 1;
}

bool lobby_trigger(Machine &m) {
    return (m.read8(LOBBY_MENU_STATE_BYTE) == 1 && m.read16(LOBBY_LINK_WORD) == 0xffff) || m.read16(VERSUS_LATCH_WORD) != 0;
}

enum class HoldPath { None, Menu, Charsel };
static HoldPath hold_path = HoldPath::None;
// LOBBY_FLASH_COUNTER_BYTE pinned during the Menu hold: the flash overlay is drawn only when counter <= 0x20 and
// (counter & 4) == 0, and the screen exits at > 0x40. 0x30 draws no overlay and never reaches the exit.
constexpr uint8_t hold_flash_pin = 0x30;
static uint16_t hold_timer = LOBBY_TIMER_DEFAULT;
static uint8_t hold_menu_state = 0;
static uint8_t hold_aux = 0;

void lobby_hold_begin(Machine &m) {
    if (m.read16(VERSUS_LATCH_WORD) == 0) {
        hold_path = HoldPath::Menu;
        hold_timer = m.read16(LOBBY_TIMER_WORD);
    } else {
        hold_path = HoldPath::Charsel;
        hold_timer = m.read16(LOBBY_TIMER_WORD);
        hold_menu_state = m.read8(LOBBY_MENU_STATE_BYTE);
        hold_aux = m.read8(LOBBY_AUX_BYTE);
    }
}

void lobby_hold_frame(Machine &m) {
    if (hold_path == HoldPath::Menu) {
        m.write8(LOBBY_MENU_STATE_BYTE, 1);
        m.write8(LOBBY_FLASH_COUNTER_BYTE, hold_flash_pin);
        m.write16(LOBBY_TIMER_WORD, hold_timer);
        m.write16(VERSUS_LATCH_WORD, 0);
    } else if (hold_path == HoldPath::Charsel) {
        m.write16(LOBBY_TIMER_WORD, hold_timer);
        m.write8(LOBBY_MENU_STATE_BYTE, hold_menu_state);
        m.write8(LOBBY_AUX_BYTE, hold_aux);
    }
}

void lobby_cancel(Machine &m) {
    if (hold_path == HoldPath::Menu) {
        m.write8(LOBBY_MENU_STATE_BYTE, 0);
        m.write8(LOBBY_FLASH_COUNTER_BYTE, 0);
        m.write16(LOBBY_TIMER_WORD, LOBBY_TIMER_DEFAULT);
        m.write16(LOBBY_LINK_WORD, 0);
        m.write16(VERSUS_LATCH_WORD, 0);
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

// Reseeds the global RNG word before every lockstepped frame. The value depends only on the server seed and the round
// index (round wins P1 + P2, synced RAM that is updated before round_task picks the stage), so it is constant within
// a round and a pure function of machine state: rollback restores the key with the snapshot.
// Evidence: docs/notes/raw/tests/landmakrj-differential-sync-experiment.md; round-index timing from a 2P run
// (stage pick 0x08e470 at frames 2631/4983/11043 with wins sum 0/1/2; bag init sub_093462 ~12 frames later).
void on_match_frame(Machine &m, uint64_t seed, uint32_t /*match_frame*/) {
    const uint32_t round_index = uint32_t(m.read16(ROUND_WINS_P1_WORD)) + m.read16(ROUND_WINS_P2_WORD);
    uint64_t key = seed ^ (uint64_t(round_index) * 0x100000001b3ULL);
    m.write32(MATCH_RNG_SEED_LONG, splitmix32(key));
}

// sync_hash: CRC32 over the synchronized gameplay RAM state, ungated (every report, from match frame 1).
// Evidence: docs/notes/raw/tests/landmakrj-differential-sync-experiment.md and
//           docs/notes/raw/tests/landmakrj-desync-695b-frame540.md (full-block sync).
// The hashed bytes are, in this order:
//   1. the 496 B gameplay block 0x401f40..0x40212f minus five cosmetic/pre-init bytes (491 B):
//        - ATTRACT_TIMER_A, ATTRACT_TIMER_B, P1_ANIM_TICK, P2_ANIM_TICK, TRAILING_MATCH_TIMER
//   2. the other synced ranges: credits (16 B), operator settings (13 B), match rounds word (2 B).
// Total 522 B. The whole block is synced (v4), so the player-structure residue that used to leak into gameplay when
// the old two-phase gate flipped (0x40206c, 0x402078, 0x4020f4..f7) is equal on both peers and no gate is needed.
// `phase` is always 1; it stays in the interface for bundles written by older builds (phase 2 = gated 491 B block).
constexpr size_t hash_region_max = sync_payload_size; // 527 >= 522 hashed bytes

inline bool gameplay_excluded(uint32_t a) {
    return a == ATTRACT_TIMER_A || a == ATTRACT_TIMER_B || a == P1_ANIM_TICK || a == P2_ANIM_TICK ||
           a == TRAILING_MATCH_TIMER;
}

// Calls f(addr) for every hashed byte in hash order.
template <typename F>
inline void for_each_hashed_address(F &&f) {
    for (uint32_t a = GAMEPLAY_BLOCK_BEGIN; a <= GAMEPLAY_BLOCK_LAST; ++a) {
        if (!gameplay_excluded(a)) f(a);
    }
    for (const auto &r : sync_ranges) {
        if (r.addr == GAMEPLAY_BLOCK_BEGIN) continue;
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
        for (uint32_t a = GAMEPLAY_BLOCK_BEGIN; a <= GAMEPLAY_BLOCK_LAST && offset < dst.size(); ++a) {
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
