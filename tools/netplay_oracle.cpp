#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/game_video.hpp"
#include "f3rt/rom.hpp"
#include "f3rt/netplay.hpp"
#include "f3rt/netplay_session.hpp"
#include "eeprom.hpp"
#include "capture_io.hpp"
#include "sound_trace.hpp"
#include "gameplay_inputs.hpp"
#include "hle_events.hpp"

#ifdef F3RT_GENERATED
#include "program.h"
#endif
#ifdef F3RT_SOUND_GENERATED
#include "sound_program.h"
#endif

#include <algorithm>
#include "sync_snapshot_proof.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define GETPID _getpid
#else
#include <unistd.h>
#define GETPID getpid
#endif

// -----------------------------------------------------------------------------
// Global Allocation Tracker (Active strictly during save_state / load_state)
// -----------------------------------------------------------------------------
static std::atomic<bool> g_track_allocations{false};
static std::atomic<uint64_t> g_allocation_count{0};
static std::atomic<size_t> g_allocated_bytes{0};

void *operator new(size_t size) {
    if (g_track_allocations.load(std::memory_order_relaxed)) {
        g_allocation_count.fetch_add(1, std::memory_order_relaxed);
        g_allocated_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    void *ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}

void operator delete(void *ptr) noexcept {
    std::free(ptr);
}

void operator delete(void *ptr, size_t) noexcept {
    std::free(ptr);
}

namespace {

// RAII temporary directory remover to guarantee cleanup on normal exit or exception
struct ScopedDirRemover {
    std::filesystem::path dir;
    ~ScopedDirRemover() {
        if (!dir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    }
};

// Running CRC-32 update for streaming audio PCM verification
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(crc & 1u)));
        }
    }
    return crc;
}

struct LandMakerStatus {
    uint16_t ram_probe{0};
    uint8_t flags_val{0};
    bool p1_active{false};
    bool p2_active{false};
    bool is_versus{false};
    std::string versus_status{"UNVERIFIED"};
};

LandMakerStatus inspect_landmaker(f3rt::Machine &m) {
    LandMakerStatus st;
    st.ram_probe = m.read16(0x00401f6e);
    st.flags_val = m.read8(0x00401f53);

    st.p1_active = (st.flags_val & 0x01) != 0;
    st.p2_active = (st.flags_val & 0x02) != 0;
    st.is_versus = f3rt::netplay::versus_match_active(m);
    if (st.is_versus) {
        st.versus_status = "VERSUS";
    } else if (st.p1_active || st.p2_active) {
        st.versus_status = "LOCAL_OR_DEMO";
    } else {
        st.versus_status = "IDLE";
    }
    return st;
}

void configure_machine(f3rt::Machine &m, const std::string &sound_driver,
                       const f3rt::GameVideoOptions &video_opts,
                       f3rt::Audio::Backend backend = f3rt::Audio::Backend::Accurate) {
    m.allow_main_fallback = false;

#ifdef F3RT_GENERATED
    if (!f3_generated_register(&m.cpu)) {
        throw std::runtime_error("f3_generated_register failed to register recompiled blocks");
    }
#else
    throw std::runtime_error("netplay_oracle requires compiled generated blocks (F3RT_GENERATED)");
#endif

    if (backend == f3rt::Audio::Backend::Hle) m.audio->set_backend(backend);
    else if (sound_driver == "native") {
#ifdef F3RT_SOUND_GENERATED
        m.use_native_sound(f3_sound_blocks, f3_sound_block_count,
                           {f3_sound_excluded_ranges, f3_sound_excluded_count});
#else
        throw std::runtime_error("Native sound requires a generated sound program (F3_ROM_DIR)");
#endif
    } else if (sound_driver != "oracle") {
        throw std::runtime_error("Unknown sound driver: " + sound_driver);
    }

    // Per contract and parent guidance: GameVideoMode::Game matching frontend
    m.game_video = std::make_unique<f3rt::GameVideo>(m, f3rt::GameVideoMode::Game, video_opts);
}

std::array<uint16_t,2> match_inputs(uint64_t seed, unsigned match, uint64_t frame) {
    std::array<uint16_t,2> words{};
    for(unsigned player=0;player<2;++player) {
        uint64_t value=seed ^ (uint64_t(match+1)*0x9e3779b97f4a7c15ull) ^
            (uint64_t(player+1)*0xd1b54a32d192ed03ull) ^ (frame/6);
        value=(value^(value>>30))*0xbf58476d1ce4e5b9ull;
        value=(value^(value>>27))*0x94d049bb133111ebull;
        value^=value>>31;
        constexpr uint16_t directions[]={0,1,2,4,8};
        words[player]=directions[value%5] | uint16_t(((value>>8)&7)<<4);
    }
    return words;
}

std::vector<uint8_t> read_file_bytes(const std::filesystem::path &p) {
    std::ifstream is(p, std::ios::binary | std::ios::ate);
    if (!is) {
        throw std::runtime_error("Sound trace file failed to open or missing: " + p.string());
    }
    auto sz = is.tellg();
    if (sz <= 0) {
        throw std::runtime_error("Sound trace file is empty: " + p.string());
    }
    is.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    is.read(reinterpret_cast<char *>(buf.data()), sz);
    if (!is) {
        throw std::runtime_error("Failed to read all bytes from sound trace: " + p.string());
    }
    return buf;
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = size_t(std::clamp(p * v.size(), 0.0, double(v.size() - 1)));
    return v[idx];
}

int calc_affordable_depth(double step_us, double save_us, double load_us) {
    constexpr double frame_budget_us = 16666.67; // 1 frame at 60Hz
    double denom = step_us + save_us;
    if (denom <= 0.0) return 0;
    double d = (frame_budget_us - load_us) / denom - 1.0;
    return d > 0.0 ? int(std::floor(d)) : 0;
}

// -----------------------------------------------------------------------------
// Mode 1: Snapshot save/load proof and performance benchmark
// -----------------------------------------------------------------------------
int run_snapshot_proof(const std::filesystem::path &romdir, const std::string &set,
                       uint64_t seed, uint64_t total_frames,
                       const std::vector<uint64_t> &test_frames,
                       const std::vector<uint64_t> &k_depths,
                       const std::string &sound_driver,
                       const std::string &schedule_type,
                       const f3rt::GameVideoOptions &video_opts,
                       const std::filesystem::path &capture_surface,
                       const std::filesystem::path &dump_dir) {
    const bool is_vs_schedule = (schedule_type != "single");
    std::cout << "--- SNAPSHOT PROOF & PERFORMANCE BENCHMARK ---\n"
              << "seed=" << seed << " frames=" << total_frames
              << " sound_driver=" << sound_driver
              << " schedule=" << (is_vs_schedule ? "versus" : "single") << '\n';

    f3rt::Machine m(f3rt::RomSet::load(romdir, set));
    configure_machine(m, sound_driver, video_opts);

    const size_t state_sz = m.state_size();
    std::cout << "Configured machine state size: " << state_sz << " bytes\n";
    if (state_sz == 0) {
        throw std::runtime_error("Machine::state_size returned 0");
    }

    std::vector<uint8_t> saved_state(state_sz);
    f3rt::test::GameplaySchedule schedule(seed, {.versus = is_vs_schedule});

    std::vector<double> save_times_us;
    std::vector<double> load_times_us;
    std::vector<double> midgame_step_times_us;
    size_t checks_performed = 0;
    size_t wall_clock_perturbations = 0;
    uint64_t total_save_allocations = 0;
    uint64_t total_load_allocations = 0;

    std::array<int16_t, 4096> audio_buf{};
    uint64_t total_audio_frames = 0;
    uint32_t cumulative_audio_crc = ~0u;

    const auto benchmark_start = std::chrono::steady_clock::now();
    const auto pid = GETPID();

    // Unique scoped temp dir to prevent collision across parallel instances
    const std::filesystem::path tmp_base = dump_dir.empty()
        ? std::filesystem::path("build/tmp_snapshots")
        : (dump_dir / "tmp_snapshots");
    std::filesystem::create_directories(tmp_base);

    for (uint64_t f = 0; f < total_frames; ++f) {
        // Pure single-frame step cost tracking during midgame frames (>= 1200) with audio drained
        bool is_midgame = (f >= 1200);

        // Check if current frame is a designated snapshot test point (including frame 0 cold boot!)
        bool is_test_point = (std::find(test_frames.begin(), test_frames.end(), f) != test_frames.end());

        if (is_test_point) {
            // Save state once at frame f
            g_allocation_count.store(0);
            g_track_allocations.store(true);
            auto t0 = std::chrono::steady_clock::now();
            m.save_state(saved_state);
            auto t1 = std::chrono::steady_clock::now();
            g_track_allocations.store(false);

            uint64_t save_allocs = g_allocation_count.load();
            total_save_allocations += save_allocs;
            save_times_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());

            if (save_allocs > 0) {
                throw std::runtime_error("save_state performed " + std::to_string(save_allocs) +
                                         " dynamic allocations; zero allocation contract violated");
            }

            const uint32_t base_crc = m.state_crc();

            // Test across each requested K depth from frame f
            for (uint64_t k_frames : k_depths) {
                if (f + k_frames > total_frames) continue;

                const std::filesystem::path scoped_tmp = tmp_base / (
                    "p" + std::to_string(pid) + "_s" + std::to_string(seed) +
                    "_" + sound_driver + "_f" + std::to_string(f) + "_k" + std::to_string(k_frames));
                std::filesystem::create_directories(scoped_tmp);
                ScopedDirRemover cleaner{scoped_tmp};

                // 2. Advance K frames on primary run, collecting ground truth outputs and sound trace
                std::vector<int16_t> pass1_audio;
                f3rt::test::GameplaySchedule k_sched = schedule; // clone schedule state

                const auto p1_sound_path = scoped_tmp / "pass1.sound";
                m.sound_trace = std::make_unique<f3rt::SoundTrace>(p1_sound_path);

                for (uint64_t k = 0; k < k_frames; ++k) {
                    auto words = k_sched.step(f + k);
                    f3rt::netplay::apply_inputs(m, words);
                    if (!m.run_frame(true) || m.fallback_instructions > 0) {
                        throw std::runtime_error("Strict-native failure during pass 1");
                    }
                    size_t pcm_cnt = 0;
                    while ((pcm_cnt = m.audio->render(audio_buf.data(), audio_buf.size() / 2)) != 0) {
                        pass1_audio.insert(pass1_audio.end(), audio_buf.begin(), audio_buf.begin() + pcm_cnt * 2);
                    }
                }

                m.sound_trace->finish(m);
                m.sound_trace.reset();

                const uint32_t pass1_final_crc = m.state_crc();
                const auto pass1_ram = m.ram;
                const auto pass1_palette = m.palette;
                const auto pass1_graphics = m.graphics;
                const auto pass1_control = m.control;
                const auto pass1_shared = m.shared;
                const auto pass1_pixels = m.pixels;
                const auto pass1_sound_bytes = read_file_bytes(p1_sound_path);

                // 3. Load state back to frame f and assert zero heap allocation
                g_allocation_count.store(0);
                g_track_allocations.store(true);
                auto t2 = std::chrono::steady_clock::now();
                m.load_state(saved_state);
                auto t3 = std::chrono::steady_clock::now();
                g_track_allocations.store(false);

                uint64_t load_allocs = g_allocation_count.load();
                total_load_allocations += load_allocs;
                load_times_us.push_back(std::chrono::duration<double, std::micro>(t3 - t2).count());

                if (load_allocs > 0) {
                    throw std::runtime_error("load_state performed " + std::to_string(load_allocs) +
                                             " dynamic allocations; zero allocation contract violated");
                }

                if (m.frame != f) {
                    throw std::runtime_error("Loaded state frame mismatch: expected " +
                                             std::to_string(f) + " got " + std::to_string(m.frame));
                }
                if (m.state_crc() != base_crc) {
                    throw std::runtime_error("Loaded state CRC mismatch immediately after load: expected 0x" +
                                             std::to_string(base_crc) + " got 0x" + std::to_string(m.state_crc()));
                }

                // 4. Re-run K frames under identical inputs (pass 2) with host wall-clock perturbation
                std::vector<int16_t> pass2_audio;
                k_sched = schedule; // reset clone

                const auto p2_sound_path = scoped_tmp / "pass2.sound";
                m.sound_trace = std::make_unique<f3rt::SoundTrace>(p2_sound_path);

                for (uint64_t k = 0; k < k_frames; ++k) {
                    // Wall-clock perturbation: sleep 1ms midway to prove host delay does not alter simulation
                    if (k == k_frames / 2) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        ++wall_clock_perturbations;
                    }

                    auto words = k_sched.step(f + k);
                    f3rt::netplay::apply_inputs(m, words);
                    if (!m.run_frame(true) || m.fallback_instructions > 0) {
                        throw std::runtime_error("Strict-native failure during pass 2 resimulation");
                    }
                    size_t pcm_cnt = 0;
                    while ((pcm_cnt = m.audio->render(audio_buf.data(), audio_buf.size() / 2)) != 0) {
                        pass2_audio.insert(pass2_audio.end(), audio_buf.begin(), audio_buf.begin() + pcm_cnt * 2);
                    }
                }

                m.sound_trace->finish(m);
                m.sound_trace.reset();

                const uint32_t pass2_final_crc = m.state_crc();
                const auto pass2_sound_bytes = read_file_bytes(p2_sound_path);

                // 5. Byte-by-byte equality checks across all outputs
                if (pass1_final_crc != pass2_final_crc) {
                    std::ostringstream ss;
                    ss << "State CRC mismatch after " << k_frames << " resimulated frames: pass1=0x"
                       << std::hex << pass1_final_crc << " pass2=0x" << pass2_final_crc;
                    throw std::runtime_error(ss.str());
                }

                if (pass1_ram != m.ram) {
                    for (size_t i = 0; i < pass1_ram.size(); ++i) {
                        if (pass1_ram[i] != m.ram[i]) {
                            throw std::runtime_error("Byte RAM mismatch at offset 0x" + std::to_string(i));
                        }
                    }
                }
                if (pass1_palette != m.palette) {
                    throw std::runtime_error("Byte Palette mismatch after resimulation");
                }
                if (pass1_graphics != m.graphics) {
                    throw std::runtime_error("Byte Graphics RAM mismatch after resimulation");
                }
                if (pass1_control != m.control) {
                    throw std::runtime_error("Byte Control RAM mismatch after resimulation");
                }
                if (pass1_shared != m.shared) {
                    throw std::runtime_error("Byte Sound Shared RAM mismatch after resimulation");
                }
                if (pass1_pixels != m.pixels) {
                    throw std::runtime_error("Byte Framebuffer pixels mismatch after resimulation");
                }
                if (pass1_audio != pass2_audio) {
                    throw std::runtime_error("Rendered audio PCM mismatch: pass1 samples=" +
                                             std::to_string(pass1_audio.size()) + " pass2 samples=" +
                                             std::to_string(pass2_audio.size()));
                }
                if (pass1_sound_bytes.empty() || pass2_sound_bytes.empty() ||
                    pass1_sound_bytes != pass2_sound_bytes) {
                    throw std::runtime_error("Sound trace records mismatch: pass1 bytes=" +
                                             std::to_string(pass1_sound_bytes.size()) + " pass2 bytes=" +
                                             std::to_string(pass2_sound_bytes.size()));
                }

                // Restore back to frame f for the next K depth
                m.load_state(saved_state);

                ++checks_performed;
                std::cout << "  [CHECK " << checks_performed << "] frame=" << f << " -> " << (f + k_frames)
                          << " (K=" << k_frames << ") state_crc=0x" << std::hex << pass1_final_crc
                          << std::dec << " save=" << std::fixed << std::setprecision(1) << save_times_us.back()
                          << "us load=" << load_times_us.back() << "us sound_records="
                          << pass1_sound_bytes.size() << "B: EXACT MATCH (allocations: 0)\n";
            }
        }

        // Normal step to advance machine to f + 1
        auto words = schedule.step(f);
        f3rt::netplay::apply_inputs(m, words);

        auto step_t0 = std::chrono::steady_clock::now();
        if (!m.run_frame(true) || m.fallback_instructions > 0) {
            throw std::runtime_error("Strict-native halted at frame " + std::to_string(f));
        }
        size_t pcm_cnt = 0;
        while ((pcm_cnt = m.audio->render(audio_buf.data(), audio_buf.size() / 2)) != 0) {
            total_audio_frames += pcm_cnt;
            cumulative_audio_crc = crc32_update(cumulative_audio_crc,
                reinterpret_cast<const uint8_t *>(audio_buf.data()), pcm_cnt * 2 * sizeof(int16_t));
        }
        auto step_t1 = std::chrono::steady_clock::now();

        if (is_midgame) {
            midgame_step_times_us.push_back(
                std::chrono::duration<double, std::micro>(step_t1 - step_t0).count());
        }
    }

    cumulative_audio_crc ^= ~0u;

    const auto benchmark_end = std::chrono::steady_clock::now();
    const double elapsed_sec = std::chrono::duration<double>(benchmark_end - benchmark_start).count();

    auto mean = [](const std::vector<double> &v) {
        return v.empty() ? 0.0 : std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
    };
    auto max_val = [](const std::vector<double> &v) {
        return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end());
    };

    const double save_mean = mean(save_times_us);
    const double save_p95 = percentile(save_times_us, 0.95);
    const double save_max = max_val(save_times_us);

    const double load_mean = mean(load_times_us);
    const double load_p95 = percentile(load_times_us, 0.95);
    const double load_max = max_val(load_times_us);

    const double step_mean = mean(midgame_step_times_us);
    const double step_p95 = percentile(midgame_step_times_us, 0.95);
    const double step_max = max_val(midgame_step_times_us);
    const double step_fps = step_mean > 0.0 ? (1000000.0 / step_mean) : 0.0;

    const int depth_mean = calc_affordable_depth(step_mean, save_mean, load_mean);
    const int depth_p95 = calc_affordable_depth(step_p95, save_p95, load_p95);
    const int depth_max = calc_affordable_depth(step_max, save_max, load_max);

    const uint32_t final_machine_crc = m.state_crc();
    const uint32_t final_frame_crc = f3rt::crc32(
        reinterpret_cast<const uint8_t *>(m.pixels.data()), m.pixels.size() * 4);

    auto lm_status = inspect_landmaker(m);

    if (!capture_surface.empty()) {
        if (capture_surface.has_parent_path()) {
            std::filesystem::create_directories(capture_surface.parent_path());
        }
        f3rt::write_bmp(capture_surface, m.pixels);
    }
    if (!dump_dir.empty()) {
        f3rt::dump_machine(m, dump_dir);
    }

    std::cout << "\n--- SNAPSHOT PERFORMANCE SUMMARY ---\n"
              << "  Snapshot size: " << state_sz << " bytes\n"
              << "  Save (us): mean=" << std::fixed << std::setprecision(1) << save_mean
              << " p95=" << save_p95 << " max=" << save_max << '\n'
              << "  Load (us): mean=" << load_mean
              << " p95=" << load_p95 << " max=" << load_max << '\n'
              << "  Step (us): mean=" << step_mean
              << " p95=" << step_p95 << " max=" << step_max
              << " (step FPS: " << step_fps << ")\n"
              << "  60Hz affordable rollback depth (16.667ms budget):\n"
              << "    Based on mean: " << depth_mean << " frames\n"
              << "    Based on p95:  " << depth_p95 << " frames\n"
              << "    Based on max:  " << depth_max << " frames\n"
              << "  Allocations during save/load: " << (total_save_allocations + total_load_allocations)
              << " (save=" << total_save_allocations << ", load=" << total_load_allocations << ")\n\n";

    std::cout << "SUCCESS snapshot_proof checks=" << checks_performed
              << " state_size=" << state_sz
              << " save_us_mean=" << save_mean
              << " save_us_p95=" << save_p95
              << " save_us_max=" << save_max
              << " load_us_mean=" << load_mean
              << " load_us_p95=" << load_p95
              << " load_us_max=" << load_max
              << " step_us_mean=" << step_mean
              << " step_us_p95=" << step_p95
              << " step_us_max=" << step_max
              << " step_fps=" << step_fps
              << " affordable_depth_mean=" << depth_mean
              << " affordable_depth_p95=" << depth_p95
              << " affordable_depth_max=" << depth_max
              << " total_save_allocs=" << total_save_allocations
              << " total_load_allocs=" << total_load_allocations
              << " wall_clock_perturbations=" << wall_clock_perturbations
              << " final_crc=0x" << std::hex << final_machine_crc
              << " frame_crc=0x" << final_frame_crc
              << " audio_crc=0x" << cumulative_audio_crc << std::dec
              << " audio_samples=" << total_audio_frames
              << " ram_word_401f6e=" << lm_status.ram_probe
              << " ram_flags=0x" << std::hex << int(lm_status.flags_val) << std::dec
              << " versus_status=" << lm_status.versus_status
              << " vs_active=" << int(lm_status.is_versus)
              << '\n';

    return 0;
}

// -----------------------------------------------------------------------------
// Mode 2: Reference Oracle Mode
// -----------------------------------------------------------------------------
int run_reference(const std::filesystem::path &romdir, const std::string &set,
                  uint64_t seed, uint64_t target_frames, unsigned delay,
                  const std::string &sound_driver,
                  const std::string &schedule_type,
                  const f3rt::GameVideoOptions &video_opts,
                  const std::filesystem::path &capture_surface,
                  const std::filesystem::path &dump_dir,
                  uint64_t dump_every, const std::filesystem::path &initial_state, unsigned match_index,
                  f3rt::Audio::Backend backend) {
    const bool is_vs_schedule = (schedule_type != "single");
    std::cout << "--- REFERENCE ORACLE RUN ---\n"
              << "seed=" << seed << " frames=" << target_frames
              << " delay=" << delay << " audio_backend=" << (backend == f3rt::Audio::Backend::Hle ? "hle" : "accurate")
              << " sound_driver=" << (backend == f3rt::Audio::Backend::Hle ? "none" : sound_driver)
              << " schedule=" << (is_vs_schedule ? "versus" : "single") << '\n';

    f3rt::Machine m(f3rt::RomSet::load(romdir, set));
    configure_machine(m, sound_driver, video_opts, backend);
    if(!initial_state.empty()) {
        m.load_sync_state(read_file_bytes(initial_state));
        if(!f3rt::netplay::versus_handoff_ready(m) || m.audio->available_frames())
            throw std::runtime_error("Reference input is not a drained versus handoff");
    }
    const uint64_t origin=m.frame;

    f3rt::test::GameplaySchedule schedule(seed, {.versus = is_vs_schedule});
    std::array<int16_t, 8192> audio_buf{};
    uint64_t audio_frames = 0;
    int audio_peak = 0;
    uint32_t audio_crc = ~0u;

    const std::vector<uint64_t> sample_frames = {1200, 1800, 2400, 3600, 6000};
    if (!dump_dir.empty()) {
        std::filesystem::create_directories(dump_dir);
    }

    const auto start_time = std::chrono::steady_clock::now();

    while (m.frame-origin < target_frames) {
        const uint64_t f = m.frame-origin;

        // Check if current frame is a requested sample point for VS mode verification
        bool is_sample_point = (std::find(sample_frames.begin(), sample_frames.end(), f) != sample_frames.end());
        if (dump_every > 0 && f > 0 && (f % dump_every == 0)) is_sample_point = true;

        if (is_sample_point) {
            auto st = inspect_landmaker(m);
            std::cout << "  [SAMPLE] frame=" << f
                      << " word_401f6e=" << st.ram_probe
                      << " flags=0x" << std::hex << int(st.flags_val) << std::dec
                      << " p1=" << int(st.p1_active) << " p2=" << int(st.p2_active)
                      << " status=" << st.versus_status << '\n';

            if (!dump_dir.empty()) {
                std::ostringstream ss;
                ss << "frame_" << std::setw(5) << std::setfill('0') << f << ".bmp";
                f3rt::write_bmp(dump_dir / ss.str(), m.pixels);
            }
        }

        // Consistent delay semantics (Item 8):
        // Inputs taking effect at simulation frame f were sampled at frame (f - delay)
        std::array<uint16_t, 2> inputs{0, 0};
        if (f >= delay) {
            inputs = initial_state.empty()?schedule.step(f-delay):match_inputs(seed,match_index,f-delay);
        }

        f3rt::netplay::apply_inputs(m, inputs);

        if (!m.run_frame(true) || m.fallback_instructions > 0) {
            throw std::runtime_error("Reference strict-native halted at frame " + std::to_string(f));
        }

        // Drain audio and compute audio CRC
        size_t count = 0;
        while ((count = m.audio->render(audio_buf.data(), audio_buf.size() / 2)) != 0) {
            audio_frames += count;
            audio_crc = crc32_update(audio_crc,
                reinterpret_cast<const uint8_t *>(audio_buf.data()), count * 2 * sizeof(int16_t));
            for (size_t i = 0; i < count * 2; ++i) {
                audio_peak = std::max(audio_peak, std::abs(int(audio_buf[i])));
            }
        }
    }

    audio_crc ^= ~0u;

    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    const double elapsed_sec = std::chrono::duration<double>(elapsed).count();
    const double fps = elapsed_sec > 0 ? double(m.frame-origin) / elapsed_sec : 0.0;

    const uint32_t final_machine_crc = initial_state.empty()?m.state_crc():m.sync_state_crc();
    const uint32_t final_frame_crc = f3rt::crc32(
        reinterpret_cast<const uint8_t *>(m.pixels.data()), m.pixels.size() * 4);

    auto lm_status = inspect_landmaker(m);

    if (!capture_surface.empty()) {
        if (capture_surface.has_parent_path()) {
            std::filesystem::create_directories(capture_surface.parent_path());
        }
        f3rt::write_bmp(capture_surface, m.pixels);
    }
    if (!dump_dir.empty()) {
        f3rt::dump_machine(m, dump_dir);
    }

    std::cout << "SUCCESS mode=reference seed=" << seed
              << " frames=" << m.frame-origin
              << " final_crc=0x" << std::hex << final_machine_crc
              << " frame_crc=0x" << final_frame_crc
              << " audio_crc=0x" << audio_crc << std::dec
              << " audio_samples=" << audio_frames
              << " audio_peak=" << audio_peak
              << " ram_word_401f6e=" << lm_status.ram_probe
              << " ram_flags=0x" << std::hex << int(lm_status.flags_val) << std::dec
              << " versus_status=" << lm_status.versus_status
              << " vs_active=" << int(lm_status.is_versus)
              << " fps=" << std::fixed << std::setprecision(1) << fps
              << '\n';

    return 0;
}

// -----------------------------------------------------------------------------
// Mode 3: Headless Netplay Client Mode
// -----------------------------------------------------------------------------
int run_client(const std::filesystem::path &romdir, const std::string &set,
               const std::string &server_addr, const std::string &room_name,
               unsigned player_slot, unsigned delay, unsigned window,
               uint64_t seed, uint64_t target_frames, const std::string &sound_driver,
               const std::string &schedule_type, const f3rt::GameVideoOptions &video_opts,
               const std::filesystem::path &capture_surface, const std::filesystem::path &dump_dir,
               double timeout_sec, bool unthrottled, uint64_t stall_at, uint32_t stall_ms,
               uint64_t withhold_at, uint32_t withhold_ms, bool corrupt_build_hash,
               uint64_t event_at, uint64_t prelude_frames, unsigned host_player,
               f3rt::Audio::Backend backend, const std::filesystem::path &wav_path) {
    using namespace f3rt::netplay;
    if(schedule_type!="versus" || target_frames>=UINT32_MAX-Rollback::history_size)
        throw std::runtime_error("Client requires versus schedule and a bounded match frame count");
    if(host_player<1 || host_player>2)throw std::runtime_error("Host player must be 1 or 2");
    f3rt::Machine m(f3rt::RomSet::load(romdir,set));
    configure_machine(m,sound_driver,video_opts,backend);
    std::unique_ptr<f3rt::WavWriter> wav;
    if(!wav_path.empty())wav=std::make_unique<f3rt::WavWriter>(wav_path,m.audio->sample_rate());
    struct CancelObservation {
        struct Voice { uint64_t instance=0,tick=0;uint8_t layer=0; };
        std::array<Voice,32> fading{};
        uint64_t voices=0,stopped=0,max_ticks=0;
    };
    auto cancellation=std::make_shared<CancelObservation>();
    if(backend==f3rt::Audio::Backend::Hle)
        m.audio->set_hle_observer([cancellation](const f3rt::hle::VoiceEvent &event) {
            using Kind=f3rt::hle::VoiceEvent::Kind;
            if(event.kind==Kind::Cancel) {
                ++cancellation->voices;
                for(auto &voice:cancellation->fading)if(!voice.instance) {
                    voice={event.instance,event.tick,event.layer};break;
                }
            } else if(event.kind==Kind::Stop) {
                for(auto &voice:cancellation->fading)
                    if(voice.instance==event.instance && voice.layer==event.layer) {
                        ++cancellation->stopped;
                        cancellation->max_ticks=std::max(cancellation->max_ticks,event.tick-voice.tick);
                        voice={};break;
                    }
            }
        });
    std::array<int16_t,8192> audio{};
    auto discard_audio=[&]() { while(m.audio->render(audio.data(),audio.size()/2)) {} };
    if(prelude_frames==UINT64_MAX)prelude_frames=player_slot==1?2400:3200;
    const auto solo_inputs=[&](uint64_t f) {
        // One start: repeated starts in the selection flow can enter versus.
        std::array<InputWord,2> words{};
        if(f>=1200 && f<1220)words[0]|=0x100;
        if(f>=1280 && f<1285)words[0]|=0x80;
        if(f>=1700 && f%60<3)words[0]|=0x10;
        if(f>=2400)words[0]=match_inputs(seed+player_slot*101,0,f-2400)[0];
        return words;
    };
    for(uint64_t f=0;f<prelude_frames;++f) {
        apply_inputs(m,solo_inputs(f));
        if(!m.run_frame(true) || m.fallback_instructions)throw std::runtime_error("Solo prehistory halted");
        discard_audio();
    }
    if(versus_match_active(m))throw std::runtime_error("Prelude unexpectedly entered versus");
    // Different persistent bytes are intentionally irrelevant after host adoption.
    // Change an EEPROM word after solo boot, not ROM or live game RAM.
    if(player_slot==2)m.eeprom->words.back()^=0x0101;
    const auto pre_crc=m.sync_state_crc();
    const auto pre_eeprom=f3rt::crc32(reinterpret_cast<const uint8_t *>(m.eeprom->words.data()),128);
    std::cout<<"[PREHISTORY] player="<<player_slot<<" frames="<<m.frame<<" crc="<<pre_crc
             <<" eeprom_crc="<<pre_eeprom<<" scale="<<video_opts.scale<<" border="<<video_opts.border
             <<" active="<<unsigned(m.ram[0x1f53]&3)<<'\n'<<std::flush;
    auto identity=machine_identity(m);
    if(corrupt_build_hash)identity.build_hash[0]^=0xff;
    uint64_t total=0,total_audio=0,total_rollbacks=0,frontier_stalls=0,local_returns=0;
    uint64_t event_rollbacks=0,event_stalls=0;
    unsigned max_depth=0,last_depth=0,event_depth=0,natural_ends=0,matches=0;
    uint32_t grand_audio=~0u,final_crc=0,frame_crc=0;
    double event_stall_ms=0,last_rtt=0;
    bool withheld=false,stalled=false;
    const auto started=std::chrono::steady_clock::now();
    while(total<target_frames) {
        TransportOptions options;
        options.server=server_addr;options.room=room_name;
        options.player=player_slot;options.host=player_slot==host_player;options.delay=delay;
        Session session(m,options,identity,window);
        session.set_frame_limit(uint32_t(target_frames-total));
        uint32_t match_audio=~0u;
        uint64_t match_samples=0,reported=total;
        bool saw_start=false,event_stalling=false;
        auto progress=std::chrono::steady_clock::now(),event_begin=progress;
        std::filesystem::path match_dir;
        if(!dump_dir.empty()) {
            match_dir=dump_dir/("match_"+std::to_string(matches));
            std::filesystem::create_directories(match_dir);
        }
        auto drain=[&]() {
            size_t count;
            while((count=session.render_audio(audio.data(),audio.size()/2))) {
                if(!saw_start)continue;
                if(wav)wav->append(std::span(audio.data(),count*2));
                match_samples+=count;total_audio+=count;
                const auto *bytes=reinterpret_cast<const uint8_t *>(audio.data());
                match_audio=crc32_update(match_audio,bytes,count*4);
                grand_audio=crc32_update(grand_audio,bytes,count*4);
            }
        };
        while(session.connected()) {
            const auto *before=session.rollback();
            const auto old_rollbacks=before?before->rollback_count():0;
            const uint64_t global=total+(before?before->frame():0);
            if(before && stall_at && global>=stall_at && !stalled) {
                std::cout<<"[STALL_BEGIN] player="<<player_slot<<" frame="<<global<<'\n'<<std::flush;
                std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
                stalled=true;
                std::cout<<"[STALL_END] player="<<player_slot<<'\n'<<std::flush;
            }
            if(before && withhold_at && global>=withhold_at && !withheld) {
                std::cout<<"[WITHHOLD_BEGIN] player="<<player_slot<<" frame="<<global<<'\n'<<std::flush;
                const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(withhold_ms);
                while(session.connected() && std::chrono::steady_clock::now()<end) {
                    session.pump();drain();std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                withheld=true;
                std::cout<<"[WITHHOLD_END] player="<<player_slot<<'\n'<<std::flush;
            }
            session.pump();
            const auto *rollback=session.rollback();
            if(rollback && !saw_start) {
                saw_start=true;progress=std::chrono::steady_clock::now();
                std::cout<<"[HANDOFF] match="<<matches<<" player="<<player_slot
                         <<" origin="<<rollback->origin_frame()<<" initial_crc="<<m.sync_state_crc()
                         <<" delay="<<session.delay()<<" flags="<<unsigned(m.ram[0x1f53])
                         <<" match_kind="<<m.read16(0x401f6e)<<'\n'<<std::flush;
                if(options.host && !match_dir.empty()) {
                    std::vector<uint8_t> bytes(m.sync_state_size());m.save_sync_state(bytes);
                    std::ofstream out(match_dir/"handoff.bin",std::ios::binary);
                    out.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());
                    if(!out)throw std::runtime_error("Cannot write handoff reference state");
                }
            }
            if(event_at && rollback && rollback->rollback_count()!=old_rollbacks &&
               rollback->frame()>=rollback->last_rollback_depth()) {
                const uint64_t corrected=total+rollback->frame()-rollback->last_rollback_depth();
                if(corrected>=event_at+session.delay() && corrected<event_at+session.delay()+window) {
                    event_rollbacks+=rollback->rollback_count()-old_rollbacks;
                    event_depth=std::max(event_depth,rollback->last_rollback_depth());
                }
            }
            bool advanced=false;
            if(session.connected()) {
                std::array<InputWord,2> local{};
                if(rollback)local[0]=match_inputs(seed,matches,rollback->frame())[session.slot()];
                else local=solo_inputs(m.frame);
                advanced=session.advance(local);
            }
            drain();
            const bool event_full=event_at && rollback && !advanced &&
                rollback->frame()-rollback->confirmed_frame()==window &&
                total+rollback->frame()>=event_at+session.delay() &&
                total+rollback->confirmed_frame()<event_at+session.delay()+window;
            if(event_full) {
                const auto now=std::chrono::steady_clock::now();
                if(!event_stalling)event_begin=now;
                ++event_stalls;
                event_stall_ms=std::max(event_stall_ms,
                    std::chrono::duration<double,std::milli>(now-event_begin).count());
            }
            event_stalling=event_full;
            if(rollback) {
                const auto confirmed=total+rollback->confirmed_frame();
                if(confirmed!=reported) {
                    progress=std::chrono::steady_clock::now();
                    if(confirmed/1000!=reported/1000 || confirmed==target_frames)
                        std::cout<<"[PROGRESS] player="<<player_slot<<" confirmed="<<confirmed<<'\n'<<std::flush;
                    reported=confirmed;
                }
            } else if(advanced)progress=std::chrono::steady_clock::now();
            if(!advanced) {
                if(rollback)++frontier_stalls;
                if(!unthrottled)std::this_thread::sleep_for(std::chrono::microseconds(200));
                else std::this_thread::yield();
            }
            if(std::chrono::duration<double>(std::chrono::steady_clock::now()-progress).count()>timeout_sec)
                session.disconnect("Oracle watchdog: no match progress");
        }
        drain();
        const auto result=session.result();
        if(result==Session::Result::Error || result==Session::Result::Disconnected) {
            const auto reason=session.error();
            const auto local_start=m.frame;
            for(unsigned i=0;i<60;++i) {
                apply_inputs(m,{match_inputs(seed+player_slot,matches,i)[0],0});
                if(!m.run_frame(true) || m.fallback_instructions)throw std::runtime_error("Local recovery halted");
                discard_audio();
            }
            std::cout<<"[LOCAL_RETURN] reason=error advanced="<<m.frame-local_start
                     <<" fallback="<<m.fallback_instructions<<'\n'<<std::flush;
            throw std::runtime_error(reason);
        }
        const auto *rollback=session.rollback();
        if(!rollback || !rollback->confirmed_frame() || rollback->frame()!=rollback->confirmed_frame())
            throw std::runtime_error("Session ended without a confirmed match frontier");
        final_crc=m.sync_state_crc();
        frame_crc=f3rt::crc32(reinterpret_cast<const uint8_t *>(m.pixels.data()),m.pixels.size()*4);
        total+=rollback->confirmed_frame();total_rollbacks+=rollback->rollback_count();
        max_depth=std::max(max_depth,rollback->maximum_rollback_depth());
        last_depth=rollback->last_rollback_depth();last_rtt=session.rtt_ms();
        if(result==Session::Result::MatchEnded)++natural_ends;
        std::cout<<"MATCH match="<<matches<<" frames="<<rollback->confirmed_frame()<<" delay="<<session.delay()
                 <<" host="<<int(options.host)<<" origin="<<rollback->origin_frame()
                 <<" final_crc=0x"<<std::hex<<final_crc<<" frame_crc=0x"<<frame_crc
                 <<" audio_crc=0x"<<(match_audio^~0u)<<std::dec<<" audio_samples="<<match_samples
                 <<" natural_end="<<int(result==Session::Result::MatchEnded)<<'\n'<<std::flush;
        if(!match_dir.empty())f3rt::dump_machine(m,match_dir);
        if(total==target_frames && !capture_surface.empty()) {
            if(capture_surface.has_parent_path())std::filesystem::create_directories(capture_surface.parent_path());
            f3rt::write_bmp(capture_surface,m.pixels);
        }
        const auto local_start=m.frame;
        // No session.advance() or input packets here: prove both machines are local.
        for(unsigned i=0;i<90+player_slot*17;++i) {
            apply_inputs(m,{match_inputs(seed+player_slot,matches,i)[0],0});
            if(!m.run_frame(true) || m.fallback_instructions)throw std::runtime_error("Post-match local play halted");
            discard_audio();
        }
        ++local_returns;
        std::cout<<"[LOCAL_RETURN] match="<<matches<<" advanced="<<m.frame-local_start
                 <<" crc="<<m.sync_state_crc()<<" fallback="<<m.fallback_instructions<<'\n'<<std::flush;
        ++matches;
    }
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    const auto hle=m.audio->hle_stats();
    std::cout<<"SUCCESS mode=client player="<<player_slot<<" seed="<<seed<<" frames="<<total
             <<" final_crc=0x"<<std::hex<<final_crc<<" frame_crc=0x"<<frame_crc
             <<" audio_crc=0x"<<(grand_audio^~0u)<<std::dec<<" audio_samples="<<total_audio
             <<" rollbacks="<<total_rollbacks<<" max_depth="<<max_depth<<" last_depth="<<last_depth
             <<" hle_commands="<<hle.commands<<" hle_reused="<<hle.reused<<" hle_cancelled="<<hle.cancelled
             <<" hle_cancelled_voices="<<cancellation->voices<<" hle_cancel_stops="<<cancellation->stopped
             <<" hle_max_cancel_ticks="<<cancellation->max_ticks
             <<" frontier_stalls="<<frontier_stalls<<" event_delta_rollbacks="<<event_rollbacks
             <<" event_max_depth="<<event_depth<<" event_delta_stalls="<<event_stalls
             <<" event_stall_ms="<<event_stall_ms<<" rtt_ms="<<last_rtt
             <<" matches="<<matches<<" natural_ends="<<natural_ends<<" local_returns="<<local_returns
             <<" pre_crc="<<pre_crc<<" pre_eeprom_crc="<<pre_eeprom
             <<" fallback="<<m.fallback_instructions<<" fps="<<double(total)/seconds<<'\n';
    return 0;
}

} // namespace

int main(int argc, char **argv) try {
    std::filesystem::path romdir;
#ifdef F3RT_DEFAULT_ROM_DIR
    romdir = F3RT_DEFAULT_ROM_DIR;
#endif
    std::string set = "landmakrj";
    std::string mode = "reference";
    std::string server_addr = "127.0.0.1:9000";
    std::string room_name = "oracle_room";
    std::string schedule_type = "versus";
    unsigned player = 0; // 1 or 2
    unsigned delay = 2;
    unsigned window = 16;
    uint64_t seed = 12345;
    uint64_t frames = 20000;
    std::string sound_driver = "native";
    std::string audio_backend = "accurate";
    bool sound_explicit = false;
    std::filesystem::path wav_path;
    f3rt::GameVideoOptions video_opts;
    std::filesystem::path capture_surface;
    std::filesystem::path dump_dir;
    std::filesystem::path initial_state;
    unsigned match_index=0,host_player=1;
    uint64_t prelude_frames=UINT64_MAX;
    uint64_t snapshot_interval = 1000;
    uint64_t snapshot_k = 0;
    uint64_t dump_every = 0;
    double timeout_sec = 120.0;
    bool unthrottled = false;
    uint64_t stall_at = 0;
    uint32_t stall_ms = 0;
    uint64_t withhold_at = 0;
    uint32_t withhold_ms = 0;
    uint64_t event_at = 0;
    bool corrupt_build_hash = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> const char * {
            if (i + 1 >= argc) throw std::runtime_error("Missing value for argument: " + arg);
            return argv[++i];
        };

        if (arg == "--rom-dir") {
            romdir = value();
        } else if (arg == "--set") {
            set = value();
        } else if (arg == "--mode") {
            mode = value();
        } else if (arg == "--server") {
            server_addr = value();
        } else if (arg == "--room") {
            room_name = value();
        } else if (arg == "--player") {
            player = unsigned(std::stoul(value()));
        } else if (arg == "--delay") {
            delay = unsigned(std::stoul(value()));
        } else if (arg == "--window") {
            window = unsigned(std::stoul(value()));
        } else if (arg == "--seed") {
            seed = std::stoull(value());
        } else if (arg == "--frames") {
            frames = std::stoull(value());
        } else if (arg == "--sound-driver") {
            sound_explicit = true;
            sound_driver = value();
        } else if (arg == "--audio-backend") {
            audio_backend = value();
        } else if (arg == "--wav") {
            wav_path = value();
        } else if (arg == "--schedule") {
            schedule_type = value();
        } else if (arg == "--video-scale") {
            video_opts.scale = unsigned(std::stoul(value()));
        } else if (arg == "--video-border") {
            video_opts.border = unsigned(std::stoul(value()));
        } else if (arg == "--surface" || arg == "--capture-surface") {
            capture_surface = value();
        } else if (arg == "--dump-dir") {
            dump_dir = value();
        } else if (arg == "--initial-state") {
            initial_state=value();
        } else if (arg == "--match-index") {
            match_index=unsigned(std::stoul(value()));
        } else if (arg == "--host-player") {
            host_player=unsigned(std::stoul(value()));
        } else if (arg == "--prelude-frames") {
            prelude_frames=std::stoull(value());
        } else if (arg == "--dump-every") {
            dump_every = std::stoull(value());
        } else if (arg == "--snapshot-interval") {
            snapshot_interval = std::stoull(value());
        } else if (arg == "--snapshot-k") {
            snapshot_k = std::stoull(value());
        } else if (arg == "--timeout") {
            timeout_sec = std::stod(value());
        } else if (arg == "--unthrottled") {
            unthrottled = true;
        } else if (arg == "--stall-at") {
            stall_at = std::stoull(value());
        } else if (arg == "--stall-ms") {
            stall_ms = unsigned(std::stoul(value()));
        } else if (arg == "--withhold-input-at") {
            withhold_at = std::stoull(value());
        } else if (arg == "--withhold-input-ms") {
            withhold_ms = unsigned(std::stoul(value()));
        } else if (arg == "--observe-event-at") {
            event_at = std::stoull(value());
        } else if (arg == "--corrupt-build-hash") {
            corrupt_build_hash = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n\n"
                      << "F3 Netplay Oracle & Snapshot Proof Harness.\n\n"
                      << "Modes:\n"
                      << "  --mode snapshot   Snapshot save/load proof and performance benchmark\n"
                      << "  --mode reference  Single-machine reference execution with delay-adjusted stream\n"
                      << "  --mode client     Divergent solo histories, real snapshot handoff, versus campaigns\n"
                      << "  --mode sync-proof Cross-presentation canonical import and exact local replay proof\n\n"
                      << "Options:\n"
                      << "  --rom-dir DIR          Path to ROM directory\n"
                      << "  --set SET              ROM set name (default: landmakrj)\n"
                      << "  --seed N               PRNG seed (default: 12345)\n"
                      << "  --frames N             Simulation frame count (default: 20000)\n"
                      << "  --player 1|2           Player slot for client mode (1=P1, 2=P2)\n"
                      << "  --server ADDR          Relay server address (default: 127.0.0.1:9000)\n"
                      << "  --room NAME            Relay room name (default: oracle_room)\n"
                      << "  --delay N              Input delay frames (default: 2)\n"
                      << "  --window N             Rollback prediction window (default: 16)\n"
                      << "  --sound-driver MODE    native (default), oracle, or all (snapshot mode)\n"
                      << "  --audio-backend MODE   accurate (default) or hle (reference/client modes)\n"
                      << "  --wav FILE             Record client output PCM (speculative when HLE)\n"
                      << "  --schedule MODE        versus (default) or single\n"
                      << "  --surface FILE.bmp     Capture final frame BMP\n"
                      << "  --dump-dir DIR         Dump machine state / sample BMPs\n"
                      << "  --dump-every N         Dump frame BMP every N frames\n"
                      << "  --initial-state FILE   Raw canonical handoff state for independent reference replay\n"
                      << "  --match-index N        Campaign match input stream index (default 0)\n"
                      << "  --host-player 1|2      Explicit authority, independent of slot (default 1)\n"
                      << "  --prelude-frames N     Independent solo history before pairing (default 2400/3200)\n"
                      << "  --snapshot-interval N  Interval for snapshot proof (default: 1000)\n"
                      << "  --snapshot-k N         Resimulation depth K (default: 1, 7, 16, 31, 97)\n"
                      << "  --timeout SEC          Timeout seconds (default: 120)\n"
                      << "  --unthrottled          Run without yield sleep in stalls\n"
                      << "  --stall-at FRAME       Inject complete stall at specified frame\n"
                      << "  --stall-ms MS          Duration of stall in milliseconds\n"
                      << "  --withhold-input-at F  Withhold submitting local inputs starting at frame\n"
                      << "  --withhold-input-ms MS Duration to withhold local inputs in milliseconds\n"
                      << "  --observe-event-at F   Measure corrections and full-window stalls near F\n"
                      << "  --corrupt-build-hash   Mutate build hash to test handshake rejection\n";
            return 0;
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (romdir.empty()) {
        throw std::runtime_error("ROM directory must be specified via --rom-dir or compiled F3RT_DEFAULT_ROM_DIR");
    }
    if (set != "landmakrj") {
        throw std::runtime_error("netplay_oracle requires landmakrj ROM set");
    }
    if (frames == 0) {
        throw std::runtime_error("--frames must be positive");
    }
    if (audio_backend != "accurate" && audio_backend != "hle")
        throw std::runtime_error("--audio-backend must be accurate or hle");
    if (audio_backend == "hle" && sound_explicit)
        throw std::runtime_error("HLE does not execute --sound-driver");
    const auto backend = audio_backend == "hle" ? f3rt::Audio::Backend::Hle : f3rt::Audio::Backend::Accurate;
    if (backend == f3rt::Audio::Backend::Hle &&
        (mode == "sync-proof" || mode == "snapshot" || mode == "snapshot-proof"))
        throw std::runtime_error("HLE playback is not restored: use client/reference modes, not accurate-PCM snapshot proofs");

    if(mode=="sync-proof") {
        const auto allocation_probe=+[](bool begin)->uint64_t {
            if(begin)g_allocation_count=0;
            g_track_allocations=begin;
            return g_allocation_count.load();
        };
        if(sound_driver=="all") {
            for(const char *driver:{"native","oracle"}) {
                const int result=run_sync_snapshot_proof(romdir,set,driver,seed,frames,allocation_probe);
                if(result)return result;
            }
            return 0;
        }
        return run_sync_snapshot_proof(romdir,set,sound_driver,seed,frames,allocation_probe);
    }
    if (mode == "snapshot" || mode == "snapshot-proof") {
        if (snapshot_interval == 0) {
            throw std::runtime_error("--snapshot-interval must be positive");
        }
        // Test multiple N points (including cold 0 and mid-game)
        std::vector<uint64_t> test_frames = {0};
        for (uint64_t n = snapshot_interval; n < frames; n += snapshot_interval) {
            test_frames.push_back(n);
        }
        // Varied K depths: default suite runs (1, 7, 16, 31, 97) per Item 1
        std::vector<uint64_t> k_depths;
        if (snapshot_k > 0) {
            k_depths = {snapshot_k};
        } else {
            k_depths = {1, 7, 16, 31, 97};
        }

        if (sound_driver == "all") {
            for (const char *driver : {"native", "oracle"}) {
                const int result = run_snapshot_proof(romdir, set, seed, frames, test_frames,
                    k_depths, driver, schedule_type, video_opts, capture_surface, dump_dir);
                if (result) return result;
            }
            return 0;
        }
        return run_snapshot_proof(romdir, set, seed, frames, test_frames,
                                  k_depths, sound_driver, schedule_type,
                                  video_opts, capture_surface, dump_dir);
    } else if (mode == "reference") {
        return run_reference(romdir, set, seed, frames, delay, sound_driver,
                             schedule_type, video_opts, capture_surface,
                             dump_dir, dump_every, initial_state, match_index, backend);
    } else if (mode == "client") {
        if (player != 1 && player != 2) {
            throw std::runtime_error("Client mode requires --player 1 or --player 2");
        }
        return run_client(romdir, set, server_addr, room_name, player, delay,
                          window, seed, frames, sound_driver, schedule_type,
                          video_opts, capture_surface, dump_dir, timeout_sec,
                          unthrottled, stall_at, stall_ms, withhold_at, withhold_ms,
                          corrupt_build_hash, event_at, prelude_frames, host_player, backend, wav_path);
    } else {
        throw std::runtime_error("Unknown --mode: " + mode + " (expected snapshot, reference, or client)");
    }
} catch (const std::exception &e) {
    std::cerr << "ORACLE ERROR: " << e.what() << '\n';
    return 1;
}
