#pragma once
#include "f3rt/netplay_transport.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace f3rt { class Machine; }
namespace f3rt::netplay {
// Active-high U/D/L/R, button 1/2/3, start, coin, service, test.
using InputWord = uint16_t;
constexpr InputWord input_mask = 0x7ff;
void apply_inputs(Machine &, const std::array<InputWord, 2> &);
Identity machine_identity(const Machine &);
// Japan 2.01J: $401f6e is the versus latch; $401f53 bits 6/7 identify selection.
bool versus_handoff_ready(const Machine &);
bool versus_match_active(const Machine &);

class Rollback {
public:
    static constexpr unsigned max_window = 32;
    static constexpr unsigned history_size = 1024;
    static constexpr unsigned checksum_interval = 60;
    // Match-relative frames; the machine retains the host snapshot's absolute time.
    // Snapshots/history/audio are allocated here, never per frame.
    Rollback(Machine &, unsigned slot, unsigned delay = 2, unsigned window = 16);
    ~Rollback();
    Rollback(const Rollback &) = delete;
    Rollback &operator=(const Rollback &) = delete;
    uint32_t frame() const;
    uint64_t origin_frame() const;
    // Exclusive end of simulated frames with both players' actual inputs.
    uint32_t confirmed_frame() const;
    bool match_finished() const;
    bool needs_local_input() const;
    Input local_input(InputWord);
    void receive(Input);
    void receive_checksum(Checksum);
    // Correct predictions and promote confirmed outputs, without advancing time.
    void synchronize();
    // Advance one frame, or return false at the bounded prediction window.
    bool advance();
    bool receive_checksum_to_send(Checksum &);
    // Only confirmed PCM. Must be drained while running (including stalls).
    size_t render_audio(int16_t *stereo, size_t max_frames);
    // End this rollback timeline at its last confirmed state before returning solo.
    void restore_confirmed();
    uint64_t rollback_count() const;
    unsigned last_rollback_depth() const;
    unsigned maximum_rollback_depth() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace f3rt::netplay
