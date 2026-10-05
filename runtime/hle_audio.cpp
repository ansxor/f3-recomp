#include "hle_audio.hpp"
#include "hle_effects.hpp"
#include "hle_sequencer.hpp"
#include "hle_synth.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace f3rt::hle {
namespace {
constexpr size_t history = 64, per_frame = 256, queue_size = 4096, pcm_capacity = 32768;
struct Packet {
    std::array<uint8_t, 8> bytes{};
    uint16_t program = 0;
    uint64_t tick = 0, instance = 0, frame = 0;
    bool matched = false;
    std::span<const uint8_t> data() const { return {bytes.data(), bytes[0]}; }
    bool sfx() const { return bytes[1] == 0x8e; }
    bool same(const Packet &other) const { return bytes == other.bytes && program == other.program; }
};
struct Frame {
    uint64_t tag = UINT64_MAX;
    size_t count = 0;
    std::array<Packet, per_frame> packets{};
};
struct Message {
    enum Kind { Command, Advance, Reset, Cancel, Gain } kind = Advance;
    Packet packet;
    uint64_t tick = 0;
    uint8_t value = 0, channel = 0;
};
}
struct AudioEngine::Impl : VoiceSink {
    uint8_t *shared;
    uint64_t clock = 0, command_count = 0, command_hash = 0, serial = 0;
    // Playback is monotonic even when a slot or host snapshot changes emulated time.
    uint64_t clock_epoch = 0, playback_epoch = 0;
    uint16_t consumer = 0;
    bool held = true, replaying = false;
    uint64_t rollback_begin = 0, rollback_end = 0;
    uint64_t frame_override = UINT64_MAX;
    std::array<uint16_t, 100 * 8> programs{};
    std::array<Frame, history> journal{};
    std::array<Packet, history * per_frame> previous{};
    size_t previous_count = 0;
    std::array<Packet, per_frame> pending{};
    size_t pending_count = 0;
    uint64_t reused = 0, cancelled = 0;

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
        if (replaying || tick <= submitted_tick) return;
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
            output[ch] = gain_model == Audio::GainModel::MameRouting ? route : gain[ch];
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
                case Message::Cancel:
                    synth.cancel(m.packet.instance, worker_tick, 240);
                    sequencer.forget(m.packet.instance);
                    break;
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
    Frame &frame(uint64_t n) {
        auto &f = journal[n % history];
        if (f.tag != n) { f.tag = n; f.count = 0; }
        return f;
    }
    void send(const Packet &p) {
        Message m; m.kind = Message::Command; m.packet = p; m.tick = p.tick; push(m);
    }
    void cancel(const Packet &p) {
        Message m; m.kind = Message::Cancel; m.packet = p; m.tick = clock; push(m); ++cancelled;
    }
    void expire(uint64_t frontier) {
        for (size_t i = 0; i < pending_count;) {
            if (pending[i].frame + 2 < frontier) {
                cancel(pending[i]); pending[i] = pending[--pending_count];
            } else ++i;
        }
    }
    void record(Packet p) {
        ++command_count;
        for (auto byte : p.data()) command_hash = (command_hash ^ byte) * 1099511628211ull;
        const auto op = p.bytes[1];
        const unsigned sequence = p.bytes[2] & 0x7f;
        if (sequence < 100) {
            if ((op == 0x80 || op == 0x81) && p.bytes[0] == 3)
                std::fill_n(programs.begin() + sequence * 8, 8, 0);
            if (p.bytes[0] >= 5 && p.bytes[3] >= 1 && p.bytes[3] <= 8) {
                auto &program = programs[sequence * 8 + p.bytes[3] - 1];
                if (op == 0x8d && p.bytes[0] == 6)
                    program = uint16_t(uint16_t(p.bytes[4]) << 8 | p.bytes[5]);
                if (p.sfx()) p.program = program;
            }
        }
        p.instance = ++serial;
        if (serial >> 63) throw std::overflow_error("HLE instance IDs exhausted");
        auto &f = frame(p.frame);
        if (f.count == per_frame) throw std::runtime_error("HLE per-frame command capacity exceeded");
        if (!replaying) {
            bool matched = false;
            if (p.sfx()) for (size_t i = 0; i < pending_count; ++i) {
                auto &old = pending[i];
                if (old.same(p) && p.frame <= old.frame + 2 && old.frame <= p.frame + 2) {
                    p.instance = old.instance; pending[i] = pending[--pending_count];
                    ++reused; matched = true; break;
                }
            }
            if (!matched) send(p);
        }
        f.packets[f.count++] = p;
    }
    void shared_write(uint32_t offset, uint64_t frame_number) {
        if (held) return;
        if (frame_override != UINT64_MAX) frame_number = frame_override;
        if (offset == 0x481) {
            const uint16_t producer = uint16_t((uint16_t(shared[0x480]) << 8 | shared[0x481]) & 0x7fe);
            unsigned consumed = 0;
            while (consumer != producer && consumed < 1024) {
                const unsigned index = consumer >> 1;
                const uint8_t length = shared[index];
                const unsigned available = ((producer - consumer) & 0x7ff) >> 1;
                if (length < 2 || length > available) break;
                Packet p; p.tick = clock; p.frame = frame_number;
                if (length <= p.bytes.size()) {
                    for (unsigned n = 0; n < length; ++n) p.bytes[n] = shared[(index + n) & 0x3ff];
                    record(p);
                }
                consumer = uint16_t((consumer + length * 2) & 0x7ff); consumed += length;
            }
            shared[0x482] = uint8_t(consumer >> 8); shared[0x483] = uint8_t(consumer);
        } else if (offset == 0x7fa || offset == 0x7fb) {
            if (!replaying) {
                Message m; m.kind = Message::Gain; m.tick = clock;
                m.value = shared[offset]; m.channel = uint8_t(offset & 1); push(m);
            }
        }
    }
    void begin(uint64_t first, uint64_t last) {
        if (replaying || first > last || last - first >= history)
            throw std::invalid_argument("HLE rollback outside retained command history");
        flush(); previous_count = 0;
        for (uint64_t n = first; n < last; ++n) {
            auto &f = frame(n);
            for (size_t i = 0; i < f.count; ++i) {
                previous[previous_count] = f.packets[i]; previous[previous_count++].matched = false;
            }
            f.count = 0;
        }
        rollback_begin = first; rollback_end = last; replaying = true;
    }
    void end() {
        if (!replaying) return;
        replaying = false;
        for (uint64_t n = rollback_begin; n < rollback_end; ++n) {
            auto &f = frame(n);
            for (size_t i = 0; i < f.count; ++i) {
                auto &p = f.packets[i]; Packet *match = nullptr;
                uint64_t distance = 3;
                for (size_t j = 0; j < previous_count; ++j) {
                    auto &old = previous[j];
                    const uint64_t delta = p.frame > old.frame ? p.frame - old.frame : old.frame - p.frame;
                    if (!old.matched && delta < distance && old.same(p)) { match = &old; distance = delta; }
                }
                bool retained = match != nullptr;
                if (match) { match->matched = true; p.instance = match->instance; ++reused; }
                if (!retained && p.sfx()) for (size_t j = 0; j < pending_count; ++j) {
                    const auto &old = pending[j];
                    if (old.same(p) && p.frame <= old.frame + 2 && old.frame <= p.frame + 2) {
                        p.instance = old.instance; pending[j] = pending[--pending_count];
                        ++reused; retained = true; break;
                    }
                }
                // Setters are idempotent and re-establish program/controller
                // context before genuinely new direct notes. Starts/releases
                // are not replayed when matched; music is never rewound.
                const uint8_t op = p.bytes[1];
                const bool setter = (op >= 0x86 && op <= 0x88) || (op >= 0x8a && op <= 0x8d) || op == 0x21;
                if (!retained || setter) send(p);
            }
        }
        for (size_t i = 0; i < previous_count; ++i) {
            const auto &old = previous[i];
            if (old.matched || !old.sfx()) continue;
            if (old.frame + 2 < rollback_end) cancel(old);
            else {
                if (pending_count == pending.size()) throw std::runtime_error("HLE cancellation capacity exceeded");
                pending[pending_count++] = old;
            }
        }
        shared_write(0x7fa, rollback_end);
        shared_write(0x7fb, rollback_end);
        submit(clock);
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
        if (!p.replaying) { Message m; m.kind = Message::Reset; m.tick = p.clock; p.push(m); }
    } else {
        p.shared_write(0x7fa, 0);
        p.shared_write(0x7fb, 0);
    }
}
void AudioEngine::shared_write(uint32_t offset, uint64_t frame) { impl_->shared_write(offset, frame); }
void AudioEngine::begin_frame(uint64_t frame) { impl_->frame_override = frame; }
void AudioEngine::finish_frame(uint64_t frame) {
    impl_->frame_override = UINT64_MAX;
    if (!impl_->replaying) { impl_->expire(frame); impl_->submit(impl_->clock); }
}
void AudioEngine::begin_rollback(uint64_t begin, uint64_t end) { impl_->begin(begin, end); }
void AudioEngine::end_rollback() { impl_->end(); }
void AudioEngine::flush() const { impl_->flush(); }
size_t AudioEngine::available() const {
    flush(); std::lock_guard lock(impl_->pcm_mutex); return size_t(impl_->pcm_write - impl_->pcm_read);
}
template<class Sample> static size_t drain(auto &p, Sample *out, size_t count) {
    p.flush(); std::lock_guard lock(p.pcm_mutex);
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
uint64_t AudioEngine::generated() const { return impl_->generated.load(std::memory_order_relaxed); }
Audio::HleStats AudioEngine::stats() const {
    return {impl_->command_count, impl_->reused, impl_->cancelled, generated()};
}
void AudioEngine::set_observer(std::function<void(const VoiceEvent &)> observer) {
    flush(); impl_->synth.set_observer(std::move(observer));
}
void AudioEngine::set_gain_model(Audio::GainModel model) { flush(); impl_->gain_model = model; }
void AudioEngine::save(std::span<uint8_t> dst) const {
    if (dst.size() != state_bytes) throw std::invalid_argument("HLE state size mismatch");
    std::fill(dst.begin(), dst.end(), 0);
    const uint64_t words[] = {impl_->clock, impl_->command_count, impl_->command_hash};
    for (size_t w = 0; w < 3; ++w) for (unsigned b = 0; b < 8; ++b) dst[w * 8 + b] = uint8_t(words[w] >> (56 - b * 8));
    dst[24] = uint8_t(impl_->consumer >> 8); dst[25] = uint8_t(impl_->consumer); dst[26] = impl_->held;
    for (size_t i = 0; i < impl_->programs.size(); ++i) {
        dst[32 + i * 2] = uint8_t(impl_->programs[i] >> 8);
        dst[33 + i * 2] = uint8_t(impl_->programs[i]);
    }
}
void AudioEngine::load(std::span<const uint8_t> src) {
    if (src.size() != state_bytes) throw std::invalid_argument("HLE state size mismatch");
    const uint16_t consumer = uint16_t(uint16_t(src[24]) << 8 | src[25]);
    if ((consumer & ~0x7feu) || src[26] > 1 ||
        std::any_of(src.begin() + 27, src.begin() + 32, [](uint8_t byte) { return byte != 0; }))
        throw std::invalid_argument("Invalid HLE mailbox state");
    uint64_t words[3]{};
    for (size_t w = 0; w < 3; ++w) for (unsigned b = 0; b < 8; ++b) words[w] = words[w] << 8 | src[w * 8 + b];
    if (!impl_->replaying) {
        impl_->flush();
        impl_->clock_epoch = words[0];
        impl_->playback_epoch = impl_->worker_tick;
        impl_->submitted_tick = words[0];
        for (auto &frame : impl_->journal) { frame.tag = UINT64_MAX; frame.count = 0; }
        impl_->previous_count = impl_->pending_count = 0;
        impl_->frame_override = UINT64_MAX;
        std::lock_guard lock(impl_->pcm_mutex);
        impl_->pcm_read = impl_->pcm_write;
    }
    impl_->clock = words[0]; impl_->command_count = words[1]; impl_->command_hash = words[2];
    impl_->consumer = consumer; impl_->held = src[26] != 0;
    for (size_t i = 0; i < impl_->programs.size(); ++i)
        impl_->programs[i] = uint16_t(uint16_t(src[32 + i * 2]) << 8 | src[33 + i * 2]);
}
} // namespace f3rt::hle
