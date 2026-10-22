#pragma once
#include "audio/hle/events.hpp"
#include "audio/hle/mailbox.hpp"
#include "audio/hle/pulse_timer.hpp"
#include "audio/hle/track_service.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace f3rt::hle {
// Reads the ROM sequence streams and turns them into mailbox events. It owns only sequencing position and tempo:
// the track records live in TrackService, and the mailbox carries every note and parameter write to the services.
class Sequencer : public PulseTimer::Listener {
public:
    Sequencer(std::span<const uint8_t> sound_rom, Mailbox &mailbox, TrackService &tracks);
    ~Sequencer();
    Sequencer(const Sequencer &) = delete;
    Sequencer &operator=(const Sequencer &) = delete;
    void command(std::span<const uint8_t> packet, uint64_t command_instance, uint64_t tick);
    void reset(uint64_t tick);
    void set_bank(uint8_t bank, uint64_t tick);
    // The per-sequence tempo pulses. Step 0 while the sequence is not playing.
    std::array<PulseTimer, SONG_SLOTS> &beats() noexcept;
    // PulseTimer::Listener: `tag` is the sequence id.
    void pulse(uint64_t tick, uint8_t tag) override;
    static bool validate(std::span<const uint8_t> sound_rom) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace f3rt::hle
