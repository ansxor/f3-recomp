#include "sync_snapshot_proof.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/game_video.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/netplay.hpp"
#include "eeprom.hpp"
#include "gameplay_inputs.hpp"
#ifdef F3RT_GENERATED
#include "program.h"
#endif
#ifdef F3RT_SOUND_GENERATED
#include "sound_program.h"
#endif

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using f3rt::Machine;
using Bytes = std::vector<uint8_t>;
constexpr uint64_t replay_frames = 31;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error("sync-proof: " + message);
}

void configure(Machine &m, const std::string &driver,
               f3rt::GameVideoMode mode, f3rt::GameVideoOptions options) {
    m.allow_main_fallback = false;
#ifdef F3RT_GENERATED
    require(f3_generated_register(&m.cpu), "main program registration failed");
#else
    throw std::runtime_error("sync-proof requires F3RT_GENERATED");
#endif
    if (driver == "native") {
#ifdef F3RT_SOUND_GENERATED
        m.use_native_sound(f3_sound_blocks, f3_sound_block_count,
                           {f3_sound_excluded_ranges, f3_sound_excluded_count}, f3_sound_rom_crc32);
#else
        throw std::runtime_error("sync-proof requires generated native sound");
#endif
    } else {
        require(driver == "oracle", "unknown sound driver " + driver);
    }
    m.game_video = std::make_unique<f3rt::GameVideo>(m, mode, options);
}

std::vector<int16_t> drain(Machine &m) {
    std::array<int16_t, 4096> samples{};
    std::vector<int16_t> result;
    size_t count;
    while ((count = m.audio->render(samples.data(), samples.size() / 2)))
        result.insert(result.end(), samples.begin(), samples.begin() + count * 2);
    return result;
}

void step(Machine &m, const std::array<uint16_t, 2> &input) {
    f3rt::netplay::apply_inputs(m, input);
    require(m.run_frame(true) && m.fallback_instructions == 0,
            "native execution halted/fell back at frame " + std::to_string(m.frame));
}

template<class F>
uint64_t measured(uint64_t (*probe)(bool), F operation) {
    probe(true);
    try {
        operation();
    } catch (...) {
        probe(false);
        throw;
    }
    return probe(false);
}

void equal_sync(Machine &host, Machine &guest, Bytes &a, Bytes &b,
                const std::string &where) {
    host.save_sync_state(a);
    guest.save_sync_state(b);
    require(a == b, where + " canonical bytes differ");
    require(host.sync_state_crc() == guest.sync_state_crc(), where + " canonical CRC differs");
    require(host.native_pixels() == guest.native_pixels(), where + " native pixels differ");
}


struct ReplayFrame {
    std::vector<int16_t> pcm;
    std::vector<uint32_t> pixels;
    std::vector<uint32_t> presentation;
    std::vector<uint32_t> gpu_reference;
    Bytes local_checkpoint;
    uint32_t local_crc = 0;
};
} // namespace

int run_sync_snapshot_proof(const std::filesystem::path &romdir,
                            const std::string &set, const std::string &driver,
                            uint64_t seed, uint64_t frames,
                            uint64_t (*allocation_probe)(bool)) {
    require(allocation_probe != nullptr, "allocation probe required");
    require(frames >= 2400 && frames <= UINT64_MAX - replay_frames,
            "frames must be >=2400 and leave replay headroom");
    std::vector<uint64_t> boundaries{0, 1, 700, 800, 1200, 2400, frames};
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    uint64_t checks = 0, paired = 0, replayed = 0, pcm_samples = 0;
    uint64_t save_allocations = 0, load_allocations = 0;
    bool active_seen = false;

    // Compare is only used with the supported Land Maker game renderer. It checks
    // native pixels against the diagnostic renderer itself on every run_frame.
    for (auto mode : {f3rt::GameVideoMode::Game, f3rt::GameVideoMode::Compare}) {
        const char *mode_name = mode == f3rt::GameVideoMode::Game ? "game" : "compare";
        Machine host(f3rt::RomSet::load(romdir, set));
        Machine guest(f3rt::RomSet::load(romdir, set));
        configure(host, driver, f3rt::GameVideoMode::Game, {});
        configure(guest, driver, mode, {.scale = 2, .border = 48});
        f3rt::test::GameplaySchedule schedule(seed);
        require(host.sync_state_size() == guest.sync_state_size(),
                "cross-geometry canonical sizes differ");
        Bytes host_sync(host.sync_state_size()), guest_sync(guest.sync_state_size());
        Bytes host_local(host.state_size()), guest_local(guest.state_size());
        // Warm each CRC scratch and lazy serialization scratch before measuring.
        host.state_crc(); guest.state_crc();
        host.sync_state_crc(); guest.sync_state_crc();
        host.save_state(host_local); guest.save_state(guest_local);
        host.load_state(host_local); guest.load_state(guest_local);
        host.save_sync_state(host_sync); guest.save_sync_state(guest_sync);

        for (size_t boundary = 0; boundary < boundaries.size(); ++boundary) {
            const uint64_t n = boundaries[boundary];
            while (host.frame < n) {
                step(host, schedule.step(host.frame));
                drain(host);
                // This is an observed active-player flag, not a game-mode enum.
                active_seen |= (host.read8(0x00401f53) & 3) != 0;
            }
            const auto resume_schedule = schedule;
            const std::string where = std::string(mode_name) + " N=" + std::to_string(n);
            // The guest really executes a different solo history before EVERY
            // handoff, including N=0. EEPROM is changed after boot so it cannot
            // be normalized by the game's EEPROM initialization routine.
            f3rt::test::GameplaySchedule solo(seed ^ (0xd1b54a32d192ed03ULL + boundary),
                                               {.versus = false});
            const uint64_t prehistory = 53 + boundary * 11;
            for (uint64_t f = 0; f < prehistory; ++f) {
                step(guest, solo.step(f));
                drain(guest);
            }
            guest.eeprom->words.back() = host.eeprom->words.back() ^ uint16_t(0x0101 + boundary);
            require(guest.frame != host.frame || guest.cpu.cycles != host.cpu.cycles,
                    where + " prehistory did not diverge");
            require(guest.eeprom->words != host.eeprom->words, where + " EEPROM did not diverge");
            guest.save_sync_state(guest_sync);
            host.save_sync_state(host_sync);
            require(guest_sync != host_sync, where + " pre-import canonical state did not diverge");

            // Alternate CPU presentation and captured GPU-scene presentation;
            // change GPU scale independently of the fixed local snapshot geometry.
            const bool gpu = (boundary % 2) != 0;
            guest.game_video->enable_gpu_presentation(gpu);
            if (gpu) guest.game_video->set_gpu_scale(3);
            host.save_state(host_local);
            const auto save_count = measured(allocation_probe, [&] { host.save_sync_state(host_sync); });
            const auto load_count = measured(allocation_probe, [&] { guest.load_sync_state(host_sync); });
            save_allocations += save_count;
            load_allocations += load_count;
            require(save_count == 0 && load_count == 0, where + " sync save/load allocated");
            equal_sync(host, guest, host_sync, guest_sync, where + " immediate");
            ++checks;
            if (gpu) {
                guest.game_video->set_gpu_scale(2);
                equal_sync(host, guest, host_sync, guest_sync, where + " GPU scale change");
                ++checks;
            }
            guest.save_state(guest_local);
            const auto base_pixels = guest.native_pixels();
            const auto base_view = guest.game_video->presentation();
            const std::vector<uint32_t> base_presentation(base_view.begin(), base_view.end());
            const auto base_crc = guest.state_crc();
            std::vector<ReplayFrame> reference;
            reference.reserve(replay_frames);
            auto future = resume_schedule;
            for (uint64_t k = 0; k < replay_frames; ++k) {
                const auto input = future.step(n + k);
                step(host, input); step(guest, input);
                auto host_pcm = drain(host);
                auto guest_pcm = drain(guest);
                require(host_pcm == guest_pcm, where + " PCM differs K=" + std::to_string(k + 1));
                pcm_samples += host_pcm.size();
                equal_sync(host, guest, host_sync, guest_sync, where + " K=" + std::to_string(k + 1));
                ReplayFrame record;
                record.pcm = std::move(guest_pcm);
                record.pixels = guest.native_pixels();
                const auto view = guest.game_video->presentation();
                record.presentation.assign(view.begin(), view.end());
                if (k == 0 || k == 6 || k == replay_frames - 1) {
                    record.local_checkpoint.resize(guest.state_size());
                    guest.save_state(record.local_checkpoint);
                    record.local_crc = guest.state_crc();
                }
                if (gpu && k == 0) {
                    guest.game_video->set_gpu_scale(3);
                    const f3rt::GameVideoOptions capture_options{.scale = 3, .border = 48};
                    record.gpu_reference.resize(size_t(capture_options.width()) * capture_options.height());
                    guest.game_video->render_reference(record.gpu_reference, capture_options, 511, true);
                    guest.game_video->set_gpu_scale(2);
                    equal_sync(host, guest, host_sync, guest_sync, where + " rendered scale3 capture");
                    ++checks;
                }
                reference.push_back(std::move(record));
                ++checks; ++paired;
            }
            const auto local_load_count = measured(allocation_probe, [&] { guest.load_state(guest_local); });
            load_allocations += local_load_count;
            require(local_load_count == 0, where + " exact local load allocated");
            Bytes local_check(guest.state_size());
            guest.save_state(local_check);
            require(local_check == guest_local && guest.state_crc() == base_crc,
                    where + " exact local immediate bytes/CRC differ");
            const auto restored_view = guest.game_video->presentation();
            require(guest.native_pixels() == base_pixels &&
                    std::equal(restored_view.begin(), restored_view.end(), base_presentation.begin(), base_presentation.end()),
                    where + " exact local retained presentation differs");
            ++checks;
            future = resume_schedule;
            for (uint64_t k = 0; k < replay_frames; ++k) {
                step(guest, future.step(n + k));
                const auto &record = reference[k];
                require(drain(guest) == record.pcm, where + " local replay PCM differs K=" + std::to_string(k + 1));
                const auto view = guest.game_video->presentation();
                require(guest.native_pixels() == record.pixels &&
                        std::equal(view.begin(), view.end(), record.presentation.begin(), record.presentation.end()),
                        where + " local replay pixels/presentation differ K=" + std::to_string(k + 1));
                if (!record.gpu_reference.empty()) {
                    guest.game_video->set_gpu_scale(3);
                    std::vector<uint32_t> capture(record.gpu_reference.size());
                    guest.game_video->render_reference(capture, {.scale = 3, .border = 48}, 511, true);
                    guest.game_video->set_gpu_scale(2);
                    require(capture == record.gpu_reference, where + " replay GPU-scale reference pixels differ");
                    ++checks;
                }
                if (!record.local_checkpoint.empty()) {
                    guest.save_state(local_check);
                    require(local_check == record.local_checkpoint && guest.state_crc() == record.local_crc,
                            where + " exact local replay bytes/CRC differ K=" + std::to_string(k + 1));
                }
                ++checks; ++replayed;
            }
            host.load_state(host_local);
            schedule = resume_schedule;
            std::cout << "SYNC_PROOF boundary=" << n << " guest_mode=" << mode_name
                      << " host_scale=1 host_border=0 guest_scale=2 guest_border=48"
                      << " gpu_capture=" << gpu << " prehistory_frames=" << prehistory
                      << " K=1,7,31 paired_frames=" << replay_frames
                      << " exact_local_replay_frames=" << replay_frames
                      << " sync_bytes=" << host_sync.size()
                      << " local_bytes=" << guest_local.size()
                      << " save_allocations=" << save_count << " load_allocations=" << load_count
                      << " local_load_allocations=" << local_load_count << '\n';
        }
    }
    require(active_seen, "schedule never observed an active player; active-gameplay coverage missing");
    std::cout << "SUCCESS mode=sync-proof seed=" << seed << " sound_driver=" << driver
              << " frames=" << frames << " boundaries=" << boundaries.size()
              << " checks=" << checks << " paired_frames=" << paired
              << " exact_local_replay_frames=" << replayed << " compared_pcm_samples=" << pcm_samples
              << " save_allocations=" << save_allocations << " load_allocations=" << load_allocations
              << " active_player_observed=1\n";
    return 0;
}
