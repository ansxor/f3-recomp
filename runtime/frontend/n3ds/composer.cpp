#include "composer.hpp"
#include "cpu_counters.hpp"
#include "log.hpp"
#include "renderer/game/captured_frame.hpp"
#include "renderer/game/indexed_compositor.hpp"
#include "renderer/game/row_workers.hpp"
#include <3ds.h>
#include <chrono>

namespace f3rt::n3ds {
namespace {
// Staging rows the display transfer reads: the picture height, not the whole texture.
constexpr unsigned STAGING_STRIDE = 512;
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
    while (true) {
        std::unique_lock lock(m_shared.mutex);
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
        const auto begin = std::chrono::steady_clock::now();
        f3rt::compose_captured_indexed(*shot->captured, m_session.playfield_tiles(),
                                       m_session.sprite_tiles(), scratch,
                                       {staging, size_t(STAGING_STRIDE) * 256}, stride,
                                       f3rt::PixelFormat::Rgba8888, options, counters);
        const auto end = std::chrono::steady_clock::now();
        // The writer owns cache maintenance: the display transfer reads these rows by DMA.
        GSPGPU_FlushDataCache(staging, u32(size_t(stride) * height * sizeof(uint32_t)));
        const auto to_us = [](auto span) {
            return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(span).count());
        };
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
