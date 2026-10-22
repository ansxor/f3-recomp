#pragma once

// Main-CPU sound capture shared by the sound-extract and music-export tools.
//
// A game turns a sound code into mailbox packets by running its own request routine on the main
// CPU. Everything here drives those routines in the interpreter from a frozen post-boot machine
// and records the exact ring bytes they produce; nothing re-encodes packets.

#include "profile.hpp"
#include "runner.hpp"
#include "f3rt/machine.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace f3rt::runner {

// Text helpers for tool arguments and index files.
// Decimal, 0x or $ hex; throws on anything else, a sign, or a value above 32 bits.
uint32_t parse_number(std::string_view text);
// "N" or "FIRST-LAST" (inclusive); throws when FIRST exceeds LAST.
std::pair<uint32_t, uint32_t> parse_range(std::string_view text);
std::string hex_bytes(std::span<const uint8_t> bytes);

// Runs a main-CPU routine to its return in the interpreter. The machine must be frozen at a frame
// boundary; restoring a snapshot afterwards removes every side effect on game RAM. Throws when
// the routine does not return.
struct RoutineCall {
    uint32_t address = 0;
    std::optional<uint16_t> stack_word;
    std::optional<uint16_t> d0;
};
void run_routine(Machine &m, const RoutineCall &call);

// Bytes appended to the mailbox ring between two producer snapshots (doubled indices).
std::vector<uint8_t> ring_bytes_between(const Machine &m, uint16_t before, uint16_t after);

struct CodeCall {
    uint32_t code;
    std::vector<uint8_t> bytes;
};

// Requests `code` through the game's request routine and returns the exact ring bytes it produced.
CodeCall request_code(Machine &m, uint32_t routine, uint32_t code);

// Interpreted boot of `cfg` for `boot_frames` frames, then the sound side is run until it has
// consumed every pending packet: the machine is frozen at a frame boundary in the post-boot state.
// `record_boot_wav` feeds the boot audio to the runner's WAV/statistics. Throws when the main
// CPU halts, the sound CPU stays in reset, the consumer cannot catch up, or the enhanced backend
// is selected but unsupported for the game.
std::unique_ptr<MachineRunner> boot_to_freeze(const RunnerConfig &cfg, uint64_t boot_frames,
                                              bool record_boot_wav);

// Machine state at the freeze point. `boot_commands` is the number of packets the enhanced engine
// parsed during boot (0 for the reference backend).
struct PostBootSnapshot {
    std::vector<uint8_t> state;
    uint64_t boot_commands = 0;
};
PostBootSnapshot take_post_boot_snapshot(Machine &m);

// The boot packet stream: mailbox ring bytes 0..producer, length-prefixed packets (first byte of
// each packet is its total length, including that byte). A reset pulse rewinds the HLE read pointer
// to ring byte 0, so these packets are re-parsed together with the next request. Throws unless the
// ring still holds the whole stream, i.e. its packet count equals `boot_commands`. The span views
// the machine's shared RAM: copy it before running anything on the machine.
std::span<const uint8_t> boot_packet_stream(const Machine &m, uint64_t boot_commands);

// Puts `runner` into the snapshot state. The enhanced engine runs on a worker outside the machine
// snapshot, and a reused worker renders the same request differently (absolute clock and DSP
// history carry over), so the enhanced backend gets a new runner (hence a new worker) built from
// `cfg`, restored from the snapshot, then a reset pulse re-parses the boot stream. The reference
// backend restores in place. Leaves the main CPU cycle count at the audio clock.
void restore_post_boot(std::unique_ptr<MachineRunner> &runner, const RunnerConfig &cfg,
                       const PostBootSnapshot &snapshot);

// Restores the snapshot into `m` itself and holds the sound side in reset, for runs that only capture the
// main CPU's mailbox bytes (request routines, sound-test steps) and never render: while held the engine does
// not parse the ring, so nothing but the main CPU runs and the call is cheap enough for thousands of codes.
// Leaves the main CPU cycle count at the audio clock.
void restore_for_capture(Machine &m, const PostBootSnapshot &snapshot);

// Splits ring bytes into length-prefixed packets (first byte = total length including itself, >= 2).
// Throws std::runtime_error on a length that is < 2 or runs past the end.
std::vector<std::vector<uint8_t>> split_packets(std::span<const uint8_t> bytes);

// Sound-test BANK switch: the main-bus bank-switch write at MAIN_SOUND_BANKSWITCH_BASE + 2 * bank
// (bank 1..31), then the profile's bank RAM words. `profile` may be null (no bank words).
void apply_sound_bank(Machine &m, const SoundProfile *profile, uint32_t bank);

// What the game's own SOUND TEST sequence does for one code.
struct SoundTestCapture {
    int category = -1;               // routine result d1; -1 when no category routine ran
    int descriptor_type = -1;        // first byte of the selector descriptor; -1 when not read
    std::vector<uint8_t> descriptor; // 8 bytes of the selector descriptor
    std::vector<CodeCall> calls;     // prelude setup codes (if any), then `code`
};

// Reads the selector descriptor of `code`, runs the category routine and, for categories <= 1 unless
// `no_prelude`, requests the two prelude setup codes the sound test derives from `code`, and finally
// requests `code` itself. `profile` null skips the descriptor/category/prelude steps and only
// requests `code`. `out` accumulates as it goes, so an exception leaves the steps that completed.
void capture_sound_test(Machine &m, const SoundProfile *profile, uint32_t request_routine,
                        uint32_t code, bool no_prelude, SoundTestCapture &out);

} // namespace f3rt::runner
