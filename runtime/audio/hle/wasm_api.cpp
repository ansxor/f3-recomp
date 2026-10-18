#include "audio/hle/core.hpp"
#include "audio/hle/replay.hpp"
#include "audio/hle/synth.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <vector>

namespace {

static std::string g_last_error;

void set_error(const std::string &msg) {
    g_last_error = msg;
}

void clear_error() {
    g_last_error.clear();
}

class FifoSink final : public f3rt::hle::PcmSink {
public:
    explicit FifoSink(size_t min_frames = 4096) {
        m_buf.resize(std::max(min_frames, size_t(4096)) * 2);
    }

    size_t size_frames() const noexcept { return m_count / 2; }

    void clear() noexcept {
        m_head = 0;
        m_count = 0;
    }

    void write(const float *stereo, size_t frames) override {
        const size_t samples = frames * 2;
        if (m_count + samples > m_buf.size()) {
            size_t new_cap = std::max(m_buf.size() * 2, m_count + samples);
            if (new_cap % 2 != 0) ++new_cap;
            std::vector<float> new_buf(new_cap);
            for (size_t i = 0; i < m_count; ++i) {
                new_buf[i] = m_buf[(m_head + i) % m_buf.size()];
            }
            m_buf = std::move(new_buf);
            m_head = 0;
        }
        for (size_t i = 0; i < samples; ++i) {
            m_buf[(m_head + m_count + i) % m_buf.size()] = stereo[i];
        }
        m_count += samples;
    }

    void pop_render(float *out_stereo, size_t frames) noexcept {
        for (size_t i = 0; i < frames; ++i) {
            const float l = m_buf[m_head];
            const float r = m_buf[m_head + 1];
            m_head = (m_head + 2) % m_buf.size();
            // Exchange OTIS/ESP (L, R) to DAC perspective (R, L).
            out_stereo[2 * i + 0] = r;
            out_stereo[2 * i + 1] = l;
        }
        m_count -= frames * 2;
        if (m_count == 0) m_head = 0;
    }

    void pop_skip(size_t frames) noexcept {
        m_head = (m_head + frames * 2) % m_buf.size();
        m_count -= frames * 2;
        if (m_count == 0) m_head = 0;
    }

private:
    std::vector<float> m_buf;
    size_t m_head = 0;
    size_t m_count = 0;
};

} // namespace

struct f3hle_engine {
    std::span<const uint8_t> sound_rom;
    std::span<const uint16_t> sample_words;
    std::vector<uint8_t> replay_data;
    FifoSink fifo{4096};
    std::unique_ptr<f3rt::hle::Core> core;
    uint64_t delivered_frames = 0;
    bool faulted = false;

    f3hle_engine(std::span<const uint8_t> sound, std::span<const uint16_t> samples)
        : sound_rom(sound), sample_words(samples) {}
};

extern "C" {

uint32_t f3hle_abi_version(void) {
    return 1;
}

const char *f3hle_last_error(void) {
    return g_last_error.c_str();
}

void *f3hle_alloc(uint32_t bytes) {
    try {
        if (bytes == 0) bytes = 1;
        const size_t total = size_t(bytes) + 16 + sizeof(void *);
        void *raw = std::malloc(total);
        if (!raw) {
            set_error("f3hle_alloc: out of memory");
            return nullptr;
        }
        const uintptr_t addr = reinterpret_cast<uintptr_t>(raw) + sizeof(void *);
        const uintptr_t aligned = (addr + 15) & ~uintptr_t(15);
        reinterpret_cast<void **>(aligned)[-1] = raw;
        clear_error();
        return reinterpret_cast<void *>(aligned);
    } catch (const std::exception &e) {
        set_error(e.what());
        return nullptr;
    } catch (...) {
        set_error("f3hle_alloc: unknown C++ exception");
        return nullptr;
    }
}

void f3hle_free(void *p) {
    if (!p) return;
    void *raw = reinterpret_cast<void **>(p)[-1];
    std::free(raw);
}

f3hle_engine *f3hle_create(const uint8_t *sound, uint32_t sound_bytes,
                           const uint16_t *sample_words, uint32_t sample_word_count) {
    try {
        if (!sound || !sample_words) {
            set_error("f3hle_create: null ROM pointer");
            return nullptr;
        }
        auto sound_span = std::span<const uint8_t>(sound, sound_bytes);
        auto sample_span = std::span<const uint16_t>(sample_words, sample_word_count);
        if (!f3rt::hle::Synth::validate(sound_span, sample_span)) {
            set_error("f3hle_create: ROM validation failed");
            return nullptr;
        }
        auto engine = std::make_unique<f3hle_engine>(sound_span, sample_span);
        clear_error();
        return engine.release();
    } catch (const std::exception &e) {
        set_error(e.what());
        return nullptr;
    } catch (...) {
        set_error("f3hle_create: unknown C++ exception");
        return nullptr;
    }
}

void f3hle_destroy(f3hle_engine *e) {
    delete e;
}

int f3hle_start(f3hle_engine *e, const uint8_t *replay, uint32_t replay_bytes) {
    try {
        if (!e) {
            set_error("f3hle_start: null engine pointer");
            return -1;
        }
        if (!replay || replay_bytes == 0) {
            e->faulted = true;
            set_error("f3hle_start: empty replay script");
            return -1;
        }
        e->replay_data.assign(replay, replay + replay_bytes);
        e->fifo.clear();
        e->delivered_frames = 0;
        e->core.reset();
        e->core = std::make_unique<f3rt::hle::Core>(e->sound_rom, e->sample_words);
        e->core->set_pcm_sink(&e->fifo);
        e->core->set_gain_model(f3rt::hle::GainModel::SingleStage);
        f3rt::hle::replay(*e->core, e->replay_data);
        e->faulted = false;
        clear_error();
        return 0;
    } catch (const std::exception &e_ex) {
        if (e) e->faulted = true;
        set_error(e_ex.what());
        return -1;
    } catch (...) {
        if (e) e->faulted = true;
        set_error("f3hle_start: unknown C++ exception");
        return -1;
    }
}

int f3hle_rewind(f3hle_engine *e) {
    try {
        if (!e) {
            set_error("f3hle_rewind: null engine pointer");
            return -1;
        }
        if (e->faulted) {
            set_error("f3hle_rewind: engine is faulted");
            return -1;
        }
        if (e->replay_data.empty()) {
            set_error("f3hle_rewind: no active replay script");
            return -1;
        }
        e->fifo.clear();
        e->delivered_frames = 0;
        e->core.reset();
        e->core = std::make_unique<f3rt::hle::Core>(e->sound_rom, e->sample_words);
        e->core->set_pcm_sink(&e->fifo);
        e->core->set_gain_model(f3rt::hle::GainModel::SingleStage);
        f3rt::hle::replay(*e->core, e->replay_data);
        clear_error();
        return 0;
    } catch (const std::exception &e_ex) {
        if (e) e->faulted = true;
        set_error(e_ex.what());
        return -1;
    } catch (...) {
        if (e) e->faulted = true;
        set_error("f3hle_rewind: unknown C++ exception");
        return -1;
    }
}

uint32_t f3hle_render(f3hle_engine *e, float *stereo, uint32_t frames) {
    try {
        if (!e) {
            set_error("f3hle_render: null engine pointer");
            return 0xffffffff;
        }
        if (e->faulted) {
            set_error("f3hle_render: engine is faulted");
            return 0xffffffff;
        }
        if (!e->core) {
            set_error("f3hle_render: engine not started");
            return 0xffffffff;
        }
        if (frames == 0) {
            clear_error();
            return 0;
        }
        if (!stereo) {
            set_error("f3hle_render: null destination buffer");
            return 0xffffffff;
        }
        while (e->fifo.size_frames() < frames) {
            e->core->advance(e->core->next_boundary());
        }
        e->fifo.pop_render(stereo, frames);
        e->delivered_frames += frames;
        clear_error();
        return frames;
    } catch (const std::exception &e_ex) {
        if (e) e->faulted = true;
        set_error(e_ex.what());
        return 0xffffffff;
    } catch (...) {
        if (e) e->faulted = true;
        set_error("f3hle_render: unknown C++ exception");
        return 0xffffffff;
    }
}

uint32_t f3hle_skip(f3hle_engine *e, uint32_t frames) {
    try {
        if (!e) {
            set_error("f3hle_skip: null engine pointer");
            return 0xffffffff;
        }
        if (e->faulted) {
            set_error("f3hle_skip: engine is faulted");
            return 0xffffffff;
        }
        if (!e->core) {
            set_error("f3hle_skip: engine not started");
            return 0xffffffff;
        }
        if (frames == 0) {
            clear_error();
            return 0;
        }
        while (e->fifo.size_frames() < frames) {
            e->core->advance(e->core->next_boundary());
        }
        e->fifo.pop_skip(frames);
        e->delivered_frames += frames;
        clear_error();
        return frames;
    } catch (const std::exception &e_ex) {
        if (e) e->faulted = true;
        set_error(e_ex.what());
        return 0xffffffff;
    } catch (...) {
        if (e) e->faulted = true;
        set_error("f3hle_skip: unknown C++ exception");
        return 0xffffffff;
    }
}

double f3hle_position_frames(const f3hle_engine *e) {
    if (!e) return 0.0;
    return static_cast<double>(e->delivered_frames);
}

double f3hle_position_ticks(const f3hle_engine *e) {
    if (!e || !e->core) return 0.0;
    return static_cast<double>(e->core->tick());
}

} // extern "C"
