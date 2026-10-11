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
    uint64_t next_tick() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace f3rt::hle
