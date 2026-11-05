#include "session.hpp"
#include "audio_ndsp.hpp"
#include "audio_pacer.hpp"
#ifdef F3_N3DS_BENCH
#include "bench.hpp"
#endif
#include "fast_boot.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/game_boot.hpp"
#include "f3rt/input.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/rom.hpp"
#include "f3rt/video.hpp"
#include "log.hpp"
#include "program.h"
#include "renderer/game/captured_frame.hpp"
#include "sprites/units.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <system_error>

#include <3ds.h>

namespace f3rt::n3ds {
namespace {
constexpr auto MAX_CATCHUP = std::chrono::milliseconds(50);

constexpr auto SLEEP_SLICE = std::chrono::milliseconds(4);
constexpr double audio_target_frames(double rate) {
    return 2.0 * (rate * double(f3rt::Machine::FRAME_PIXELS) / double(f3rt::Machine::PIXEL_CLOCK));
}
} // namespace

Session::Session(Shared &shared, const Settings &settings, const std::string &set,
                 const std::string &data_dir, NdspAudioOutput *audio, PerfStats &perf)
    : m_shared(shared), m_settings(settings), m_set(set), m_data_dir(data_dir),
      m_video{1, settings.border}, m_audio(audio), m_perf(perf), m_volume(settings.volume) {}
Session::~Session() { stop(); }
void Session::start() {
    m_thread = f3rt::Thread(f3rt::ThreadRole::Emulation, 0, [this] { run(); });
}
void Session::stop() {
    m_stop.store(true);
    m_thread.join();
}
void Session::post(Command command, unsigned value) {
    std::lock_guard lock(m_command_mutex);
    m_commands.emplace_back(command, value);
}
std::string Session::error() const {
    std::lock_guard lock(m_error_mutex);
    return m_error;
}
void Session::fail(const std::string &message) {
    {
        std::lock_guard lock(m_error_mutex);
        m_error = message;
    }
    log_line("n3ds: session error: %s", message.c_str());
}
void Session::apply(Command command, unsigned value) {
    switch (command) {
    case Command::Pause:
    case Command::SystemPause:
        m_paused.store(true);
        break;
    case Command::Resume:
    case Command::SystemResume:
        m_paused.store(false);
        break;
    case Command::SetVolume:
        m_volume.store(std::min(value, 100u));
        if (m_audio) m_audio->set_volume(float(std::min(value, 100u)) / 100.0f);
        break;
    case Command::Stop:
        m_stop.store(true);
        break;
    }
}
void Session::push_audio(const int16_t *stereo, size_t frames) {
    if (!m_audio || !frames) return;
    const size_t queued = m_audio->queued_frames();
    if (m_audio_pushes++ && queued == 0) ++m_audio_window_underruns;
    const auto action = m_pacer->before_push(queued, frames);
    if (action.drop_backlog) m_audio->clear();
    if (action.lead_silence_frames) {
        m_silence.assign(action.lead_silence_frames * 2, 0);
        m_audio->push(m_silence.data(), action.lead_silence_frames);
    }
    m_audio->push(stereo, frames);
    m_audio_ratio = m_pacer->after_push(m_audio->queued_frames());
    m_audio->set_rate(m_audio_ratio);
    m_perf.drops.store(m_pacer->stats().drops);
    const size_t depth = m_audio->queued_frames();
    m_perf.audio_queue_frames.store(uint32_t(depth));
    m_audio_window_pushed += frames;
    for (size_t i = 0; i < frames * 2; ++i) {
        const int32_t sample = stereo[i];
        m_audio_window_peak = std::max(m_audio_window_peak, uint32_t(std::abs(sample)));
        m_audio_window_energy += uint64_t(sample * sample);
    }
    m_audio_window_samples_pcm += frames * 2;
    m_audio_window_frames += depth;
    ++m_audio_window_samples;
    m_audio_min = std::min(m_audio_min, depth);
    m_audio_max = std::max(m_audio_max, depth);
}
void Session::log_audio_window(f3rt::Machine &machine) {
    const auto now = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration<double>(now - m_audio_window_start).count();
    if (seconds <= 0) return;
    const double rate = m_source_rate > 0 ? m_source_rate : 1.0;
    const auto to_ms = [rate](double frames) { return frames * 1000.0 / rate; };
    const uint64_t emulated = m_perf.emulated.load(std::memory_order_relaxed);
    const double mean = m_audio_window_samples
                            ? double(m_audio_window_frames) / double(m_audio_window_samples)
                            : 0.0;
    const uint64_t drops = m_pacer ? m_pacer->stats().drops - m_audio_window_drops_base : 0;
    const auto hle = machine.audio->performance_stats();
    const uint64_t commands = hle.commands - m_hle_commands_base;
    m_hle_commands_base = hle.commands;
    const double synth_ms = emulated > m_audio_window_emulated
                                ? double(hle.render_ns - m_hle_render_ns_base) / 1e6 /
                                      double(emulated - m_audio_window_emulated)
                                : 0.0;
    m_hle_render_ns_base = hle.render_ns;
    log_line("n3ds: audio emu_fps=%.1f emu_ms=%.2f push_fps=%.0f queue_ms=min/mean/max=%.2f/"
             "%.2f/%.2f audio_ms=%.2f drops=%llu underruns=%llu late=%llu ratio=%.4f "
             "ready=%llu/%llu/%llu synth_ms=%.2f hle_cmd=%llu voices=%llu/%u peak=%u rms=%.0f",
             double(emulated - m_audio_window_emulated) / seconds,
             double(m_perf.emu_us.load(std::memory_order_relaxed)) / 1000.0,
             double(m_audio_window_pushed) / seconds,
             to_ms(m_audio_window_samples ? double(m_audio_min) : 0.0), to_ms(mean),
             to_ms(double(m_audio_max)),
             to_ms(double(m_audio->queued_frames())), static_cast<unsigned long long>(drops),
             static_cast<unsigned long long>(m_audio_window_underruns),
             static_cast<unsigned long long>(m_audio_window_late), m_audio_ratio,
             static_cast<unsigned long long>(m_audio_window_empty),
             static_cast<unsigned long long>(m_audio_window_single),
             static_cast<unsigned long long>(m_audio_window_burst), synth_ms,
             static_cast<unsigned long long>(commands),
             static_cast<unsigned long long>(hle.voices_started), hle.voices_active,
             m_audio_window_peak,
             m_audio_window_samples_pcm
                 ? std::sqrt(double(m_audio_window_energy) / double(m_audio_window_samples_pcm))
                 : 0.0);
    m_audio_window_start = now;
    m_audio_window_emulated = emulated;
    m_audio_window_drops_base = m_pacer ? m_pacer->stats().drops : 0;
    m_audio_window_frames = m_audio_window_samples = 0;
    m_audio_window_pushed = 0;
    m_audio_min = SIZE_MAX;
    m_audio_max = 0;
    m_audio_window_underruns = 0;
    m_audio_window_late = 0;
    m_audio_window_empty = m_audio_window_single = m_audio_window_burst = 0;
    m_audio_window_peak = 0;
    m_audio_window_energy = 0;
    m_audio_window_samples_pcm = 0;
}
void Session::stage(const char *step) {
    m_stage = step;
    log_line("n3ds: setup %s", step);
}
void Session::startup() {
    {
        stage("roms");
        auto roms = f3rt::RomSet::load("romfs:/" + m_set, m_set);
        stage("machine");
        auto machine = std::make_unique<f3rt::Machine>(std::move(roms));
        stage("block registration");
        if (!f3_generated_register(&machine->cpu))
            throw std::runtime_error("Generated block registration failed");
        machine->allow_main_fallback = false;
        stage("game video");
        machine->game_video =
            std::make_unique<f3rt::GameVideo>(*machine, f3rt::GameVideoMode::Game, m_video);
        machine->game_video->enable_captured_presentation();
        stage("sprite units");
        machine->sprite_units = f3rt::make_game_sprite_units(*machine, false);
        m_playfield = f3rt::TileRowTable(machine->video->playfield_tiles());
        m_sprite = machine->video->sprite_tiles();
        m_machine = std::move(machine);
    }
    stage("capture slots");
    std::array<f3rt::CapturedFrame *, f3rt::FrameSlots::SLOT_COUNT> captures{};
    for (size_t slot = 0; slot < m_captures.size(); ++slot)
        captures[slot] = (m_captures[slot] = std::make_unique<f3rt::CapturedFrame>()).get();
    m_shared.emulation.attach_captures(captures);
    auto &machine = *m_machine;
    const f3rt::GameBoot *boot = f3rt::game_boot();
    if (boot && m_settings.fast_boot) {
        stage("fast boot");
        f3rt::FastBootRequest request;
        request.eeprom_init = m_data_dir + "/eeprom/" + m_set + "-init.bin";
        request.snapshot = m_video;
        request.translated = true;
        request.cancelled = [this] { return m_stop.load(); };
        const auto result = f3rt::run_fast_boot(machine, *boot, request);
        log_line("fast_boot frames=%llu source=%s eeprom=%s",
                 static_cast<unsigned long long>(result.frames), result.source, result.eeprom);
        if (m_audio) m_audio->clear();
    }
#ifdef F3_N3DS_BENCH
    stage("bench script");
    m_bench = Bench::load(m_data_dir + "/bench.txt");
    if (!m_bench) throw std::runtime_error("bench.txt is missing or malformed in " + m_data_dir);
    log_line("n3ds: bench starts at frame=%llu fast_boot=%s",
             static_cast<unsigned long long>(machine.frame + 1),
             m_settings.fast_boot ? "on" : "off");
#endif
    stage("audio pacer");
    const double source_rate = machine.audio->sample_rate();
    m_source_rate = source_rate;
    if (m_audio) m_audio->set_source_rate(source_rate);
    m_pacer = std::make_unique<f3rt::AudioPacer>(f3rt::AudioPacer::Config{
        .rate = uint32_t(source_rate),
        .target_frames = size_t(audio_target_frames(source_rate)),
        .max_queue_ms = 50,
        .throttle = true});
    log_line("n3ds: audio source rate=%.0f Hz target_frames=%zu", source_rate,
             size_t(audio_target_frames(source_rate)));
    m_audio_window_start = std::chrono::steady_clock::now();
    m_audio_window_emulated = m_perf.emulated.load(std::memory_order_relaxed);
    m_audio_window_drops_base = 0;
}
void Session::run() {
    try {
        startup();
        running.store(true);
        stage("frame loop");
        emulate();
    } catch (const std::system_error &error) {
        char detail[192];
        std::snprintf(detail, sizeof(detail), "%s: %s (code=0x%08x, %s)", m_stage, error.what(),
                      unsigned(error.code().value()), error.code().category().name());
        fail(detail);
    } catch (const std::exception &error) {
        fail(std::string(m_stage) + ": " + error.what());
    }
    running.store(false);
}
bool Session::profile_frame() {
    if (!m_perf.cpu_counters.load(std::memory_order_relaxed)) return false;
    if (!m_counters_open) m_counters_open = m_counters.open("emulation");
    return m_counters.available();
}
void Session::emulate() {
    auto &machine = *m_machine;
    const auto frame_time = std::chrono::nanoseconds(
        uint64_t(1e9 * f3rt::Machine::FRAME_PIXELS / f3rt::Machine::PIXEL_CLOCK));
    std::array<int16_t, 8192> samples{};
    auto next_frame = std::chrono::steady_clock::now();
    bool was_paused = m_paused.load();
    while (!m_stop.load()) {
        std::vector<std::pair<Command, unsigned>> commands;
        {
            std::lock_guard lock(m_command_mutex);
            commands.swap(m_commands);
        }
        for (const auto &[command, value] : commands) apply(command, value);
        if (m_stop.load()) break;
        const bool paused = m_paused.load();
        const auto now = std::chrono::steady_clock::now();
        if (was_paused && !paused) {
            ++m_epoch;
            next_frame = now;
        }
        was_paused = paused;
        if (paused || now < next_frame) {
            const auto until = paused ? now + SLEEP_SLICE : next_frame;
            const auto delta = until - now;
            if (delta > std::chrono::nanoseconds::zero()) svcSleepThread(s64(delta.count()));
            continue;
        }
        f3rt::FrameSnapshot *slot = &m_shared.emulation.begin();
        if (slot->captured) machine.game_video->set_capture_target(slot->captured);
        slot->frame = machine.frame;
        slot->epoch = m_epoch;
        slot->scale = m_video.scale;
        slot->start = next_frame;
        const auto frame_start = std::chrono::steady_clock::now();
        std::array<f3rt::LocalInputWord, f3rt::LOCAL_PLAYER_COUNT> local{
            f3rt::LocalInputWord(input.load()), 0, 0, 0};
#ifdef F3_N3DS_BENCH
        const auto scripted = m_bench->begin_frame(machine);
        for (size_t player = 0; player < local.size(); ++player)
            local[player] = f3rt::LocalInputWord(local[player] | scripted[player]);
#endif
        f3rt::apply_local_inputs(machine, local);
        const bool profile = profile_frame();
        machine.game_video->set_probe(profile ? &m_stages : nullptr);
        if (profile) m_stages.begin_frame();
        if (!machine.run_frame(true))
            throw std::runtime_error("CPU halted at " + std::to_string(machine.cpu.pc));
        if (profile) {
            m_stages.end_frame();
            for (size_t stage = 0; stage < m_stages.stages().size(); ++stage)
                m_perf.emulation_cpu[stage].store(m_stages.stages()[stage]);
        }
        m_shared.emulation.publish();
        const auto frame_end = std::chrono::steady_clock::now();
        const uint64_t elapsed_us = uint64_t(
            std::chrono::duration_cast<std::chrono::microseconds>(frame_end - frame_start).count());
        PerfStats::smooth(m_perf.emu_us, elapsed_us);
        const uint64_t synth_ns = machine.audio->performance_stats().render_ns;
        m_perf.emu_start_ns.store(uint64_t(frame_start.time_since_epoch().count()),
                                  std::memory_order_relaxed);
        m_perf.emu_end_ns.store(uint64_t(frame_end.time_since_epoch().count()),
                                std::memory_order_relaxed);
        m_perf.emu_frame_us.store(elapsed_us, std::memory_order_relaxed);
        m_perf.synth_frame_us.store((synth_ns - m_synth_render_ns) / 1000, std::memory_order_relaxed);
        m_synth_render_ns = synth_ns;
        m_perf.emulated.fetch_add(1, std::memory_order_relaxed);
        size_t count, ready = 0;
        while ((count = machine.audio->render_ready(samples.data(), samples.size() / 2)) != 0) {
            push_audio(samples.data(), count);
            ready += count;
        }
        ++(ready == 0 ? m_audio_window_empty : ready < 756 ? m_audio_window_single : m_audio_window_burst);
        if (m_audio && std::chrono::steady_clock::now() - m_audio_window_start >=
                           std::chrono::seconds(5))
            log_audio_window(machine);
        next_frame += frame_time;
        const auto after = std::chrono::steady_clock::now();
        if (after - next_frame > MAX_CATCHUP) {
            next_frame = after;
            ++m_epoch;
            ++m_audio_window_late;
        }
    }
}
} // namespace f3rt::n3ds
