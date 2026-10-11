#pragma once
#include <cstdint>
#include <cstddef>
#include <span>

namespace f3rt {
class Machine;

struct GameNetplay {
    // True when the guest game has committed to 2-player versus mode.
    // The generic frontend detects the 0 -> 1 transition edge.
    bool (*in_versus)(Machine &) = nullptr;

    // Optional: host captures game sync blob (returns bytes written, 1..2048)
    size_t (*capture_sync)(Machine &, std::span<uint8_t>) = nullptr;

    // Optional: joiner applies game sync blob
    void (*apply_sync)(Machine &, std::span<const uint8_t>, uint64_t seed) = nullptr;

    // Optional: called on BOTH peers before every lockstepped run_frame
    void (*on_match_frame)(Machine &, uint64_t seed, uint32_t match_frame) = nullptr;

    // Optional: if present, CRC reports use this instead of Machine::state_crc
    uint32_t (*sync_hash)(Machine &) = nullptr;

    // Optional: writes the exact bytes the desync hash covers into dst and returns the count.
    // `phase` receives a small game-defined id (Land Maker: 1 = synced ranges, 2 = full gameplay block).
    // When present, the netplay step function hashes these bytes (crc32) so the recorded copy cannot drift from the report.
    size_t (*sync_hash_region)(Machine &, std::span<uint8_t>, uint8_t &phase) = nullptr;

    // Optional, offline tooling only: emits the Machine address of every byte sync_hash_region returns for `phase`.
    size_t (*sync_hash_addresses)(uint8_t phase, std::span<uint32_t>) = nullptr;

    // Optional: opens the frontend lobby on false -> true edge.
    // If null, frontend falls back to in_versus.
    bool (*lobby_trigger)(Machine &) = nullptr;

    // Optional: called once at the versus trigger (after snapshot) to record reference values for the hold
    void (*lobby_hold_begin)(Machine &) = nullptr;

    // Optional: called every lobby frame around run_frame while waiting in the lobby
    // to pin game logic state to trigger values while animations and music continue.
    void (*lobby_hold_frame)(Machine &) = nullptr;

    // Optional: called on Cancel Netplay / lobby close to un-commit the selection and return to menu
    void (*lobby_cancel)(Machine &) = nullptr;
};

const GameNetplay *game_netplay(); // nullptr when the game provides none

} // namespace f3rt
