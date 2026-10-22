#pragma once
#include "audio/hle/block.hpp"
#include "audio/hle/faust/generated/voice_filter.hpp"
#include "audio/hle/sound_bank.hpp"
#include "audio/hle/voice_service.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace f3rt::hle {

// Frames per voice-filter block: each active voice runs one faust::VoiceFilterDsp::compute() per block.
inline constexpr size_t VOICE_BLOCK_FRAMES = 64;

// Render-side state: the per-slot filter instances and the scratch buffers the voices accumulate into. It is not a
// service: it holds no voice decisions, only the DSP memory and the frame -> tick map of the current block.
struct VoiceRenderState {
    std::array<faust::VoiceFilterDsp, 32> filters;
    std::array<float *, 32> mode_zone{};
    std::array<uint32_t, 32> filter_epoch{};
    std::vector<uint64_t> frame_ticks;
    alignas(64) float input[VOICE_BLOCK_FRAMES]{};
    alignas(64) float a1[VOICE_BLOCK_FRAMES]{};
    alignas(64) float a2[VOICE_BLOCK_FRAMES]{};
    alignas(64) float high_pole[VOICE_BLOCK_FRAMES]{};
    alignas(64) float gain_left[VOICE_BLOCK_FRAMES]{};
    alignas(64) float gain_right[VOICE_BLOCK_FRAMES]{};
    PlanarAudioBlock<2, VOICE_BLOCK_FRAMES> filter_out;
    uint64_t tick = 0;
    uint32_t tick_remainder = 0;

    VoiceRenderState() {
        frame_ticks.reserve(1024);
        for (size_t i = 0; i < filters.size(); ++i) {
            filters[i].init(int(SAMPLE_RATE));
            mode_zone[i] = faust::Controls(filters[i]).zone("mode");
        }
    }
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
