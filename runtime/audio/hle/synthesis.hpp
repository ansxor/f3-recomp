#pragma once
#include "audio/hle/block.hpp"
#include "audio/hle/sound_bank.hpp"
#include "audio/hle/voice_filter.hpp"
#include "audio/hle/voice_service.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace f3rt::hle {

inline constexpr size_t VOICE_BLOCK_FRAMES = 64;

struct VoiceRenderState {
    std::array<VoiceFilterState, 32> filters;
    std::array<uint32_t, 32> filter_epoch{};
    std::vector<uint64_t> frame_ticks;
    alignas(64) std::array<float, VOICE_BLOCK_FRAMES * 8> discard{};
    uint64_t tick = 0;
    uint32_t tick_remainder = 0;

    VoiceRenderState() { frame_ticks.reserve(1024); }
};

// Renders one voice into its OTIS bus pair over [frame_start, frame_end) of `buses`.
void render_voice(VoiceService &voices, Voice &voice, SoundBank &bank, VoiceRenderState &state,
                  MutableAudioBlockSpan<8> buses, size_t frame_start, size_t frame_end,
                  std::span<const uint64_t> frame_ticks);

// Renders every active voice of the pool, in slot order.
void render_voices(VoiceService &voices, SoundBank &bank, VoiceRenderState &state,
                   MutableAudioBlockSpan<8> buses, size_t frame_start, size_t frame_end,
                   std::span<const uint64_t> frame_ticks);

// Restarts the frame -> tick map. The filter memory of the slots stays.
void reset_render_state(VoiceRenderState &state, uint64_t tick);

} // namespace f3rt::hle
