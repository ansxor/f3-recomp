#include "f3rt/netplay.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "netplay_build.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace f3rt::netplay {
namespace {
constexpr uint32_t empty_frame = UINT32_MAX;
constexpr size_t frame_samples = 4096; // interleaved, over four times nominal output
constexpr size_t output_samples = frame_samples * (Rollback::max_window + 2);
void validate_word(InputWord word) {
    if (word & ~input_mask) throw std::runtime_error("Invalid synchronized input word");
}
[[noreturn]] void desync(uint32_t frame, uint32_t local, uint32_t remote) {
    throw std::runtime_error("DESYNC at frame " + std::to_string(frame) +
        ": local CRC " + std::to_string(local) + " remote CRC " + std::to_string(remote));
}
}

void apply_inputs(Machine &m, const std::array<InputWord, 2> &words) {
    m.inputs.fill(0xffffffff);
    m.system_inputs = 0xff;
    for (unsigned slot = 0; slot < words.size(); ++slot) {
        const auto word = words[slot];
        validate_word(word);
        const unsigned shift = slot * 4;
        m.set_input(1, uint32_t(word & 0xf) << shift, true);
        m.set_input(0, uint32_t((word >> 4) & 7) << shift, true);
        m.set_input(0, 0x1000u << slot, word & 0x80);
        m.set_input(0, 0x200u << slot, word & 0x200);
        if (word & 0x100) m.system_inputs &= uint8_t(~(0x10u << slot));
        if (word & 0x400) m.system_inputs &= uint8_t(~2u);
    }
}

Identity machine_identity(const Machine &m) {
    Identity result{};
    const std::vector<uint8_t> *regions[] = {&m.roms.main, &m.roms.sprites, &m.roms.sprites_hi,
        &m.roms.tiles, &m.roms.tiles_hi, &m.roms.sound, &m.roms.samples};
    for (size_t i = 0; i < result.rom_crc.size(); ++i)
        result.rom_crc[i] = crc32(regions[i]->data(), regions[i]->size());
    constexpr char hash[] = F3_NETPLAY_BUILD_HASH;
    auto nibble = [](char c) { return unsigned(c <= '9' ? c - '0' : c - 'a' + 10); };
    for (size_t i = 0; i < result.build_hash.size(); ++i)
        result.build_hash[i] = uint8_t(nibble(hash[2*i]) * 16 + nibble(hash[2*i+1]));
    const auto bytes = m.sync_state_size();
    if (bytes >= (1u << 24)) throw std::runtime_error("Synchronization state exceeds format limit");
    result.state_format = uint32_t(bytes) | (1u << 24) |
        (unsigned(bool(m.sound_native)) << 25) | (unsigned(bool(m.game_video)) << 26) |
        (unsigned(m.audio->backend() == Audio::Backend::Hle) << 27);
    return result;
}

bool versus_match_active(const Machine &m) {
    // $401f6e is the versus latch. The low active-player bits alone also
    // occur in the tutorial/demo; $401f54 is a stage selector, not a mode.
    return m.ram[0x1f6e] == 0 && m.ram[0x1f6f] == 1 && (m.ram[0x1f53] & 3) == 3;
}
bool versus_handoff_ready(const Machine &m) {
    // A frame boundary in versus character selection, before combat. One
    // character may already be confirmed by the host's preceding local play.
    return versus_match_active(m) && (m.ram[0x1f53] & 0xc0) != 0;
}

struct Rollback::Impl {
    struct Frame {
        uint32_t tag = empty_frame;
        std::array<InputWord, 2> actual{}, used{};
        uint8_t known = 0;
        bool simulated = false, exited_versus = false;
        uint32_t crc = 0;
    };
    struct AudioFrame {
        uint32_t tag = empty_frame;
        size_t count = 0;
        std::array<int16_t, frame_samples> samples{};
    };
    struct Hash {
        uint32_t tag = empty_frame, local = 0, remote = 0;
        bool have_local = false, have_remote = false;
    };
    Machine &machine;
    unsigned slot, delay, window;
    const uint64_t origin;
    bool stopped = false, ended = false;
    uint32_t confirmed = 0, dirty = empty_frame;
    size_t snapshot_size;
    std::vector<uint8_t> snapshots;
    std::array<uint32_t, max_window + 1> snapshot_tags{};
    std::array<Frame, history_size> frames{};
    std::array<AudioFrame, max_window + 1> audio{};
    std::array<Hash, history_size> hashes{};
    std::array<Checksum, 64> outgoing{};
    size_t hash_read = 0, hash_write = 0;
    std::array<int16_t, output_samples> output{};
    size_t output_read = 0, output_write = 0;
    uint64_t rollbacks = 0;
    unsigned last_depth = 0, max_depth = 0;

    Impl(Machine &m, unsigned s, unsigned d, unsigned w)
        : machine(m), slot(s), delay(d), window(w), origin(m.frame), snapshot_size(m.state_size()) {
        if (s > 1 || d > 8 || w < 16 || w > max_window)
            throw std::runtime_error("Netplay requires slot 0/1, delay 0..8, rollback window 16..32");
        if (m.allow_main_fallback || m.fallback_instructions || m.sound_trace)
            throw std::runtime_error("Rollback requires strict-native execution without sound tracing");
        if (m.audio->available_frames())
            throw std::runtime_error("Rollback requires an empty initial audio queue");
        if (!versus_match_active(m))
            throw std::runtime_error("Rollback starts only after both versus players have joined");
        snapshots.resize(snapshot_size * (w + 1));
        snapshot_tags.fill(empty_frame);
        for (unsigned f = 0; f < delay; ++f) entry(f).known = 3;
        save(0);
        (void)machine.sync_state_crc(); // Prepare checksum scratch before simulation.
    }
    uint32_t current() const {
        if (machine.frame < origin || machine.frame - origin >= empty_frame - history_size)
            throw std::runtime_error("Netplay frame counter exhausted or moved outside its timeline");
        return uint32_t(machine.frame - origin);
    }
    Frame &entry(uint32_t f) {
        auto &e = frames[f % history_size];
        if (e.tag != f) { e = {}; e.tag = f; }
        return e;
    }
    std::span<uint8_t> snapshot(uint32_t f) {
        return {snapshots.data() + size_t(f % (window + 1)) * snapshot_size, snapshot_size};
    }
    void save(uint32_t f) {
        machine.save_state(snapshot(f));
        snapshot_tags[f % (window + 1)] = f;
    }
    Hash &hash(uint32_t f) {
        auto &h = hashes[(f / checksum_interval) % hashes.size()];
        if (h.tag != f) { h = {}; h.tag = f; }
        return h;
    }
    void step() {
        const auto f = current();
        auto &e = entry(f);
        const auto previous = f ? entry(f - 1).used : std::array<InputWord, 2>{};
        for (unsigned p = 0; p < 2; ++p)
            e.used[p] = (e.known & (1u << p)) ? e.actual[p] : previous[p];
        apply_inputs(machine, e.used);
        if (!machine.run_frame(true) || machine.fallback_instructions)
            throw std::runtime_error("Netplay strict-native execution halted at frame " + std::to_string(f));
        auto &pcm = audio[f % (window + 1)];
        pcm.tag = f;
        pcm.count = 0;
        if (machine.audio->backend() != Audio::Backend::Hle) {
            pcm.count = machine.audio->render(pcm.samples.data(), pcm.samples.size() / 2) * 2;
            if (machine.audio->available_frames())
                throw std::runtime_error("Netplay frame audio capacity exceeded");
        }
        save(f + 1);
        if ((f + 1) % checksum_interval == 0) e.crc = machine.sync_state_crc();
        e.exited_versus = !versus_match_active(machine);
        e.simulated = true;
    }
    void promote() {
        while (confirmed < current()) {
            auto &e = entry(confirmed);
            if (e.known != 3 || !e.simulated) break;
            if (e.actual != e.used) throw std::logic_error("Unreconciled input reached confirmation");
            const auto &pcm = audio[confirmed % (window + 1)];
            if (pcm.tag != confirmed) throw std::logic_error("Confirmed audio overwritten");
            if (output_write - output_read + pcm.count > output.size())
                throw std::runtime_error("Confirmed audio queue full; drain render_audio while stalled");
            for (size_t i = 0; i < pcm.count; ++i) output[(output_write++) % output.size()] = pcm.samples[i];
            ++confirmed;
            if (confirmed % checksum_interval == 0) {
                auto &h = hash(confirmed);
                h.local = e.crc;
                h.have_local = true;
                if (h.have_remote && h.remote != h.local) desync(confirmed, h.local, h.remote);
                if (hash_write - hash_read == outgoing.size())
                    throw std::runtime_error("Checksum queue full; drain receive_checksum_to_send");
                outgoing[(hash_write++) % outgoing.size()] = {confirmed, h.local};
            }
            if (e.exited_versus) {
                // Only an actual-input-confirmed exit ends a match. Discard any
                // speculative post-match frames and their unpublished PCM.
                machine.load_state(snapshot(confirmed));
                dirty = empty_frame;
                stopped = ended = true;
                break;
            }
        }
    }
    void synchronize() {
        if (stopped) return;
        if (dirty != empty_frame) {
            const uint32_t end = current(), begin = dirty;
            if (begin < confirmed || snapshot_tags[begin % (window + 1)] != begin)
                throw std::logic_error("Rollback exceeded retained snapshot window");
            machine.audio->begin_rollback(origin + begin, origin + end);
            machine.load_state(snapshot(begin));
            last_depth = end - begin;
            max_depth = std::max(max_depth, last_depth);
            ++rollbacks;
            dirty = empty_frame;
            // Accurate devices rerun and replace speculative PCM. HLE only
            // journals commands here; its worker and rendered PCM never rewind.
            while (current() < end) step();
            machine.audio->end_rollback();
        }
        promote();
    }
};

Rollback::Rollback(Machine &m, unsigned slot, unsigned delay, unsigned window)
    : impl(std::make_unique<Impl>(m, slot, delay, window)) {}
Rollback::~Rollback() = default;
uint32_t Rollback::frame() const { return impl->current(); }
uint32_t Rollback::confirmed_frame() const { return impl->confirmed; }
uint64_t Rollback::origin_frame() const { return impl->origin; }
bool Rollback::match_finished() const { return impl->ended; }
bool Rollback::needs_local_input() const {
    if (impl->stopped) return false;
    const auto f = frame() + impl->delay;
    const auto &e = impl->frames[f % history_size];
    return e.tag != f || !(e.known & (1u << impl->slot));
}
Input Rollback::local_input(InputWord word) {
    validate_word(word);
    if (!needs_local_input()) throw std::logic_error("Local frame input already sampled");
    const auto f = frame() + impl->delay;
    auto &e = impl->entry(f);
    e.actual[impl->slot] = word;
    e.known |= uint8_t(1u << impl->slot);
    return {f, word};
}
void Rollback::receive(Input input) {
    validate_word(input.word);
    auto &p = *impl;
    if (input.frame >= frame() && input.frame - frame() >= history_size / 2)
        throw std::runtime_error("Peer input exceeds bounded receive window");
    if (input.frame < p.confirmed && p.confirmed - input.frame >= history_size / 2) return;
    auto &e = p.entry(input.frame);
    const unsigned peer = p.slot ^ 1;
    if (e.known & (1u << peer)) {
        if (e.actual[peer] != input.word) throw std::runtime_error("Peer changed previously received input");
        return;
    }
    if (input.frame < p.confirmed) throw std::logic_error("Missing input below confirmed frontier");
    e.actual[peer] = input.word;
    e.known |= uint8_t(1u << peer);
    if (e.simulated && e.used[peer] != input.word) p.dirty = std::min(p.dirty, input.frame);
}
void Rollback::receive_checksum(Checksum value) {
    if (!value.frame || value.frame % checksum_interval ||
        (value.frame > frame() && value.frame - frame() > history_size / 2))
        throw std::runtime_error("Invalid peer checksum frame");
    if (value.frame < impl->confirmed && impl->confirmed - value.frame >= history_size) return;
    auto &h = impl->hash(value.frame);
    if (h.have_remote && h.remote != value.crc)
        throw std::runtime_error("Peer changed previously received checksum");
    h.remote = value.crc;
    h.have_remote = true;
    if (h.have_local && h.local != h.remote) desync(value.frame, h.local, h.remote);
}
void Rollback::synchronize() { impl->synchronize(); }
bool Rollback::advance() {
    auto &p = *impl;
    p.synchronize();
    if (p.stopped || frame() - p.confirmed >= p.window) return false;
    const auto &e = p.entry(frame());
    if (!(e.known & (1u << p.slot))) throw std::logic_error("Local input was not scheduled before advance");
    p.step();
    p.promote();
    return true;
}
bool Rollback::receive_checksum_to_send(Checksum &value) {
    if (impl->hash_read == impl->hash_write) return false;
    value = impl->outgoing[(impl->hash_read++) % impl->outgoing.size()];
    return true;
}
size_t Rollback::render_audio(int16_t *stereo, size_t max_frames) {
    auto &p = *impl;
    if (p.machine.audio->backend() == Audio::Backend::Hle)
        return p.machine.audio->render(stereo, max_frames);
    const auto count = std::min(max_frames * 2, p.output_write - p.output_read);
    for (size_t i = 0; i < count; ++i) stereo[i] = p.output[(p.output_read++) % p.output.size()];
    return count / 2;
}
void Rollback::restore_confirmed() {
    auto &p = *impl;
    if (p.snapshot_tags[p.confirmed % (p.window + 1)] != p.confirmed)
        throw std::logic_error("Confirmed state no longer retained");
    p.machine.load_state(p.snapshot(p.confirmed));
    p.dirty = empty_frame;
    p.stopped = true;
}
uint64_t Rollback::rollback_count() const { return impl->rollbacks; }
unsigned Rollback::last_rollback_depth() const { return impl->last_depth; }
unsigned Rollback::maximum_rollback_depth() const { return impl->max_depth; }
} // namespace f3rt::netplay
