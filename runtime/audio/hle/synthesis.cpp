#include "audio/hle/synthesis.hpp"
#include "audio/hle/voice_render.hpp"

namespace f3rt::hle {
namespace {

VoiceFilterState &filter_memory(VoiceRenderState &state, const Voice &voice) noexcept {
    const size_t slot = voice.slot;
    if (state.filter_epoch[slot] != voice.filter_epoch) {
        state.filters[slot].clear();
        state.filter_epoch[slot] = voice.filter_epoch;
    }
    return state.filters[slot];
}

} // namespace

void reset_render_state(VoiceRenderState &state, uint64_t tick) {
    state.tick = tick;
    state.tick_remainder = 0;
}

void render_voice(VoiceService &voices, Voice &voice, SoundBank &bank, VoiceRenderState &state,
                  MutableAudioBlockSpan<8> buses, size_t frame_start, size_t frame_end,
                  std::span<const uint64_t> frame_ticks) {
    VoiceFilterState &memory = filter_memory(state, voice);
    voices.set_render_voice(unsigned(voice.slot));
    const VoiceBus bus(buses, voice.pair, state.discard);
    const SampleWords samples(bank.samples);
    memory = with_filter_mode(voice.filter.mode, [&](auto mode) {
        return VoiceSpan<decltype(mode)::value>(voice, samples, memory)
            .render(bus, voices, frame_ticks, frame_start, frame_end);
    });
}

void render_voices(VoiceService &voices, SoundBank &bank, VoiceRenderState &state,
                   MutableAudioBlockSpan<8> buses, size_t frame_start, size_t frame_end,
                   std::span<const uint64_t> frame_ticks) {
    auto &pool = voices.pool();
    for (Voice &voice : pool) {
        if (!voice.active) continue;
        render_voice(voices, voice, bank, state, buses, frame_start, frame_end, frame_ticks);
    }
}

} // namespace f3rt::hle
