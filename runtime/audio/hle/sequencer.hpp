#pragma once
#include "audio/hle/events.hpp"
#include <cstdint>
#include <memory>
#include <span>

namespace f3rt::hle {
class Sequencer {
public:
    Sequencer(std::span<const uint8_t> sound_rom, VoiceSink &sink);
    ~Sequencer();
    Sequencer(const Sequencer &) = delete;
    Sequencer &operator=(const Sequencer &) = delete;
    void command(std::span<const uint8_t> packet, uint64_t command_instance, uint64_t tick);
    void advance_to(uint64_t tick);
    void reset(uint64_t tick);
    void set_bank(uint8_t bank, uint64_t tick);
    uint64_t next_tick() const;
    static bool validate(std::span<const uint8_t> sound_rom) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace f3rt::hle
