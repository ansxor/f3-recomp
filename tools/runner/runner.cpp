#include "runner.hpp"
#include "f3rt/rom.hpp"

#if defined(F3RT_GAME_VIDEO)
#include "f3rt/game_video.hpp"
#endif

#if defined(F3RT_GENERATED)
#include "program.h"
#endif

#if defined(F3RT_SOUND_GENERATED)
#include "sound_program.h"
#endif

#include "audio/hle/events.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace f3rt::runner {

std::unique_ptr<Machine> create_strict_machine(const std::filesystem::path &rom_dir,
                                               const std::string &set,
                                               bool enable_gpu,
                                               unsigned gpu_scale) {
    if (rom_dir.empty()) {
        throw std::runtime_error("ROM directory must be specified");
    }
    auto machine = std::make_unique<Machine>(RomSet::load(rom_dir, set));
    machine->allow_main_fallback = false;
#if defined(F3RT_GENERATED)
    if (!f3_generated_register(&machine->cpu)) {
        throw std::runtime_error("f3_generated_register failed to register recompiled blocks");
    }
#else
    throw std::runtime_error("Strict-native machine requires compiled generated blocks (F3RT_GENERATED)");
#endif
#if defined(F3RT_SOUND_GENERATED)
    machine->use_native_sound(f3_sound_blocks, f3_sound_block_count,
                              {f3_sound_excluded_ranges, f3_sound_excluded_count},
                              f3_sound_rom_crc32);
#endif
#if defined(F3RT_GAME_VIDEO)
    if (enable_gpu) {
        GameVideoOptions vopts;
        vopts.scale = gpu_scale;
        machine->game_video = std::make_unique<GameVideo>(*machine, GameVideoMode::Game, vopts);
        machine->game_video->enable_gpu_presentation();
        machine->game_video->set_gpu_scale(gpu_scale);
    }
#endif
    return machine;
}

std::vector<uint8_t> parse_hex(std::string_view hex_str) {
    std::string clean;
    clean.reserve(hex_str.size());
    for (char c : hex_str) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        clean.push_back(c);
    }
    if (clean.empty()) {
        throw std::runtime_error("Empty packet hex string");
    }
    if (clean.size() % 2 != 0) {
        throw std::runtime_error("Packet hex string length must be even: " + std::string(hex_str));
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(clean.size() / 2);
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < clean.size(); i += 2) {
        int hi = hex_val(clean[i]);
        int lo = hex_val(clean[i + 1]);
        if (hi < 0 || lo < 0) {
            throw std::runtime_error("Invalid hex character in packet: " + std::string(hex_str));
        }
        bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }

    if (bytes.size() < 2) {
        throw std::runtime_error("Packet too short (minimum 2 bytes: length prefix and opcode): " + std::string(hex_str));
    }
    if (bytes[0] != bytes.size()) {
        std::ostringstream ss;
        ss << "Packet length prefix mismatch in '" << hex_str << "': byte 0 is " << int(bytes[0])
           << " but packet byte count is " << bytes.size();
        throw std::runtime_error(ss.str());
    }
    return bytes;
}

ScheduledPacket parse_at_schedule(std::string_view spec) {
    auto colon = spec.find(':');
    if (colon == std::string_view::npos) {
        throw std::runtime_error("Malformed --at specification (expected SECONDS:HEX): " + std::string(spec));
    }
    std::string sec_str(spec.substr(0, colon));
    std::string hex_str(spec.substr(colon + 1));
    double sec = 0.0;
    try {
        size_t idx = 0;
        sec = std::stod(sec_str, &idx);
        if (idx != sec_str.size()) throw std::invalid_argument("trailing characters");
    } catch (const std::exception &e) {
        throw std::runtime_error("Invalid seconds in --at schedule '" + std::string(spec) + "': " + e.what());
    }
    if (sec < 0.0 || std::isnan(sec) || std::isinf(sec)) {
        throw std::runtime_error("Scheduled time in --at must be non-negative: " + std::string(spec));
    }
    auto packet = parse_hex(hex_str);
    if (sec > double(UINT64_MAX / Machine::main_clock)) {
        throw std::runtime_error("Scheduled time exceeds the trace clock range");
    }
    return ScheduledPacket{sec, std::move(packet), uint64_t(std::round(sec * Machine::main_clock))};
}

void publish_packet(Machine &m, const std::vector<uint8_t> &pkt, uint64_t current_cycles) {
    uint16_t prod_d = RingBufferState::read_producer(m);
    size_t prod_byte = (prod_d / 2) & 0x3ff;

    m.cpu.pc = 0xffffffff;
    m.cpu.cycles = current_cycles;

    for (size_t i = 0; i < pkt.size(); ++i) {
        uint32_t ring_addr = 0xc00000 + ((prod_byte + i) & 0x3ff);
        m.write8(ring_addr, pkt[i]);
    }

    size_t next_prod_byte = (prod_byte + pkt.size()) & 0x3ff;
    uint16_t new_prod_d = uint16_t((next_prod_byte * 2) & 0x7fe);

    m.write8(0xc00480, uint8_t(new_prod_d >> 8));
    m.write8(0xc00481, uint8_t(new_prod_d & 0xff));
}

MachineRunner::MachineRunner(RunnerConfig config)
    : config_(std::move(config)) {
    if (config_.rom_dir.empty()) {
        throw std::runtime_error("ROM directory must be specified");
    }

    machine_ = std::make_unique<Machine>(RomSet::load(config_.rom_dir, config_.set));
    machine_->allow_main_fallback = config_.allow_fallback;

    if (config_.record_sprite_writers || !config_.dump_dir.empty()) {
        machine_->sprite_writers = std::make_unique<std::array<uint32_t, 0x1000>>();
    }

    if (!config_.profile_path.empty()) {
        profile_ = std::make_unique<BlockProfileSession>(machine_->roms, config_.profile_path);
    }

    if (!config_.sound_trace_path.empty()) {
        machine_->sound_trace = std::make_unique<SoundTrace>(config_.sound_trace_path);
    }

    if (config_.audio_backend == Audio::Backend::Enhanced) {
        machine_->audio->set_backend(Audio::Backend::Enhanced);
        if (!config_.hle_events_path.empty()) {
            auto events = std::make_shared<std::ofstream>(config_.hle_events_path);
            if (!*events) throw std::runtime_error("Cannot create HLE event CSV");
            *events << "kind,instance,tick,sequence,track,key,layer,pair,start,end,frequency,left,right,k1,k2,loop,reverse\n";
            machine_->audio->set_hle_observer([events](const hle::VoiceEvent &e) {
                *events << unsigned(e.kind) << ',' << e.instance << ',' << e.tick << ','
                        << unsigned(e.sequence) << ',' << unsigned(e.track) << ',' << unsigned(e.key) << ','
                        << unsigned(e.layer) << ',' << unsigned(e.output_pair) << ','
                        << e.sample_start << ',' << e.sample_end << ',' << e.frequency << ','
                        << e.left_volume << ',' << e.right_volume << ',' << e.k1 << ',' << e.k2 << ','
                        << e.loop << ',' << e.reverse << '\n';
            });
        }
    } else if (config_.sound_driver == "native") {
#if defined(F3RT_SOUND_GENERATED)
        machine_->use_native_sound(f3_sound_blocks, f3_sound_block_count,
                                   {f3_sound_excluded_ranges, f3_sound_excluded_count},
                                   f3_sound_rom_crc32);
#else
        throw std::runtime_error("Native sound requires a generated sound program (F3_ROM_DIR)");
#endif
    }

    if (!config_.wav_path.empty()) {
        wav_ = std::make_unique<WavWriter>(config_.wav_path, machine_->audio->sample_rate());
    }

    if (config_.video_diff) {
#if defined(F3RT_GAME_VIDEO)
        if (!config_.video_diff_every || !config_.video_layer_mask || (config_.video_layer_mask & ~511u)) {
            throw std::runtime_error("Video diff requires a positive interval and nine-layer mask");
        }
        machine_->game_video = std::make_unique<GameVideo>(*machine_);
#else
        throw std::runtime_error("Video diff requires F3RT_GAME_VIDEO");
#endif
    }

    if (config_.translated) {
#if defined(F3RT_GENERATED)
        if (!f3_generated_register(&machine_->cpu)) {
            throw std::runtime_error("f3_generated_register failed to register recompiled blocks");
        }
#else
        throw std::runtime_error("Strict native mode requires compiled generated blocks (F3RT_GENERATED)");
#endif
    }
}

MachineRunner::~MachineRunner() = default;

void MachineRunner::drain_audio(bool record_to_wav) {
    size_t count = 0;
    while ((count = machine_->audio->render(audio_buffer_.data(), audio_buffer_.size() / 2)) != 0) {
        total_audio_frames_ += count;
        if (wav_ && record_to_wav) {
            wav_->append(std::span(audio_buffer_.data(), count * 2));
        }
        for (size_t i = 0; i < count * 2; ++i) {
            audio_peak_ = std::max(audio_peak_, std::abs(int(audio_buffer_[i])));
            nonzero_samples_ += (audio_buffer_[i] != 0);
            crc_byte(audio_crc_, uint8_t(audio_buffer_[i]));
            crc_byte(audio_crc_, uint8_t(uint16_t(audio_buffer_[i]) >> 8));
        }
    }
}

bool MachineRunner::run_frames(uint64_t target_frames, std::function<bool(uint64_t frame)> per_frame_cb) {
    while (machine_->frame < target_frames) {
        if (profile_) profile_->tick();

        if (config_.input_source) {
            config_.input_source->apply(*machine_, machine_->frame);
        }

        bool ok = machine_->run_frame(config_.translated);
        if (!ok || machine_->cpu.halted) {
            return false;
        }

        drain_audio(config_.wav_window == "full");

        if (per_frame_cb && !per_frame_cb(machine_->frame)) {
            return false;
        }
    }
    return true;
}

void MachineRunner::save_bmp(const std::filesystem::path &path) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    write_bmp(path, machine_->native_pixels());
}

void MachineRunner::finish() {
    if (machine_->sound_trace) {
        machine_->sound_trace->finish(*machine_);
    }
    if (profile_) {
        profile_->flush();
    }
}

} // namespace f3rt::runner
