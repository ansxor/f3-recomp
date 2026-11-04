#include "audio_ndsp.hpp"
#include "log.hpp"
#include <3ds.h>
#include <algorithm>
#include <array>
#include <cstring>

namespace f3rt::n3ds {
namespace {
constexpr unsigned CHANNEL = 0;
constexpr double DEFAULT_SOURCE_RATE = 48000;
struct State {
    std::array<ndspWaveBuf, NdspAudioOutput::BUFFER_COUNT> buffers{};
};
State *state(void *memory) { return static_cast<State *>(memory); }
} // namespace
NdspAudioOutput::NdspAudioOutput() {
    // The ndsp wave buffers live in the linear heap: the DSP cannot read the application heap.
    m_memory = linearAlloc(sizeof(State));
    m_data = linearAlloc(size_t(BUFFER_COUNT) * BUFFER_FRAMES * 4);
    if (!m_memory || !m_data) {
        log_line("n3ds: ndsp buffers unavailable");
        if (m_memory) linearFree(m_memory);
        if (m_data) linearFree(m_data);
        m_memory = m_data = nullptr;
        return;
    }
    std::memset(m_data, 0, size_t(BUFFER_COUNT) * BUFFER_FRAMES * 4);
    m_source_rate = DEFAULT_SOURCE_RATE;
    auto *buffers = state(m_memory);
    *buffers = State{};
    ndspChnReset(CHANNEL);
    ndspChnSetFormat(CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetRate(CHANNEL, float(m_source_rate));
    ndspChnSetInterp(CHANNEL, NDSP_INTERP_POLYPHASE);
    m_valid = true;
    set_volume(1);
}
NdspAudioOutput::~NdspAudioOutput() {
    if (m_valid) {
        ndspChnWaveBufClear(CHANNEL);
        m_valid = false;
    }
    if (m_data) linearFree(m_data);
    if (m_memory) linearFree(m_memory);
    m_data = m_memory = nullptr;
}
size_t NdspAudioOutput::queued_frames() const {
    if (!m_valid) return 0;
    const auto *buffers = state(m_memory);
    size_t queued = 0;
    for (unsigned i = 0; i < BUFFER_COUNT; ++i) {
        const auto &buffer = buffers->buffers[i];
        if (buffer.status == NDSP_WBUF_QUEUED) queued += buffer.nsamples;
        else if (buffer.status == NDSP_WBUF_PLAYING) {
            const unsigned position = ndspChnGetSamplePos(CHANNEL);
            queued += position < buffer.nsamples ? buffer.nsamples - position : 0;
        }
    }
    return queued;
}
void NdspAudioOutput::push(const int16_t *stereo, size_t frames) {
    if (!m_valid || !stereo || !frames) return;
    auto *buffers = state(m_memory);
    auto *samples = static_cast<int16_t *>(m_data);
    while (frames) {
        ndspWaveBuf *buffer = nullptr;
        for (unsigned attempt = 0; attempt < BUFFER_COUNT; ++attempt) {
            auto &candidate = buffers->buffers[(m_next + attempt) % BUFFER_COUNT];
            if (candidate.status == NDSP_WBUF_FREE || candidate.status == NDSP_WBUF_DONE) {
                buffer = &candidate;
                m_next = (m_next + attempt + 1) % BUFFER_COUNT;
                break;
            }
        }
        if (!buffer) return;
        const size_t take = std::min(frames, size_t(BUFFER_FRAMES));
        auto *destination = samples + size_t(&*buffer - &buffers->buffers[0]) * BUFFER_FRAMES * 2;
        std::memcpy(destination, stereo, take * 4);
        DSP_FlushDataCache(destination, u32(take * 4));
        buffer->data_pcm16 = destination;
        buffer->nsamples = u32(take);
        ndspChnWaveBufAdd(CHANNEL, buffer);
        stereo += take * 2;
        frames -= take;
    }
}
void NdspAudioOutput::clear() {
    if (!m_valid) return;
    ndspChnWaveBufClear(CHANNEL);
    auto *buffers = state(m_memory);
    for (auto &buffer : buffers->buffers) buffer.status = NDSP_WBUF_FREE;
}
void NdspAudioOutput::set_source_rate(double rate) {
    if (!m_valid || rate <= 0) return;
    m_source_rate = rate;
    ndspChnSetRate(CHANNEL, float(m_source_rate * m_ratio));
}
void NdspAudioOutput::set_rate(double ratio) {
    if (!m_valid) return;
    m_ratio = ratio;
    ndspChnSetRate(CHANNEL, float(m_source_rate * ratio));
}
void NdspAudioOutput::set_volume(float volume) {
    if (!m_valid) return;
    float mix[12] = {};
    mix[0] = volume; // front left
    mix[1] = volume; // front right
    ndspChnSetMix(CHANNEL, mix);
}
} // namespace f3rt::n3ds
