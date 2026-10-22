#pragma once
// F3RP replay script: the engine-visible effect of the game's sound-test request. One implementation of the format:
// encode_replay() writes it, replay() reads it and drives a fresh Core.
//
//   u8[4] "F3RP"  u8 version = 1  u8 reserved = 0  u16le item_count
//   items: u8 op, payload
//     0x01 RESET                           core.reset(0)
//     0x02 GAIN    u8 channel(0|1) u8 raw  core.set_gain(channel, raw, 0)
//     0x03 BANK    u8 page                 core.set_bank(page, 0)
//     0x04 PACKET  u8 n(2..8) n bytes      core.command(bytes, ++serial, 0); bytes[0] == n
//
// Every item runs at worker tick 0 and no time passes while a script is applied.
#include "audio/hle/core.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace f3rt::hle {

inline constexpr uint8_t REPLAY_VERSION = 1;
inline constexpr size_t REPLAY_MAX_PACKET = 8; // the engine drops longer mailbox packets

struct ReplayScript {
    std::array<uint8_t, 2> gain_raw{};           // mailbox bytes 0x7fa, 0x7fb at the post-boot freeze
    std::optional<uint8_t> page;                 // page delivered to set_sound_bank by the sound-test BANK write
    std::vector<std::vector<uint8_t>> packets;   // boot stream packets, then the request calls' packets; each [n, op, ...]
};

// Item order: RESET, GAIN 0, GAIN 1, [BANK], PACKET... Throws std::invalid_argument when a packet is not
// 2..REPLAY_MAX_PACKET bytes long with bytes[0] == size, or when there are more than 65535 items.
std::vector<uint8_t> encode_replay(const ReplayScript &script);

// Applies `script` to a freshly constructed Core (tick() == 0 && frames() == 0). The whole script is validated
// before anything is applied. Throws std::invalid_argument for a malformed script or a used Core; exceptions of the
// Core calls propagate.
void replay(Core &core, std::span<const uint8_t> script);

} // namespace f3rt::hle
