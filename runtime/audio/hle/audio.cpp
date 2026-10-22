#include "audio/hle/audio.hpp"
#include "f3rt/memory_map.hpp"
#include "audio/hle/core.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace f3rt::hle {
static_assert(Audio::OUTPUT_BOOST == OUTPUT_BOOST,
              "Core and Audio must share the output stage gain");
namespace {
constexpr size_t QUEUE_SIZE = 4096, PCM_CAPACITY = 32768;
struct Packet {
    std::array<uint8_t, 8> bytes{};
    uint64_t tick = 0, instance = 0;
    std::span<const uint8_t> data() const { return {bytes.data(), bytes[0]}; }
};
struct Message {
    enum Kind { Command, Advance, Reset, Gain, Bank } kind = Advance;
    Packet packet;
    uint64_t tick = 0;
    uint8_t value = 0, channel = 0;
};
} // namespace
struct AudioEngine::Impl : PcmSink {
    uint8_t *shared;
    uint64_t clock = 0, command_count = 0, serial = 0;
    // Playback is monotonic even when a slot load changes emulated time.
    uint64_t clock_epoch = 0, playback_epoch = 0;
    uint16_t consumer = 0;
    bool held = true;

    // Owned by the worker thread; the main thread touches it only after flush() (observer, gain model).
    Core core;
    mutable std::mutex mutex;
    mutable std::condition_variable wake, completed;
    std::array<Message, QUEUE_SIZE> queue{};
    uint64_t pushed = 0, popped = 0, done = 0, submitted_tick = 0;
    bool stopping = false;
    std::exception_ptr failure;
    std::mutex pcm_mutex;
    std::array<float, PCM_CAPACITY * 2> pcm{};
    uint64_t pcm_read = 0, pcm_write = 0;
    std::atomic<uint64_t> generated{0};
    std::thread worker;

    Impl(std::span<const uint8_t> rom, std::span<const uint16_t> samples, uint8_t *ram)
        : shared(ram), core(rom, samples) {
        if (!shared) throw std::invalid_argument("HLE requires shared RAM");
        core.set_pcm_sink(this);
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        completed.notify_all();
        worker.join();
    }
    // Core's PcmSink: runs on the worker, once per rendered block.
    void write(const float *stereo, size_t n) override {
        std::lock_guard lock(pcm_mutex);
        for (size_t i = 0; i < n; ++i) {
            const size_t pos = size_t(pcm_write % PCM_CAPACITY) * 2;
            pcm[pos] = stereo[i * 2];
            pcm[pos + 1] = stereo[i * 2 + 1];
            ++pcm_write;
        }
        if (pcm_write - pcm_read > PCM_CAPACITY) pcm_read = pcm_write - PCM_CAPACITY;
    }
    void push(const Message &m) {
        std::unique_lock lock(mutex);
        completed.wait(lock, [&] { return pushed - popped < QUEUE_SIZE || failure; });
        if (failure) std::rethrow_exception(failure);
        auto &queued = queue[pushed++ % QUEUE_SIZE];
        queued = m;
        queued.tick = m.tick >= clock_epoch
                          ? playback_epoch + (m.tick - clock_epoch)
                          : playback_epoch - std::min(playback_epoch, clock_epoch - m.tick);
        wake.notify_one();
    }
    void submit(uint64_t tick) {
        if (tick <= submitted_tick) return;
        Message m;
        m.kind = Message::Advance;
        m.tick = tick;
        push(m);
        submitted_tick = tick;
    }
    void flush() {
        submit(clock);
        std::unique_lock lock(mutex);
        const uint64_t target = pushed;
        completed.wait(lock, [&] { return done >= target || failure; });
        if (failure) std::rethrow_exception(failure);
    }
    void run() noexcept {
        Core::enable_flush_denormals();
        try {
            for (;;) {
                Message m;
                uint64_t ticket;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [&] { return stopping || popped != pushed; });
                    if (stopping && popped == pushed) return;
                    m = queue[popped++ % QUEUE_SIZE];
                    ticket = popped;
                    completed.notify_all();
                }
                switch (m.kind) {
                case Message::Command:
                    core.command(m.packet.data(), m.packet.instance, m.tick);
                    break;
                case Message::Reset:
                    core.reset(m.tick);
                    break;
                case Message::Bank:
                    core.set_bank(m.value, m.tick);
                    break;
                case Message::Gain:
                    core.set_gain(m.channel, m.value, m.tick);
                    break;
                case Message::Advance:
                    core.advance(m.tick);
                    break;
                }
                generated.store(core.frames(), std::memory_order_relaxed);
                {
                    std::lock_guard lock(mutex);
                    done = ticket;
                }
                completed.notify_all();
            }
        } catch (...) {
            {
                std::lock_guard lock(mutex);
                failure = std::current_exception();
            }
            completed.notify_all();
            wake.notify_all();
        }
    }
    void record(Packet p) {
        ++command_count;
        p.instance = ++serial;
        if (serial >> 63) throw std::overflow_error("HLE instance IDs exhausted");
        Message m;
        m.kind = Message::Command;
        m.packet = p;
        m.tick = p.tick;
        push(m);
    }
    void shared_write(uint32_t offset) {
        if (held) return;
        if (offset == MAILBOX_PRODUCER_L) {
            const uint16_t producer = uint16_t(
                (uint16_t(shared[MAILBOX_PRODUCER_H]) << 8 | shared[MAILBOX_PRODUCER_L]) & 0x7fe);
            unsigned consumed = 0;
            while (consumer != producer && consumed < 1024) {
                const unsigned index = consumer >> 1;
                const uint8_t length = shared[index];
                const unsigned available = ((producer - consumer) & 0x7ff) >> 1;
                if (length < 2 || length > available) break;
                Packet p;
                p.tick = clock;
                if (length <= p.bytes.size()) {
                    for (unsigned n = 0; n < length; ++n)
                        p.bytes[n] = shared[(index + n) & MAILBOX_RING_MASK];
                    record(p);
                }
                consumer = uint16_t((consumer + length * 2) & 0x7ff);
                consumed += length;
            }
            shared[MAILBOX_CONSUMER_H] = uint8_t(consumer >> 8);
            shared[MAILBOX_CONSUMER_L] = uint8_t(consumer);
        } else if (offset == MAILBOX_GAIN_CH0 || offset == MAILBOX_GAIN_CH1) {
            Message m;
            m.kind = Message::Gain;
            m.tick = clock;
            m.value = shared[offset];
            m.channel = uint8_t(offset & 1);
            push(m);
        }
    }
};

AudioEngine::AudioEngine(std::span<const uint8_t> rom, std::span<const uint16_t> samples,
                         uint8_t *shared)
    : m_impl(std::make_unique<Impl>(rom, samples, shared)) {}
AudioEngine::~AudioEngine() = default;
void AudioEngine::advance(uint32_t cycles) { m_impl->clock += cycles; }
uint64_t AudioEngine::clock() const { return m_impl->clock; }
bool AudioEngine::is_reset() const { return m_impl->held; }
void AudioEngine::set_reset(bool asserted) {
    auto &p = *m_impl;
    if (p.held == asserted) return;
    p.held = asserted;
    if (asserted) {
        p.consumer = 0;
        Message m;
        m.kind = Message::Reset;
        m.tick = p.clock;
        p.push(m);
    } else {
        p.shared_write(MAILBOX_GAIN_CH0);
        p.shared_write(MAILBOX_GAIN_CH1);
    }
}
void AudioEngine::set_bank(uint8_t bank) {
    auto &p = *m_impl;
    Message m;
    m.kind = Message::Bank;
    m.tick = p.clock;
    m.value = bank;
    p.push(m);
}
void AudioEngine::shared_write(uint32_t offset) { m_impl->shared_write(offset); }
void AudioEngine::finish_frame() { m_impl->submit(m_impl->clock); }
void AudioEngine::flush() const { m_impl->flush(); }
size_t AudioEngine::available() const {
    flush();
    std::lock_guard lock(m_impl->pcm_mutex);
    return size_t(m_impl->pcm_write - m_impl->pcm_read);
}
namespace {
template <bool Wait = true, class Sample> size_t drain(auto &p, Sample *out, size_t count) {
    if constexpr (Wait) p.flush();
    std::lock_guard lock(p.pcm_mutex);
    count = std::min<uint64_t>(count, p.pcm_write - p.pcm_read);
    for (size_t n = 0; n < count; ++n) {
        for (unsigned c = 0; c < 2; ++c) {
            const float value = p.pcm[(p.pcm_read % PCM_CAPACITY) * 2 + c];
            if constexpr (std::is_same_v<Sample, float>) out[n * 2 + c] = value;
            else out[n * 2 + c] = int16_t(std::clamp(value * 32768.f, -32768.f, 32767.f));
        }
        ++p.pcm_read;
    }
    return count;
}
} // namespace
size_t AudioEngine::render(float *out, size_t count) { return drain(*m_impl, out, count); }
size_t AudioEngine::render(int16_t *out, size_t count) { return drain(*m_impl, out, count); }
size_t AudioEngine::render_ready(int16_t *out, size_t count) {
    return drain<false>(*m_impl, out, count);
}
uint64_t AudioEngine::generated() const {
    return m_impl->generated.load(std::memory_order_relaxed);
}
Audio::EnhancedStats AudioEngine::stats() const { return {m_impl->command_count, generated()}; }
#if F3RT_HLE_EVENTS
void AudioEngine::set_observer(std::function<void(const VoiceEvent &)> observer) {
    flush();
    m_impl->core.set_voice_observer(std::move(observer));
}
#endif
void AudioEngine::save(std::span<uint8_t> dst) const {
    if (dst.size() != STATE_BYTES) throw std::invalid_argument("HLE state size mismatch");
    std::fill(dst.begin(), dst.end(), 0);
    const uint64_t words[] = {m_impl->clock, m_impl->command_count};
    for (size_t w = 0; w < 2; ++w)
        for (unsigned b = 0; b < 8; ++b) dst[w * 8 + b] = uint8_t(words[w] >> (56 - b * 8));
    dst[16] = uint8_t(m_impl->consumer >> 8);
    dst[17] = uint8_t(m_impl->consumer);
    dst[18] = m_impl->held;
}
void AudioEngine::load(std::span<const uint8_t> src) {
    if (src.size() != STATE_BYTES) throw std::invalid_argument("HLE state size mismatch");
    const uint16_t consumer = uint16_t(uint16_t(src[16]) << 8 | src[17]);
    if ((consumer & ~0x7feu) || src[18] > 1 ||
        std::any_of(src.begin() + 19, src.end(), [](uint8_t byte) { return byte != 0; }))
        throw std::invalid_argument("Invalid HLE mailbox state");
    uint64_t words[2]{};
    for (size_t w = 0; w < 2; ++w)
        for (unsigned b = 0; b < 8; ++b) words[w] = words[w] << 8 | src[w * 8 + b];
    m_impl->flush();
    m_impl->clock_epoch = words[0];
    m_impl->playback_epoch = m_impl->core.tick();
    m_impl->submitted_tick = words[0];
    {
        std::lock_guard lock(m_impl->pcm_mutex);
        m_impl->pcm_read = m_impl->pcm_write;
    }
    m_impl->clock = words[0];
    m_impl->command_count = words[1];
    m_impl->consumer = consumer;
    m_impl->held = src[18] != 0;
}
bool validate(std::span<const uint8_t> sound, std::span<const uint16_t> samples) noexcept {
    return Core::validate(sound, samples);
}
} // namespace f3rt::hle
