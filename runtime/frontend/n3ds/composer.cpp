#include "composer.hpp"
#include "cpu_counters.hpp"
#include "f3rt/machine.hpp"
#include "log.hpp"
#include "renderer/game/captured_frame.hpp"
#include "renderer/game/indexed_compositor.hpp"
#include "renderer/game/row_workers.hpp"
#include <3ds.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <string>

namespace f3rt::n3ds {
namespace {
// Staging rows the display transfer reads: the picture height, not the whole texture.
constexpr unsigned STAGING_STRIDE = 512;

using Clock = std::chrono::steady_clock;
constexpr auto FRAME_PERIOD = std::chrono::nanoseconds(
    uint64_t(1e9 * f3rt::Machine::FRAME_PIXELS / f3rt::Machine::PIXEL_CLOCK));
// What the core-0 helper leaves to synthesis on top of its estimated block time: the synth must
// finish a frame's block before the emulation thread polls it one frame later, and the presenter
// shares the core.
constexpr auto SYNTH_MARGIN = std::chrono::milliseconds(2);
// Added to the emulation time estimate before the core-2 helper may start: emulation frames vary.
constexpr auto EMULATION_MARGIN = std::chrono::microseconds(500);
// Composes slower than this are written to the log with how the participants spent them.
constexpr uint32_t SLOW_FRAME_US = 14000;

uint64_t to_us(Clock::duration span) {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(span).count());
}

// Forwards the caller's share of a dispatch to the performance counters (when observing) and
// stamps its start: everything before it is serial preparation.
class ShareTimer final : public f3rt::WorkWindow {
public:
    explicit ShareTimer(f3rt::WorkWindow *inner) : m_inner(inner) {}
    void begin() override {
        m_begin = Clock::now();
        if (m_inner) m_inner->begin();
    }
    void end() override {
        if (m_inner) m_inner->end();
    }
    Clock::time_point began() const { return m_begin; }

private:
    f3rt::WorkWindow *m_inner;
    Clock::time_point m_begin{};
};

// How one composed frame went, kept for the log.
struct FrameRecord {
    uint64_t frame = 0;
    uint32_t compose_us = 0, prep_us = 0;
    uint32_t emu_us = 0, synth_us = 0; // the emulated and synthesized frames running alongside
    f3rt::DispatchTrace trace;
};

// Frames over SLOW_FRAME_US wait here until the composer has nothing to compose: writing the log
// takes milliseconds that would otherwise delay the next frame. A full queue drops the newest
// record and counts it.
class SlowFrameLog {
public:
    void note(const FrameRecord &record) {
        if (record.compose_us <= SLOW_FRAME_US) return;
        if (m_count == m_records.size()) {
            ++m_dropped;
            return;
        }
        m_records[m_count++] = record;
    }
    void flush() {
        for (size_t i = 0; i < m_count; ++i) write(m_records[i]);
        if (m_count && m_dropped) log_line("n3ds: slow frames dropped=%u", m_dropped);
        m_count = 0;
        m_dropped = 0;
    }

private:
    static void write(const FrameRecord &r) {
        // Per participant: chunks pulled @ first claim - end of last chunk (ms from the dispatch
        // start), and in parentheses the part of that spent waiting for a helper window.
        std::string split;
        for (size_t i = 0; i < r.trace.participants.size(); ++i) {
            const f3rt::ParticipantTrace &p = r.trace.participants[i];
            if (!p.joined) continue;
            char name[16], part[64];
            if (i) std::snprintf(name, sizeof(name), "h%zu", i - 1);
            else std::snprintf(name, sizeof(name), "caller");
            std::snprintf(part, sizeof(part), " %s=%u@%.1f-%.1f(w%.1f)", name, p.chunks,
                          p.start_us / 1000.0, p.end_us / 1000.0, p.waited_us / 1000.0);
            split += part;
        }
        log_line("n3ds: slow frame=%llu compose_ms=%.2f prep_ms=%.2f wait_ms=%.2f emu_ms=%.2f "
                 "synth_ms=%.2f%s",
                 static_cast<unsigned long long>(r.frame), r.compose_us / 1000.0,
                 r.prep_us / 1000.0,
                 (r.trace.total_us - r.trace.participants[0].end_us) / 1000.0, r.emu_us / 1000.0,
                 r.synth_us / 1000.0, split.c_str());
    }

    std::array<FrameRecord, 8> m_records;
    size_t m_count = 0;
    unsigned m_dropped = 0;
};
} // namespace

Composer::Composer(Shared &shared, Session &session, const Settings &settings, PerfStats &perf)
    : m_shared(shared), m_session(session), m_settings(settings), m_perf(perf) {}
Composer::~Composer() { stop(); }
void Composer::start() {
    m_thread = f3rt::Thread(f3rt::ThreadRole::Composer, 0, [this] { run(); });
}
void Composer::stop() {
    m_stop.store(true);
    m_shared.cv.notify_all();
    m_thread.join();
}

void Composer::run() {
    // Presentation stays pixel-exact at scale 1; only the side border may widen the frame.
    const f3rt::GameVideoOptions options{1, m_settings.border};
    const unsigned height = options.height();
    f3rt::IndexedScratch scratch;
    scratch.configure(options);
    SlowFrameLog slow_frames;
    // Synthesis time of a frame, averaged: what the core-0 helper has to leave the synth.
    Clock::duration synth_estimate{};
    while (true) {
        std::unique_lock lock(m_shared.mutex);
        // Slow-frame records are written only while no frame waits to be composed.
        if (!m_shared.emulation.unseen()) {
            lock.unlock();
            slow_frames.flush();
            lock.lock();
        }
        m_shared.cv.wait(lock, [&] { return m_stop.load() || m_shared.emulation.unseen(); });
        if (m_stop.load()) return;
        bool fresh = false;
        const f3rt::FrameSnapshot *shot = m_shared.emulation.take(fresh);
        if (!shot || !shot->captured) continue;
        // The emulation slot stays held (immutable) until the next take, so composing outside the
        // lock reads a frame nobody will rewrite.
        f3rt::FrameSnapshot &out = m_shared.present.begin();
        out.frame = shot->frame;
        out.epoch = shot->epoch;
        out.scale = options.scale;
        out.start = shot->start;
        uint32_t *staging = out.rgba;
        const size_t stride = out.stride;
        lock.unlock();
        if (!staging) continue;
        CpuCounters *counters = nullptr;
        if (m_perf.cpu_counters.load(std::memory_order_relaxed)) {
            if (!m_counters_open) m_counters_open = m_counters.open("composer");
            if (m_counters.available()) counters = &m_counters;
        }
        const auto begin = Clock::now();
        // The helpers outrank what they share a core with, so each is confined to the part of
        // every frame period the other thread leaves free. Emulation runs from each frame tick
        // (shot->start, then every period) for its frame time; HLE synthesis then runs the frame's
        // block, and has to finish it within the period. The core-2 helper may therefore compose
        // from the end of emulation to the next tick, the core-0 helper from the end of emulation
        // for the period less the synth time.
        const auto synth_us = m_perf.synth_frame_us.load(std::memory_order_relaxed);
        synth_estimate += (std::chrono::microseconds(synth_us) - synth_estimate) / 8;
        const auto emulation_busy = std::chrono::microseconds(std::max(
                                        m_perf.emu_us.load(std::memory_order_relaxed),
                                        m_perf.emu_frame_us.load(std::memory_order_relaxed))) +
                                    EMULATION_MARGIN;
        const auto idle_from = shot->start + emulation_busy;
        f3rt::ComposeSchedule schedule;
        schedule.emulation_core = {idle_from, FRAME_PERIOD,
                                   std::max(FRAME_PERIOD - emulation_busy, Clock::duration::zero())};
        schedule.synth_core = {idle_from, FRAME_PERIOD,
                               std::max(FRAME_PERIOD - synth_estimate - SYNTH_MARGIN,
                                        Clock::duration::zero())};
        f3rt::compose_workers().set_schedule(schedule);
        ShareTimer share(counters);
        f3rt::compose_captured_indexed(*shot->captured, m_session.playfield_tiles(),
                                       m_session.sprite_tiles(), scratch,
                                       {staging, size_t(STAGING_STRIDE) * 256}, stride,
                                       f3rt::PixelFormat::Rgba8888, options, &share);
        const auto end = Clock::now();
        // The writer owns cache maintenance: the display transfer reads these rows by DMA.
        GSPGPU_FlushDataCache(staging, u32(size_t(stride) * height * sizeof(uint32_t)));
        const auto ema = [](std::atomic<uint64_t> &slot, uint64_t sample) {
            const uint64_t previous = slot.load(std::memory_order_relaxed);
            const int64_t value =
                previous ? int64_t(previous) + (int64_t(sample) - int64_t(previous)) / 16
                         : int64_t(sample);
            slot.store(uint64_t(value), std::memory_order_relaxed);
        };
        const auto maximum = [](std::atomic<uint64_t> &slot, uint64_t sample) {
            uint64_t previous = slot.load(std::memory_order_relaxed);
            while (sample > previous &&
                   !slot.compare_exchange_weak(previous, sample, std::memory_order_relaxed)) {
            }
        };
        const uint64_t compose_us = to_us(end - begin);
        ema(m_perf.compose_us, compose_us);
        maximum(m_perf.compose_max_us, compose_us);
        m_perf.fallback_rows.store(scratch.stats.fallback_rows.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
        m_perf.special_px.store(scratch.stats.special_px.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
        const f3rt::ChunkCounts chunks = f3rt::compose_workers().chunk_counts();
        for (size_t i = 0; i < chunks.size(); ++i)
            m_perf.chunks[i].store(chunks[i], std::memory_order_relaxed);
        slow_frames.note({shot->frame, uint32_t(compose_us), uint32_t(to_us(share.began() - begin)),
                          uint32_t(m_perf.emu_frame_us.load(std::memory_order_relaxed)),
                          uint32_t(synth_us), f3rt::compose_workers().trace()});
        if (counters) m_perf.composer_cpu.store(counters->sample());
        m_perf.composed.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard publish_lock(m_shared.mutex);
            m_shared.present.publish();
        }
        m_shared.cv.notify_all();
    }
}
} // namespace f3rt::n3ds
