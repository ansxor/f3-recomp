#pragma once
#include "audio/hle/events.hpp"
#include "audio/hle/observe.hpp"
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace f3rt::hle {
// Filter-table index of a direct (key-region) voice, as the sound driver computes it at c195a4-c195d0:
// d0 = (patch[0xe] & 15) * word8; d0 = (d0 << 7) >> 16; d0 += patch[0xe] * 4. word8 is voice +8, which the
// note-on routine (c17baa-c17bb2) fills with the note velocity (bit 7 cleared) in both bytes, i.e.
// velocity * 257. The driver then clamps the table byte offset 2 * index to 0x7fe.
inline int direct_filter_index(uint8_t patch_e, uint8_t velocity) {
    const int word8 = int(velocity & 127) * 257;
    return patch_e * 4 + ((patch_e & 15) * word8 >> 9);
}
// ROM-data synthesis, independent of sound CPU and sound-device execution.
// Input spans must outlive Synth. Output is four interleaved stereo buses.
class Synth {
public:
    Synth(std::span<const uint8_t> sound_rom, std::span<const uint16_t> sample_words);
    ~Synth();
    Synth(const Synth &) = delete;
    Synth &operator=(const Synth &) = delete;
    void note_on(const Note &note);
    void note_off(uint64_t instance, uint64_t tick);
    void channel_update(uint8_t sequence, uint8_t track, const Channel &channel, uint64_t tick);
    void reset(uint64_t tick);
    void set_bank(uint8_t bank);
    void render(float *interleaved_eight_channels, size_t frames);
    void set_observer(std::function<void(const VoiceEvent &)> observer);
    std::optional<ProgramInfo> describe_program(uint16_t program) const;
    uint8_t bend_range() const;
    static bool validate(std::span<const uint8_t> sound_rom,
                         std::span<const uint16_t> sample_words) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace f3rt::hle
