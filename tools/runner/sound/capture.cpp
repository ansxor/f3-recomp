#include "capture.hpp"

#include "f3rt/audio.hpp"
#include "f3rt/memory_map.hpp"
#include "interpreter.hpp"

#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace f3rt::runner {

uint32_t parse_number(std::string_view text) {
    std::string s(text);
    int base = 10;
    if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
        base = 16;
        s.erase(0, 2);
    } else if (!s.empty() && s[0] == '$') {
        base = 16;
        s.erase(0, 1);
    }
    if (s.empty() || s[0] == '-' || s[0] == '+') throw std::runtime_error("Invalid number: " + std::string(text));
    size_t end = 0;
    const unsigned long value = std::stoul(s, &end, base);
    if (end != s.size() || value > UINT32_MAX)
        throw std::runtime_error("Invalid number: " + std::string(text));
    return uint32_t(value);
}

std::pair<uint32_t, uint32_t> parse_range(std::string_view text) {
    const auto dash = text.find('-');
    if (dash == std::string_view::npos) {
        const uint32_t v = parse_number(text);
        return {v, v};
    }
    const uint32_t first = parse_number(text.substr(0, dash));
    const uint32_t last = parse_number(text.substr(dash + 1));
    if (first > last) throw std::runtime_error("Range start exceeds end: " + std::string(text));
    return {first, last};
}

std::string hex_bytes(std::span<const uint8_t> bytes) {
    std::string out;
    char buf[3];
    for (uint8_t b : bytes) {
        std::snprintf(buf, sizeof buf, "%02x", b);
        out += buf;
    }
    return out;
}

namespace {

constexpr uint32_t RETURN_SENTINEL = 0x000200; // ROM address that is never executed here

std::vector<const SoundProfile *> &profiles() {
    static std::vector<const SoundProfile *> registered;
    return registered;
}

} // namespace

SoundProfileRegistrar::SoundProfileRegistrar(const SoundProfile &profile) {
    profiles().push_back(&profile);
}

const SoundProfile *find_sound_profile(std::string_view set) {
    for (const SoundProfile *p : profiles())
        if (set == p->set) return p;
    return nullptr;
}

void run_routine(Machine &m, const RoutineCall &call) {
    auto &cpu = m.cpu;
    cpu.stopped = 0;
    uint32_t sp = cpu.a[7];
    if (call.stack_word) {
        sp -= 2;
        m.write16(sp, *call.stack_word);
    }
    sp -= 4;
    m.write32(sp, RETURN_SENTINEL);
    cpu.a[7] = sp;
    if (call.d0) cpu.d[0] = *call.d0;
    cpu.pc = call.address;
    for (unsigned guard = 0; cpu.pc != RETURN_SENTINEL; ++guard) {
        if (guard > 1000000 || cpu.halted) {
            std::ostringstream e;
            e << "Main routine 0x" << std::hex << call.address << " did not return (pc=0x"
              << cpu.pc << ")";
            throw std::runtime_error(e.str());
        }
        m.interpreter->run_main(1);
    }
    // Pop the caller-pushed argument like `addq.l #2, a7`.
    cpu.a[7] = cpu.a[7] + (call.stack_word ? 2u : 0u);
}

std::vector<uint8_t> ring_bytes_between(const Machine &m, uint16_t before, uint16_t after) {
    std::vector<uint8_t> bytes;
    size_t from = (before / 2) & MAILBOX_RING_MASK;
    const size_t to = (after / 2) & MAILBOX_RING_MASK;
    while (from != to) {
        bytes.push_back(m.shared[from]);
        from = (from + 1) & MAILBOX_RING_MASK;
    }
    return bytes;
}

CodeCall request_code(Machine &m, uint32_t routine, uint32_t code) {
    const uint16_t before = RingBufferState::read_producer(m);
    run_routine(m, {routine, uint16_t(code), std::nullopt});
    const uint16_t after = RingBufferState::read_producer(m);
    return {code, ring_bytes_between(m, before, after)};
}

std::unique_ptr<MachineRunner> boot_to_freeze(const RunnerConfig &cfg, uint64_t boot_frames,
                                              bool record_boot_wav) {
    auto runner = std::make_unique<MachineRunner>(cfg);
    auto &m = runner->machine();
    if (cfg.audio_backend == Audio::Backend::Enhanced &&
        !m.audio->backend_available(Audio::Backend::Enhanced)) {
        throw std::runtime_error("Enhanced audio is not supported for this game");
    }
    for (uint64_t f = 0; f < boot_frames; ++f) {
        if (!m.run_frame(/*translated=*/false) || m.cpu.halted) {
            std::ostringstream error;
            error << "Main CPU halted during boot at frame " << m.frame << " PC 0x" << std::hex
                  << m.cpu.pc;
            throw std::runtime_error(error.str());
        }
        runner->drain_audio(record_boot_wav);
    }
    if (m.audio->is_reset()) {
        throw std::runtime_error("Sound CPU is still held in reset; increase --boot-frames");
    }
    m.cpu.cycles = m.audio->clock_ticks();
    uint32_t wait_cycles = 0;
    const uint32_t max_wait = 16000000;
    while (RingBufferState::read_consumer(m) != RingBufferState::read_producer(m) &&
           wait_cycles < max_wait) {
        constexpr uint32_t STEP = 1000;
        m.audio->advance(STEP);
        m.cpu.cycles += STEP;
        wait_cycles += STEP;
        if (wait_cycles % 16000 == 0) runner->drain_audio(record_boot_wav);
    }
    runner->drain_audio(record_boot_wav);
    if (RingBufferState::read_consumer(m) != RingBufferState::read_producer(m)) {
        throw std::runtime_error(
            "Sound CPU consumer did not synchronize with producer before freeze");
    }
    return runner;
}

PostBootSnapshot take_post_boot_snapshot(Machine &m) {
    PostBootSnapshot snapshot;
    snapshot.state.resize(m.state_size());
    m.save_state(snapshot.state);
    snapshot.boot_commands =
        m.audio->backend() == Audio::Backend::Enhanced ? m.audio->enhanced_stats().commands : 0;
    return snapshot;
}

std::span<const uint8_t> boot_packet_stream(const Machine &m, uint64_t boot_commands) {
    const size_t end = (RingBufferState::read_producer(m) / 2) & MAILBOX_RING_MASK;
    uint64_t packets = 0;
    for (size_t at = 0; at < end;) {
        const uint8_t length = m.shared[at];
        if (length < 2 || at + length > end) {
            packets = UINT64_MAX;
            break;
        }
        at += length;
        ++packets;
    }
    if (packets != boot_commands)
        throw std::runtime_error("Boot mailbox ring no longer holds the whole boot packet stream (" +
                                 std::to_string(packets) + " parsed, " +
                                 std::to_string(boot_commands) +
                                 " sent); cannot restore the HLE post-boot state");
    return {m.shared.data(), end};
}

void restore_post_boot(std::unique_ptr<MachineRunner> &runner, const RunnerConfig &cfg,
                       const PostBootSnapshot &snapshot) {
    if (cfg.audio_backend == Audio::Backend::Enhanced) {
        runner = std::make_unique<MachineRunner>(cfg);
        auto &m = runner->machine();
        m.load_state(snapshot.state);
        m.audio->set_reset(true);
        m.audio->set_reset(false);
    } else {
        runner->machine().load_state(snapshot.state);
    }
    auto &m = runner->machine();
    m.cpu.cycles = m.audio->clock_ticks();
}

void restore_for_capture(Machine &m, const PostBootSnapshot &snapshot) {
    m.load_state(snapshot.state);
    m.audio->set_reset(true);
    m.cpu.cycles = m.audio->clock_ticks();
}

std::vector<std::vector<uint8_t>> split_packets(std::span<const uint8_t> bytes) {
    std::vector<std::vector<uint8_t>> packets;
    for (size_t at = 0; at < bytes.size();) {
        const size_t length = bytes[at];
        if (length < 2 || at + length > bytes.size())
            throw std::runtime_error("Malformed mailbox packet at ring offset " + std::to_string(at));
        packets.emplace_back(bytes.begin() + ptrdiff_t(at), bytes.begin() + ptrdiff_t(at + length));
        at += length;
    }
    return packets;
}

void apply_sound_bank(Machine &m, const SoundProfile *profile, uint32_t bank) {
    m.write16(MAIN_SOUND_BANKSWITCH_BASE + bank * 2, 0);
    if (!profile) return;
    for (const uint32_t word : profile->bank_words)
        if (word) m.write16(word, uint16_t(bank));
}

void capture_sound_test(Machine &m, const SoundProfile *profile, uint32_t request_routine,
                        uint32_t code, bool no_prelude, SoundTestCapture &out) {
    if (profile && profile->selector_base_ptr) {
        // descriptor of the code in the game's selector table
        const uint32_t base = m.read32(profile->selector_base_ptr);
        const uint32_t desc = m.read32(base + code * 4);
        out.descriptor.reserve(8);
        for (uint32_t i = 0; i < 8; ++i) out.descriptor.push_back(m.read8(desc + i));
        out.descriptor_type = out.descriptor[0];
        if (profile->category) {
            run_routine(m, {profile->category, std::nullopt, uint16_t(code)});
            out.category = int(m.cpu.d[1] & 0xffff);
            if (!no_prelude && out.category <= 1 && profile->prelude) {
                run_routine(m, {profile->prelude, std::nullopt, uint16_t(code)});
                const uint32_t first = m.cpu.d[0] & 0xffff, second = m.cpu.d[1] & 0xffff;
                out.calls.push_back(request_code(m, request_routine, first));
                out.calls.push_back(request_code(m, request_routine, second));
            }
        }
    }
    out.calls.push_back(request_code(m, request_routine, code));
}

} // namespace f3rt::runner
