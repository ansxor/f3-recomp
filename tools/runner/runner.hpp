#pragma once

#include "inputs.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "capture_io.hpp"
#include "debug/block_profile.hpp"
#include "audio/sound_trace.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace f3rt::runner {

struct ScheduledPacket {
    double time_seconds = 0.0;
    std::vector<uint8_t> packet;
    uint64_t tick = 0;
};

std::vector<uint8_t> parse_hex(std::string_view hex_str);
ScheduledPacket parse_at_schedule(std::string_view spec);

struct RingBufferState {
    static constexpr size_t RING_SIZE = 1024;
    static uint16_t read_producer(const Machine &m) {
        return (uint16_t(m.shared[0x480]) << 8) | m.shared[0x481];
    }
    static uint16_t read_consumer(const Machine &m) {
        return (uint16_t(m.shared[0x482]) << 8) | m.shared[0x483];
    }
    static size_t available_bytes(const Machine &m) {
        uint16_t prod_d = read_producer(m);
        uint16_t cons_d = read_consumer(m);
        uint16_t occupied_doubled = (prod_d - cons_d) & 0x7fe;
        size_t occupied = occupied_doubled / 2;
        if (occupied >= RING_SIZE - 1) return 0;
        return (RING_SIZE - 1) - occupied;
    }
};

void publish_packet(Machine &m, const std::vector<uint8_t> &pkt, uint64_t current_cycles);

inline void crc_byte(uint32_t &crc, uint8_t byte) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
}

// Drain all pending audio samples from machine into a vector.
inline std::vector<int16_t> drain_audio_samples(Machine &m) {
    std::array<int16_t, 8192> block{};
    std::vector<int16_t> result;
    while (auto count = m.audio->render(block.data(), block.size() / 2)) {
        result.insert(result.end(), block.begin(), block.begin() + count * 2);
    }
    return result;
}

// Creates and initializes a strict-native Machine with recompiled code, native sound, and optional GPU presentation.
std::unique_ptr<Machine> create_strict_machine(const std::filesystem::path &rom_dir,
                                               const std::string &set = "landmakrj",
                                               bool enable_gpu = false,
                                               unsigned gpu_scale = 1);

struct RunnerConfig {
    std::filesystem::path rom_dir;
    std::string set = "landmakrj";
    std::string sound_driver = "oracle"; // "oracle" or "native"
    Audio::Backend audio_backend = Audio::Backend::Reference;
    std::filesystem::path sound_trace_path;
    std::filesystem::path wav_path;
    std::string wav_window = "full"; // "full" or "event"
    std::filesystem::path profile_path;
    std::filesystem::path hle_events_path;
    std::filesystem::path dump_dir;
    bool translated = true;
    bool allow_fallback = false;
    bool record_sprite_writers = false;
    bool video_diff = false;
    unsigned video_layer_mask = 511;
    unsigned video_diff_every = 120;
    std::shared_ptr<InputSource> input_source;
};

class MachineRunner {
public:
    explicit MachineRunner(RunnerConfig config);
    ~MachineRunner();

    Machine &machine() { return *machine_; }
    const Machine &machine() const { return *machine_; }
    const RunnerConfig &config() const { return config_; }

    // Run N emulated frames. Optional per_frame_cb is called right after run_frame().
    // If per_frame_cb returns false, execution stops early.
    bool run_frames(uint64_t target_frames, std::function<bool(uint64_t frame)> per_frame_cb = nullptr);

    // Drain audio into buffer/wav and update stats.
    void drain_audio(bool record_to_wav = true);

    // Save final screen frame as BMP
    void save_bmp(const std::filesystem::path &path);

    // Finish audio/trace/profile sessions
    void finish();

    // Stats
    uint64_t audio_frames() const { return total_audio_frames_; }
    uint64_t nonzero_samples() const { return nonzero_samples_; }
    int audio_peak() const { return audio_peak_; }
    uint32_t audio_crc() const { return audio_crc_; }

private:
    RunnerConfig config_;
    std::unique_ptr<Machine> machine_;
    std::unique_ptr<WavWriter> wav_;
    std::unique_ptr<BlockProfileSession> profile_;
    std::array<int16_t, 8192> audio_buffer_{};
    uint64_t total_audio_frames_ = 0;
    uint64_t nonzero_samples_ = 0;
    int audio_peak_ = 0;
    uint32_t audio_crc_ = 0xffffffffu;
};

} // namespace f3rt::runner
