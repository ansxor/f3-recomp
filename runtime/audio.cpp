// Standalone Taito F3 audio implementation for f3rt
// Incorporates ES5505 OTIS, ES5510 DSP, MC68681 DUART, MB87078 Volume, and ESQPUMP

#include "f3rt/audio.hpp"

#include "third_party/audio/es5505.hpp"
#include "third_party/audio/es5510.hpp"
#include "third_party/audio/mb87078.hpp"
#include "third_party/audio/mc68681.hpp"
#include "state_io.hpp"
#include "hle_audio.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace f3rt {

namespace {

inline int16_t clamp16(int32_t val) {
    if (val > 32767) return 32767;
    if (val < -32768) return -32768;
    return int16_t(val);
}

} // namespace

struct Audio::Impl {
    static constexpr size_t WORK_RAM_SIZE = 0x10000; // 64KB
    static constexpr size_t RING_BUFFER_CAPACITY = 32768; // Stereo frames

    std::vector<uint8_t> m_work_ram;
    std::vector<uint8_t> m_sound_rom;
    std::vector<uint16_t> m_sample_words;
    std::unique_ptr<hle::AudioEngine> m_hle;
    uint32_t m_bank_mask;

    uint8_t *m_shared_ram;
    size_t m_shared_ram_size;

    uint16_t m_bank_table[ES5505::MAX_VOICES];

    ES5505  m_es5505;
    ES5510  m_es5510;
    MC68681 m_duart;
    MB87078 m_volume;

    Audio::GainModel m_gain_model = Audio::GainModel::MameRouting;
    std::array<float, 2> m_volume_gain{1.0f, 1.0f};
    std::array<float, 2> m_otis_gain{}, m_output_gain{};

    bool m_reset_asserted;
    bool m_esp_halted = true;

    std::function<int(int cycles)> m_cpu_runner;
    std::function<void(bool asserted)> m_reset_cb;

    // Fractional cycle accumulators
    int64_t m_cpu_accum;
    uint32_t m_duart_accum;
    uint64_t m_sample_accum;
    uint64_t m_clock_ticks = 0;
    uint64_t m_generated_frames = 0;

    // Thread-safe audio sample ring buffer (interleaved stereo)
    mutable std::mutex m_audio_mutex;
    std::vector<float> m_sample_buffer;
    size_t m_rb_read_pos;
    size_t m_rb_write_pos;
    size_t m_rb_count;

    Impl()
        : m_work_ram(WORK_RAM_SIZE, 0)
        , m_bank_mask(7)
        , m_shared_ram(nullptr)
        , m_shared_ram_size(0x800)
        , m_reset_asserted(true)
        , m_cpu_accum(0)
        , m_duart_accum(0)
        , m_sample_accum(0)
        , m_sample_buffer(RING_BUFFER_CAPACITY * 2, 0.0f)
        , m_rb_read_pos(0)
        , m_rb_write_pos(0)
        , m_rb_count(0)
    {
        std::memset(m_bank_table, 0, sizeof(m_bank_table));

        // Connect DUART OP6 output to ES5510 HALT pin
        m_duart.set_outport_callback([this](uint8_t output_pins) {
            // Pin OP6: 1 = ESPHALT asserted (halted), 0 = ESP running
            m_esp_halted = (output_pins & 0x40) != 0;
        });

        // The baseline board mixer applies quantized MB87078 percent/32 gain
        // both before ESP input and after the pump. Keep that observable model
        // separate from a single analog attenuation stage.
        update_gains();
        m_volume.set_gain_callback([this](int channel, float gain) {
            if (channel >= 2) {
                m_volume_gain[channel & 1] = gain;
                update_gains();
            }
        });
    }

    void update_gains() {
        for (unsigned channel = 0; channel < 2; ++channel) {
            const float physical = m_volume_gain[channel];
            const float route = float(int(physical * 100.0f + 0.5f)) / 32.0f;
            m_otis_gain[channel] = 0.18f *
                (m_gain_model == Audio::GainModel::MameRouting ? route : 1.0f);
            m_output_gain[channel] =
                m_gain_model == Audio::GainModel::MameRouting ? route : physical;
        }
    }


    void set_reset(bool asserted) {
        if (m_reset_asserted != asserted) {
            m_reset_asserted = asserted;
            if (m_reset_cb) {
                m_reset_cb(asserted);
            }
        }
    }

    void reset_board() {
        set_reset(true);
        m_cpu_accum = 0;
        m_duart.reset();
        m_es5510.reset();
        m_volume.reset();
        if (m_sound_rom.size() >= 8)
            std::memcpy(m_work_ram.data(), m_sound_rom.data(), 8);
    }

    void push_frame(float left, float right) {
        std::lock_guard<std::mutex> lock(m_audio_mutex);
        if (m_rb_count >= RING_BUFFER_CAPACITY) {
            // Overwrite oldest frame to preserve real-time latency
            m_rb_read_pos = (m_rb_read_pos + 1) % RING_BUFFER_CAPACITY;
            --m_rb_count;
        }
        m_sample_buffer[m_rb_write_pos * 2 + 0] = left;
        m_sample_buffer[m_rb_write_pos * 2 + 1] = right;
        m_rb_write_pos = (m_rb_write_pos + 1) % RING_BUFFER_CAPACITY;
        ++m_rb_count;
    }

    void generate_one_frame() {
        ++m_generated_frames;
        int32_t cursample[ES5505::NUM_CHANNELS];
        m_es5505.generate_one_sample(cursample);

        // ES5505 has a signed 20-bit accumulator. Clamp at its output before
        // applying board gain, then quantize only the six serial DSP inputs.
        float channels[ES5505::NUM_CHANNELS];
        for (unsigned i = 0; i < ES5505::NUM_CHANNELS; ++i)
            channels[i] = std::clamp(float(cursample[i]) / 524288.0f, -1.0f, 1.0f) * m_otis_gain[i & 1];
        for (unsigned i = 0; i < 6; ++i)
            m_es5510.ser_w(i, int16_t(int32_t(channels[i + 2] * 32768.0f)));

        if (!m_esp_halted) {
            m_es5510.run_once();
        }

        // ES5510 processed outputs (Main L / Main R)
        int16_t main_l = m_es5510.ser_r(6);
        int16_t main_r = m_es5510.ser_r(7);

        // Aux bypasses ESP and retains floating precision, as in the baseline
        // mixer; the main route comes from the DSP's signed 16-bit output.
        const float out_l = (float(main_l) / 32768.0f + channels[0]) * 0.5f * m_output_gain[0];
        const float out_r = (float(main_r) / 32768.0f + channels[1]) * 0.5f * m_output_gain[1];

        push_frame(out_l, out_r);
    }

    void advance_slice(uint32_t main_cycles) {
        m_clock_ticks += main_cycles;
        // Advance DUART 68681 (clock 4.0 MHz = 16MHz / 4).
        m_duart_accum += main_cycles;
        uint32_t duart_cycles = m_duart_accum / 4;
        m_duart_accum %= 4;

        if (duart_cycles > 0) {
            m_duart.advance(duart_cycles);
        }

        // Use the same integer stream rate advertised to SDL/WAV consumers.
        // A rational crystal divider here drifts against both that metadata
        // and the baseline's integer-rate OTIS stream (~30 ppm at 32 voices).
        m_sample_accum += uint64_t(main_cycles) * m_es5505.sample_rate();
        while (m_sample_accum >= 16000000ULL) {
            m_sample_accum -= 16000000ULL;
            generate_one_frame();
        }

        // Device edges precede the CPU dispatch due at this clock. Dispatch one
        // instruction at a time so IRQ recognition cannot depend on main blocks.
        if (!m_reset_asserted && m_cpu_runner) {
            m_cpu_accum += int64_t(main_cycles) * 15238090LL;
            if (m_cpu_accum >= 16000000LL)
                m_cpu_accum -= int64_t(m_cpu_runner(1)) * 16000000LL;
        } else {
            m_cpu_accum = 0;
        }
    }

    void advance(uint32_t main_cycles) {
        while (main_cycles > 0) {
            // Stop at each sample or CPU deadline, retaining fractional clocks
            // and complete-instruction overrun across arbitrary caller chunks.
            uint32_t step = main_cycles;
            const uint32_t rate = m_es5505.sample_rate();
            if (m_sample_accum + uint64_t(step) * rate >= 16000000ULL)
                step = uint32_t((16000000ULL - m_sample_accum + rate - 1) / rate);
            if (!m_reset_asserted && m_cpu_runner) {
                const uint32_t until_cpu = uint32_t((16000000LL - m_cpu_accum + 15238089LL) / 15238090LL);
                step = std::min(step, until_cpu);
            }
            main_cycles -= step;
            advance_slice(step);
        }
    }

    // Bus handlers
    uint8_t read8(uint32_t address) {
        address &= 0x00ffffff;

        // Work RAM (0x000000 - 0x03ffff, mirrored 4 times)
        if (address < 0x040000) {
            return m_work_ram[address & 0xffff];
        }

        // Shared DPRAM (0x140000 - 0x140fff, 8-bit even byte lane)
        if (address >= 0x140000 && address < 0x141000) {
            if ((address & 1) == 0) {
                uint32_t idx = (address - 0x140000) >> 1;
                return (m_shared_ram && idx < m_shared_ram_size) ? m_shared_ram[idx] : 0xff;
            }
            return 0xff;
        }

        // ES5505 OTIS (0x200000 - 0x20001f, 16-bit word)
        if (address >= 0x200000 && address < 0x200020) {
            uint16_t w = m_es5505.read((address >> 1) & 0x0f);
            return (address & 1) ? (w & 0xff) : (w >> 8);
        }

        // ES5510 DSP host interface (0x260000 - 0x2601ff, 8-bit odd byte lane)
        if (address >= 0x260000 && address < 0x260200) {
            if (address & 1) {
                return m_es5510.host_r((address >> 1) & 0xff);
            }
            return 0x00;
        }

        // MC68681 DUART (0x280000 - 0x28001f, 8-bit odd byte lane)
        if (address >= 0x280000 && address < 0x280020) {
            if (address & 1) {
                return m_duart.read((address >> 1) & 0x0f);
            }
            return 0xff;
        }

        // ES5505 Bank Table readback (0x300000 - 0x30003f)
        if (address >= 0x300000 && address < 0x300040) {
            uint16_t w = m_bank_table[(address >> 1) & 0x1f];
            return (address & 1) ? (w & 0xff) : (w >> 8);
        }

        // MB87078 Electronic Volume (0x340000 - 0x340003, 8-bit even byte lane)
        if (address >= 0x340000 && address < 0x340004) {
            if ((address & 1) == 0) {
                uint32_t offset = (address >> 1) & 1;
                return m_volume.read(offset ^ 1);
            }
            return 0xff;
        }

        // Sound Program ROM (0xc00000 - 0xc7ffff, linearly mapped 512KB)
        if (address >= 0xc00000 && address < 0xc80000) {
            uint32_t off = address - 0xc00000;
            return (off < m_sound_rom.size()) ? m_sound_rom[off] : 0xff;
        }

        // Work RAM mirror (0xff0000 - 0xffffff)
        if (address >= 0xff0000) {
            return m_work_ram[address & 0xffff];
        }

        return 0xff;
    }

    uint16_t read16(uint32_t address) {
        address &= 0x00fffffe;

        // Work RAM
        if (address < 0x040000) {
            uint32_t off = address & 0xfffe;
            return (uint16_t(m_work_ram[off]) << 8) | m_work_ram[off + 1];
        }

        // Shared DPRAM (even byte has data, odd byte unmapped)
        if (address >= 0x140000 && address < 0x141000) {
            uint32_t idx = (address - 0x140000) >> 1;
            uint8_t data = (m_shared_ram && idx < m_shared_ram_size) ? m_shared_ram[idx] : 0xff;
            return (uint16_t(data) << 8) | 0xff;
        }

        // ES5505 OTIS (16-bit word)
        if (address >= 0x200000 && address < 0x200020) {
            return m_es5505.read((address >> 1) & 0x0f);
        }

        // ES5510 DSP host interface (odd byte has data)
        if (address >= 0x260000 && address < 0x260200) {
            return m_es5510.host_r((address >> 1) & 0xff);
        }

        // MC68681 DUART (odd byte has data)
        if (address >= 0x280000 && address < 0x280020) {
            return m_duart.read((address >> 1) & 0x0f);
        }

        // ES5505 Bank Table
        if (address >= 0x300000 && address < 0x300040) {
            return m_bank_table[(address >> 1) & 0x1f];
        }

        // MB87078 Electronic Volume
        if (address >= 0x340000 && address < 0x340004) {
            uint32_t offset = (address >> 1) & 1;
            return uint16_t(m_volume.read(offset ^ 1)) << 8;
        }

        // Sound Program ROM
        if (address >= 0xc00000 && address < 0xc80000) {
            uint32_t off = address - 0xc00000;
            if (off + 1 < m_sound_rom.size()) {
                return (uint16_t(m_sound_rom[off]) << 8) | m_sound_rom[off + 1];
            }
            return 0xffff;
        }

        // Work RAM mirror
        if (address >= 0xff0000) {
            uint32_t off = address & 0xfffe;
            return (uint16_t(m_work_ram[off]) << 8) | m_work_ram[off + 1];
        }

        return 0xffff;
    }

    uint32_t read32(uint32_t address) {
        return (uint32_t(read16(address)) << 16) | read16(address + 2);
    }

    void write8(uint32_t address, uint8_t value) {
        address &= 0x00ffffff;

        // Work RAM
        if (address < 0x040000) {
            m_work_ram[address & 0xffff] = value;
            return;
        }

        // Shared DPRAM (even byte only)
        if (address >= 0x140000 && address < 0x141000) {
            if ((address & 1) == 0) {
                uint32_t idx = (address - 0x140000) >> 1;
                if (m_shared_ram && idx < m_shared_ram_size) {
                    m_shared_ram[idx] = value;
                }
            }
            return;
        }

        // ES5505 OTIS
        if (address >= 0x200000 && address < 0x200020) {
            uint32_t offset = (address >> 1) & 0x0f;
            uint16_t mem_mask = (address & 1) ? 0x00ff : 0xff00;
            uint16_t data = (address & 1) ? value : (uint16_t(value) << 8);
            m_es5505.write(offset, data, mem_mask);
            return;
        }

        // ES5510 DSP host interface (odd byte only)
        if (address >= 0x260000 && address < 0x260200) {
            if (address & 1) {
                m_es5510.host_w((address >> 1) & 0xff, value);
            }
            return;
        }

        // MC68681 DUART (odd byte only)
        if (address >= 0x280000 && address < 0x280020) {
            if (address & 1) {
                m_duart.write((address >> 1) & 0x0f, value);
            }
            return;
        }

        // ES5505 Bank Table (32 words)
        if (address >= 0x300000 && address < 0x300040) {
            uint32_t voice = (address >> 1) & 0x1f;
            if (address & 1) {
                m_bank_table[voice] = (m_bank_table[voice] & 0xff00) | value;
            } else {
                m_bank_table[voice] = (m_bank_table[voice] & 0x00ff) | (uint16_t(value) << 8);
            }
            m_es5505.set_voice_bank(voice, (m_bank_table[voice] & m_bank_mask) << 20);
            return;
        }

        // MB87078 Volume (even byte only)
        if (address >= 0x340000 && address < 0x340004) {
            if ((address & 1) == 0) {
                uint32_t offset = (address >> 1) & 1;
                m_volume.write(offset ^ 1, value);
            }
            return;
        }

        // Work RAM mirror
        if (address >= 0xff0000) {
            m_work_ram[address & 0xffff] = value;
            return;
        }
    }

    void write16(uint32_t address, uint16_t value) {
        address &= 0x00fffffe;

        // Work RAM
        if (address < 0x040000) {
            uint32_t off = address & 0xfffe;
            m_work_ram[off] = (value >> 8) & 0xff;
            m_work_ram[off + 1] = value & 0xff;
            return;
        }

        // Shared DPRAM
        if (address >= 0x140000 && address < 0x141000) {
            uint32_t idx = (address - 0x140000) >> 1;
            if (m_shared_ram && idx < m_shared_ram_size) {
                m_shared_ram[idx] = (value >> 8) & 0xff;
            }
            return;
        }

        // ES5505 OTIS
        if (address >= 0x200000 && address < 0x200020) {
            m_es5505.write((address >> 1) & 0x0f, value, 0xffff);
            return;
        }

        // ES5510 DSP host interface
        if (address >= 0x260000 && address < 0x260200) {
            m_es5510.host_w((address >> 1) & 0xff, value & 0xff);
            return;
        }

        // MC68681 DUART
        if (address >= 0x280000 && address < 0x280020) {
            m_duart.write((address >> 1) & 0x0f, value & 0xff);
            return;
        }

        // ES5505 Bank Table
        if (address >= 0x300000 && address < 0x300040) {
            uint32_t voice = (address >> 1) & 0x1f;
            m_bank_table[voice] = value;
            m_es5505.set_voice_bank(voice, (value & m_bank_mask) << 20);
            return;
        }

        // MB87078 Volume
        if (address >= 0x340000 && address < 0x340004) {
            uint32_t offset = (address >> 1) & 1;
            m_volume.write(offset ^ 1, (value >> 8) & 0xff);
            return;
        }

        // Work RAM mirror
        if (address >= 0xff0000) {
            uint32_t off = address & 0xfffe;
            m_work_ram[off] = (value >> 8) & 0xff;
            m_work_ram[off + 1] = value & 0xff;
            return;
        }
    }

    void write32(uint32_t address, uint32_t value) {
        write16(address, uint16_t(value >> 16));
        write16(address + 2, uint16_t(value & 0xffff));
    }

    size_t state_size() const {
        return sizeof(CanonicalAudioCore) +
               WORK_RAM_SIZE +
               sizeof(float) * RING_BUFFER_CAPACITY * 2 +
               m_es5505.state_size() +
               m_es5510.state_size() +
               m_duart.state_size() +
               m_volume.state_size();
    }

    void save_state(StateWriter &writer) const {
        std::lock_guard<std::mutex> lock(m_audio_mutex);
        CanonicalAudioCore core{};
        core.bank_mask = m_bank_mask;
        for (int i = 0; i < ES5505::MAX_VOICES; ++i) core.bank_table[i] = m_bank_table[i];
        core.reset_asserted = m_reset_asserted ? 1 : 0;
        core.esp_halted = m_esp_halted ? 1 : 0;
        core.gain_model = uint8_t(m_gain_model);
        core.volume_gain[0] = m_volume_gain[0]; core.volume_gain[1] = m_volume_gain[1];
        core.otis_gain[0] = m_otis_gain[0]; core.otis_gain[1] = m_otis_gain[1];
        core.output_gain[0] = m_output_gain[0]; core.output_gain[1] = m_output_gain[1];
        core.cpu_accum = m_cpu_accum;
        core.duart_accum = m_duart_accum;
        core.sample_accum = m_sample_accum;
        core.clock_ticks = m_clock_ticks;
        core.generated_frames = m_generated_frames;
        core.rb_count = uint32_t(m_rb_count);
        writer.write(core);

        writer.write_bytes(m_work_ram.data(), WORK_RAM_SIZE);

        // Fast block copy for canonical ring buffer: m_rb_count frames in play order, remainder 0.0f
        if (m_rb_count > 0) {
            size_t first_part = std::min(m_rb_count, RING_BUFFER_CAPACITY - m_rb_read_pos);
            writer.write_bytes(&m_sample_buffer[m_rb_read_pos * 2], first_part * 2 * sizeof(float));
            if (m_rb_count > first_part) {
                size_t second_part = m_rb_count - first_part;
                writer.write_bytes(&m_sample_buffer[0], second_part * 2 * sizeof(float));
            }
        }
        size_t zero_frames = RING_BUFFER_CAPACITY - m_rb_count;
        if (zero_frames > 0) {
            size_t bytes = zero_frames * 2 * sizeof(float);
            if (writer.remaining() < bytes) throw std::runtime_error("StateWriter buffer overflow");
            std::memset(writer.current(), 0, bytes);
            writer.advance(bytes);
        }

        m_es5505.save_state(writer);
        m_es5510.save_state(writer);
        m_duart.save_state(writer);
        m_volume.save_state(writer);
    }

    void load_state(StateReader &reader) {
        std::lock_guard<std::mutex> lock(m_audio_mutex);
        CanonicalAudioCore core;
        reader.read(core);
        m_bank_mask = core.bank_mask;
        for (int i = 0; i < ES5505::MAX_VOICES; ++i) m_bank_table[i] = core.bank_table[i];
        m_reset_asserted = core.reset_asserted != 0;
        m_esp_halted = core.esp_halted != 0;
        m_gain_model = Audio::GainModel(core.gain_model);
        m_volume_gain[0] = core.volume_gain[0]; m_volume_gain[1] = core.volume_gain[1];
        m_otis_gain[0] = core.otis_gain[0]; m_otis_gain[1] = core.otis_gain[1];
        m_output_gain[0] = core.output_gain[0]; m_output_gain[1] = core.output_gain[1];
        m_cpu_accum = core.cpu_accum;
        m_duart_accum = core.duart_accum;
        m_sample_accum = core.sample_accum;
        m_clock_ticks = core.clock_ticks;
        m_generated_frames = core.generated_frames;

        reader.read_bytes(m_work_ram.data(), WORK_RAM_SIZE);

        m_rb_count = std::min(size_t(core.rb_count), RING_BUFFER_CAPACITY);
        m_rb_read_pos = 0;
        m_rb_write_pos = m_rb_count % RING_BUFFER_CAPACITY;
        if (m_rb_count > 0) {
            reader.read_bytes(m_sample_buffer.data(), m_rb_count * 2 * sizeof(float));
        }
        std::memset(&m_sample_buffer[m_rb_count * 2], 0, (RING_BUFFER_CAPACITY - m_rb_count) * 2 * sizeof(float));
        reader.skip((RING_BUFFER_CAPACITY - m_rb_count) * 2 * sizeof(float));

        m_es5505.load_state(reader);
        m_es5510.load_state(reader);
        m_duart.load_state(reader);
        m_volume.load_state(reader);
        update_gains();
    }
};

Audio::Audio() : m_impl(std::make_unique<Impl>()) {}
Audio::~Audio() = default;

Audio::Audio(Audio &&) noexcept = default;
Audio &Audio::operator=(Audio &&) noexcept = default;

void Audio::set_backend(Backend backend) {
    if (clock_ticks() || !is_reset())
        throw std::runtime_error("Select audio backend before machine execution");
    if (backend == Backend::Hle && !m_impl->m_hle)
        m_impl->m_hle = std::make_unique<hle::AudioEngine>(
            m_impl->m_sound_rom, m_impl->m_sample_words, m_impl->m_shared_ram);
    else if (backend == Backend::Accurate) m_impl->m_hle.reset();
}
Audio::Backend Audio::backend() const { return m_impl->m_hle ? Backend::Hle : Backend::Accurate; }
void Audio::shared_write(uint32_t offset, uint64_t frame) {
    if (m_impl->m_hle) m_impl->m_hle->shared_write(offset, frame);
}
void Audio::begin_frame(uint64_t frame) {
    if (m_impl->m_hle) m_impl->m_hle->begin_frame(frame);
}
void Audio::finish_frame(uint64_t frame) {
    if (m_impl->m_hle) m_impl->m_hle->finish_frame(frame);
}
void Audio::begin_rollback(uint64_t begin, uint64_t end) {
    if (m_impl->m_hle) m_impl->m_hle->begin_rollback(begin, end);
}
void Audio::end_rollback() { if (m_impl->m_hle) m_impl->m_hle->end_rollback(); }
Audio::HleStats Audio::hle_stats() const {
    return m_impl->m_hle ? m_impl->m_hle->stats() : HleStats{};
}
void Audio::set_hle_observer(std::function<void(const hle::VoiceEvent &)> observer) {
    if (!m_impl->m_hle) throw std::logic_error("HLE observer requires HLE backend");
    m_impl->m_hle->set_observer(std::move(observer));
}

void Audio::load_sound_rom(std::span<const uint8_t> sound_rom) {
    if (m_impl->m_hle) throw std::logic_error("Load ROMs before selecting HLE audio");
    m_impl->m_sound_rom.assign(sound_rom.begin(), sound_rom.end());
    if (m_impl->m_sound_rom.size() >= 8) {
        std::memcpy(m_impl->m_work_ram.data(), m_impl->m_sound_rom.data(), 8);
    }
}

void Audio::load_sample_rom(std::span<const uint8_t> sample_rom) {
    if (m_impl->m_hle) throw std::logic_error("Load ROMs before selecting HLE audio");
    size_t word_count = sample_rom.size() / 2;
    m_impl->m_sample_words.resize(word_count);
    for (size_t i = 0; i < word_count; i++) {
        m_impl->m_sample_words[i] = (uint16_t(sample_rom[i * 2]) << 8) | sample_rom[i * 2 + 1];
    }
    m_impl->m_es5505.set_sample_rom(m_impl->m_sample_words.data(), m_impl->m_sample_words.size());
    m_impl->m_bank_mask = (sample_rom.size() >= 0x200000) ? ((sample_rom.size() / 0x200000) - 1) : 0;
}

void Audio::set_shared_ram(uint8_t *shared_ram, size_t size) {
    if (m_impl->m_hle) throw std::logic_error("Configure shared RAM before selecting HLE audio");
    m_impl->m_shared_ram = shared_ram;
    m_impl->m_shared_ram_size = size;
}

void Audio::set_cpu_runner(std::function<int(int cycles)> runner) {
    m_impl->m_cpu_runner = runner;
}

void Audio::set_reset(bool asserted) {
    if (m_impl->m_hle) { m_impl->m_hle->set_reset(asserted); return; }
    m_impl->set_reset(asserted);
}

void Audio::reset_board() {
    if (m_impl->m_hle) { m_impl->m_hle->set_reset(true); return; }
    m_impl->reset_board();
}

uint64_t Audio::clock_ticks() const { return m_impl->m_hle ? m_impl->m_hle->clock() : m_impl->m_clock_ticks; }
uint64_t Audio::generated_frames() const { return m_impl->m_hle ? m_impl->m_hle->generated() : m_impl->m_generated_frames; }

bool Audio::is_reset() const {
    if (m_impl->m_hle) return m_impl->m_hle->is_reset();
    return m_impl->m_reset_asserted;
}

void Audio::set_reset_callback(std::function<void(bool asserted)> cb) {
    m_impl->m_reset_cb = cb;
}

void Audio::set_irq_callback(std::function<void(bool asserted)> cb) {
    m_impl->m_duart.set_irq_callback(std::move(cb));
}

uint8_t Audio::read8(uint32_t address) {
    return m_impl->read8(address);
}

uint16_t Audio::read16(uint32_t address) {
    return m_impl->read16(address);
}

uint32_t Audio::read32(uint32_t address) {
    return m_impl->read32(address);
}

void Audio::write8(uint32_t address, uint8_t value) {
    m_impl->write8(address, value);
}

void Audio::write16(uint32_t address, uint16_t value) {
    m_impl->write16(address, value);
}

void Audio::write32(uint32_t address, uint32_t value) {
    m_impl->write32(address, value);
}

int Audio::irq_level() const {
    // DUART timer/counter triggers IRQ level 6
    if (m_impl->m_duart.irq_pending()) {
        return 6;
    }
    return 0;
}

uint8_t Audio::irq_ack(int level) {
    if (level == 6) {
        return m_impl->m_duart.get_irq_vector();
    }
    return uint8_t(0x18 + level); // Standard 68000 autovector fallback
}

void Audio::advance(uint32_t main_cycles) {
    if (m_impl->m_hle) { m_impl->m_hle->advance(main_cycles); return; }
    m_impl->advance(main_cycles);
}

uint32_t Audio::sample_rate() const {
    if (m_impl->m_hle) return hle::sample_rate;
    return m_impl->m_es5505.sample_rate();
}

void Audio::set_gain_model(GainModel model) {
    if (m_impl->m_hle) { m_impl->m_hle->set_gain_model(model); return; }
    m_impl->m_gain_model = model;
    m_impl->update_gains();
}

size_t Audio::available_frames() const {
    if (m_impl->m_hle) return m_impl->m_hle->available();
    std::lock_guard<std::mutex> lock(m_impl->m_audio_mutex);
    return m_impl->m_rb_count;
}

size_t Audio::render(int16_t *interleaved_stereo, size_t max_frames) {
    if (m_impl->m_hle) return m_impl->m_hle->render(interleaved_stereo, max_frames);
    std::lock_guard<std::mutex> lock(m_impl->m_audio_mutex);
    size_t frames = std::min(max_frames, m_impl->m_rb_count);
    for (size_t i = 0; i < frames; i++) {
        float l = m_impl->m_sample_buffer[m_impl->m_rb_read_pos * 2 + 0];
        float r = m_impl->m_sample_buffer[m_impl->m_rb_read_pos * 2 + 1];
        m_impl->m_rb_read_pos = (m_impl->m_rb_read_pos + 1) % Impl::RING_BUFFER_CAPACITY;

        interleaved_stereo[i * 2 + 0] = clamp16(int32_t(std::clamp(l, -1.0f, 1.0f) * 32768.0f));
        interleaved_stereo[i * 2 + 1] = clamp16(int32_t(std::clamp(r, -1.0f, 1.0f) * 32768.0f));
    }
    m_impl->m_rb_count -= frames;
    return frames;
}

size_t Audio::render(float *interleaved_stereo, size_t max_frames) {
    if (m_impl->m_hle) return m_impl->m_hle->render(interleaved_stereo, max_frames);
    std::lock_guard<std::mutex> lock(m_impl->m_audio_mutex);
    size_t frames = std::min(max_frames, m_impl->m_rb_count);
    for (size_t i = 0; i < frames; i++) {
        interleaved_stereo[i * 2 + 0] = m_impl->m_sample_buffer[m_impl->m_rb_read_pos * 2 + 0];
        interleaved_stereo[i * 2 + 1] = m_impl->m_sample_buffer[m_impl->m_rb_read_pos * 2 + 1];
        m_impl->m_rb_read_pos = (m_impl->m_rb_read_pos + 1) % Impl::RING_BUFFER_CAPACITY;
    }
    m_impl->m_rb_count -= frames;
    return frames;
}

size_t Audio::state_size() const {
    if (m_impl->m_hle) return hle::AudioEngine::state_bytes;
    return m_impl->state_size();
}

void Audio::save_state(std::span<uint8_t> dst) const {
    if (m_impl->m_hle) { m_impl->m_hle->save(dst); return; }
    if (dst.size() != state_size()) {
        throw std::invalid_argument("Audio::save_state size mismatch");
    }
    StateWriter writer(dst);
    m_impl->save_state(writer);
    if (writer.remaining() != 0) {
        throw std::logic_error("Audio::save_state remaining unwritten bytes");
    }
}

void Audio::load_state(std::span<const uint8_t> src) {
    if (m_impl->m_hle) { m_impl->m_hle->load(src); return; }
    if (src.size() != state_size()) {
        throw std::invalid_argument("Audio::load_state size mismatch");
    }
    StateReader reader(src);
    m_impl->load_state(reader);
    if (reader.remaining() != 0) {
        throw std::logic_error("Audio::load_state remaining unread bytes");
    }
}
} // namespace f3rt
