#include "audio/hle/audio.hpp"
#include "audio/hle/effects.hpp"
#include "audio/hle/sequencer.hpp"
#include "audio/hle/synth.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE__)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

namespace f3rt::hle {
namespace {
// Flush-to-zero and denormals-are-zero prevent decaying reverb/filter tails from becoming subnormal.
static void enable_ftz_daz() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(__aarch64__)
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
}
constexpr size_t queue_size = 4096, pcm_capacity = 32768;
struct Packet {
    std::array<uint8_t, 8> bytes{};
    uint64_t tick = 0, instance = 0;
    std::span<const uint8_t> data() const { return {bytes.data(), bytes[0]}; }
};
struct Message {
    enum Kind { Command, Advance, Reset, Gain } kind = Advance;
    Packet packet;
    uint64_t tick = 0;
    uint8_t value = 0, channel = 0;
};
}
struct AudioEngine::Impl : VoiceSink {
    uint8_t *shared;
    uint64_t clock = 0, command_count = 0, serial = 0;
    // Playback is monotonic even when a slot load changes emulated time.
    uint64_t clock_epoch = 0, playback_epoch = 0;
    uint16_t consumer = 0;
    bool held = true;

    Synth synth;
    Effects effects;
    Sequencer sequencer;
    uint64_t worker_tick = 0, worker_frames = 0;
    std::array<float, 2> gain{1.f, 1.f};
    Audio::GainModel gain_model = Audio::GainModel::MameRouting;
    mutable std::mutex mutex;
    mutable std::condition_variable wake, completed;
    std::array<Message, queue_size> queue{};
    uint64_t pushed = 0, popped = 0, done = 0, submitted_tick = 0;
    bool stopping = false;
    std::exception_ptr failure;
    std::mutex pcm_mutex;
    std::array<float, pcm_capacity * 2> pcm{};
    uint64_t pcm_read = 0, pcm_write = 0;
    std::atomic<uint64_t> generated{0};
    std::thread worker;

    Impl(std::span<const uint8_t> rom, std::span<const uint16_t> samples, uint8_t *ram)
        : shared(ram), synth(rom, samples), effects(rom), sequencer(rom, *this) {
        if (!shared) throw std::invalid_argument("HLE requires shared RAM");
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        { std::lock_guard lock(mutex); stopping = true; }
        wake.notify_all(); completed.notify_all();
        worker.join();
    }
    void note_on(const Note &n) override { synth.note_on(n); }
    void note_off(uint64_t id, uint64_t tick) override { synth.note_off(id, tick); }
    void channel_update(uint8_t s, uint8_t t, const Channel &c, uint64_t tick) override {
        synth.channel_update(s, t, c, tick);
    }
    void effect(uint8_t preset, uint64_t) override { effects.select(preset); }
    void effect_parameter(uint8_t index, uint8_t value, uint64_t) override { effects.parameter(index, value); }
    void reset(uint64_t tick) override { synth.reset(tick); effects.reset(); gain.fill(1.f); }
    void push(const Message &m) {
        std::unique_lock lock(mutex);
        completed.wait(lock, [&] { return pushed - popped < queue_size || failure; });
        if (failure) std::rethrow_exception(failure);
        auto &queued = queue[pushed++ % queue_size];
        queued = m;
        queued.tick = m.tick >= clock_epoch ? playback_epoch + (m.tick - clock_epoch) :
            playback_epoch - std::min(playback_epoch, clock_epoch - m.tick);
        wake.notify_one();
    }
    void submit(uint64_t tick) {
        if (tick <= submitted_tick) return;
        Message m; m.kind = Message::Advance; m.tick = tick; push(m); submitted_tick = tick;
    }
    void flush() {
        submit(clock);
        std::unique_lock lock(mutex);
        const uint64_t target = pushed;
        completed.wait(lock, [&] { return done >= target || failure; });
        if (failure) std::rethrow_exception(failure);
    }
    void render_until(uint64_t tick) {
        const uint64_t end = tick / main_clock * sample_rate + tick % main_clock * sample_rate / main_clock;
        std::array<float, 256 * 8> buses;
        std::array<float, 256 * 2> stereo;
        std::array<float, 2> input, output;
        for (unsigned ch = 0; ch < 2; ++ch) {
            const float route = float(int(gain[ch] * 100.f + .5f)) / 32.f;
            input[ch] = .18f * (gain_model == Audio::GainModel::MameRouting ? route : 1.f);
            output[ch] = Audio::output_boost * (gain_model == Audio::GainModel::MameRouting ? route : gain[ch]);
        }
        while (worker_frames < end) {
            const size_t n = size_t(std::min<uint64_t>(256, end - worker_frames));
            synth.render(buses.data(), n);
            effects.mix(buses.data(), stereo.data(), n, input, output);
            {
                std::lock_guard lock(pcm_mutex);
                for (size_t i = 0; i < n; ++i) {
                    pcm[(pcm_write % pcm_capacity) * 2] = stereo[i * 2];
                    pcm[(pcm_write % pcm_capacity) * 2 + 1] = stereo[i * 2 + 1];
                    ++pcm_write;
                }
                if (pcm_write - pcm_read > pcm_capacity) pcm_read = pcm_write - pcm_capacity;
            }
            worker_frames += n;
        }
        generated.store(worker_frames, std::memory_order_relaxed);
    }
    void advance_worker(uint64_t tick) {
        while (worker_tick < tick) {
            const uint64_t next = std::min(tick, sequencer.next_tick());
            render_until(next);
            sequencer.advance_to(next);
            worker_tick = next;
        }
    }
    void run() noexcept {
        enable_ftz_daz();
        try {
            for (;;) {
                Message m;
                uint64_t ticket;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [&] { return stopping || popped != pushed; });
                    if (stopping && popped == pushed) return;
                    m = queue[popped++ % queue_size]; ticket = popped;
                    completed.notify_all();
                }
                advance_worker(std::max(worker_tick, m.tick));
                switch (m.kind) {
                case Message::Command: sequencer.command(m.packet.data(), m.packet.instance, worker_tick); break;
                case Message::Reset: sequencer.reset(worker_tick); break;
                case Message::Gain: gain[m.channel] = std::pow(10.f, -.5f * float(63 - (m.value & 63)) / 20.f); break;
                case Message::Advance: break;
                }
                { std::lock_guard lock(mutex); done = ticket; }
                completed.notify_all();
            }
        } catch (...) {
            { std::lock_guard lock(mutex); failure = std::current_exception(); }
            completed.notify_all(); wake.notify_all();
        }
    }
    void record(Packet p) {
        ++command_count;
        p.instance = ++serial;
        if (serial >> 63) throw std::overflow_error("HLE instance IDs exhausted");
        Message m; m.kind = Message::Command; m.packet = p; m.tick = p.tick; push(m);
    }
    void shared_write(uint32_t offset) {
        if (held) return;
        if (offset == 0x481) {
            const uint16_t producer = uint16_t((uint16_t(shared[0x480]) << 8 | shared[0x481]) & 0x7fe);
            unsigned consumed = 0;
            while (consumer != producer && consumed < 1024) {
                const unsigned index = consumer >> 1;
                const uint8_t length = shared[index];
                const unsigned available = ((producer - consumer) & 0x7ff) >> 1;
                if (length < 2 || length > available) break;
                Packet p; p.tick = clock;
                if (length <= p.bytes.size()) {
                    for (unsigned n = 0; n < length; ++n) p.bytes[n] = shared[(index + n) & 0x3ff];
                    record(p);
                }
                consumer = uint16_t((consumer + length * 2) & 0x7ff); consumed += length;
            }
            shared[0x482] = uint8_t(consumer >> 8); shared[0x483] = uint8_t(consumer);
        } else if (offset == 0x7fa || offset == 0x7fb) {
            Message m; m.kind = Message::Gain; m.tick = clock;
            m.value = shared[offset]; m.channel = uint8_t(offset & 1); push(m);
        }
    }
};

AudioEngine::AudioEngine(std::span<const uint8_t> rom, std::span<const uint16_t> samples, uint8_t *shared)
    : impl_(std::make_unique<Impl>(rom, samples, shared)) {}
AudioEngine::~AudioEngine() = default;
void AudioEngine::advance(uint32_t cycles) { impl_->clock += cycles; }
uint64_t AudioEngine::clock() const { return impl_->clock; }
bool AudioEngine::is_reset() const { return impl_->held; }
void AudioEngine::set_reset(bool asserted) {
    auto &p = *impl_;
    if (p.held == asserted) return;
    p.held = asserted;
    if (asserted) {
        p.consumer = 0;
        Message m; m.kind = Message::Reset; m.tick = p.clock; p.push(m);
    } else {
        p.shared_write(0x7fa);
        p.shared_write(0x7fb);
    }
}
void AudioEngine::shared_write(uint32_t offset) { impl_->shared_write(offset); }
void AudioEngine::finish_frame() { impl_->submit(impl_->clock); }
void AudioEngine::flush() const { impl_->flush(); }
size_t AudioEngine::available() const {
    flush(); std::lock_guard lock(impl_->pcm_mutex); return size_t(impl_->pcm_write - impl_->pcm_read);
}
template<bool Wait = true, class Sample> static size_t drain(auto &p, Sample *out, size_t count) {
    if constexpr (Wait) p.flush();
    std::lock_guard lock(p.pcm_mutex);
    count = std::min<uint64_t>(count, p.pcm_write - p.pcm_read);
    for (size_t n = 0; n < count; ++n) {
        for (unsigned c = 0; c < 2; ++c) {
            const float value = p.pcm[(p.pcm_read % pcm_capacity) * 2 + c];
            if constexpr (std::is_same_v<Sample, float>) out[n * 2 + c] = value;
            else out[n * 2 + c] = int16_t(std::clamp(value * 32768.f, -32768.f, 32767.f));
        }
        ++p.pcm_read;
    }
    return count;
}
size_t AudioEngine::render(float *out, size_t count) { return drain(*impl_, out, count); }
size_t AudioEngine::render(int16_t *out, size_t count) { return drain(*impl_, out, count); }
size_t AudioEngine::render_ready(int16_t *out, size_t count) { return drain<false>(*impl_, out, count); }
uint64_t AudioEngine::generated() const { return impl_->generated.load(std::memory_order_relaxed); }
Audio::EnhancedStats AudioEngine::stats() const {
    return {impl_->command_count, generated()};
}
void AudioEngine::set_observer(std::function<void(const VoiceEvent &)> observer) {
    flush(); impl_->synth.set_observer(std::move(observer));
}
void AudioEngine::set_gain_model(Audio::GainModel model) { flush(); impl_->gain_model = model; }
void AudioEngine::save(std::span<uint8_t> dst) const {
    if (dst.size() != state_bytes) throw std::invalid_argument("HLE state size mismatch");
    std::fill(dst.begin(), dst.end(), 0);
    const uint64_t words[] = {impl_->clock, impl_->command_count};
    for (size_t w = 0; w < 2; ++w) for (unsigned b = 0; b < 8; ++b) dst[w * 8 + b] = uint8_t(words[w] >> (56 - b * 8));
    dst[16] = uint8_t(impl_->consumer >> 8); dst[17] = uint8_t(impl_->consumer); dst[18] = impl_->held;
}
void AudioEngine::load(std::span<const uint8_t> src) {
    if (src.size() != state_bytes) throw std::invalid_argument("HLE state size mismatch");
    const uint16_t consumer = uint16_t(uint16_t(src[16]) << 8 | src[17]);
    if ((consumer & ~0x7feu) || src[18] > 1 ||
        std::any_of(src.begin() + 19, src.end(), [](uint8_t byte) { return byte != 0; }))
        throw std::invalid_argument("Invalid HLE mailbox state");
    uint64_t words[2]{};
    for (size_t w = 0; w < 2; ++w) for (unsigned b = 0; b < 8; ++b) words[w] = words[w] << 8 | src[w * 8 + b];
    impl_->flush();
    impl_->clock_epoch = words[0];
    impl_->playback_epoch = impl_->worker_tick;
    impl_->submitted_tick = words[0];
    {
        std::lock_guard lock(impl_->pcm_mutex);
        impl_->pcm_read = impl_->pcm_write;
    }
    impl_->clock = words[0]; impl_->command_count = words[1];
    impl_->consumer = consumer; impl_->held = src[18] != 0;
}
} // namespace f3rt::hle
