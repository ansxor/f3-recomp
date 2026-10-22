// hle::Core and the F3RP replay script:
//  * ReplayCodec.*  encoder layout and malformed-script rejection; need no ROM (synthetic sound ROM with no songs).
//  * HleCore.*      A2 (AudioEngine vs Core + replay, bit-exact), the encode->replay round trip and A3 (chunk invariance
//                   property). They load sets from the directories next to F3_ROM_DIR (kirameki, landmakrj) and skip
//                   when a set is unavailable. Land Maker needs its real boot packet stream, so one 900-frame
//                   interpreted boot runs (about 5 s) the first time it is needed.
#include "audio/hle/audio.hpp"
#include "audio/hle/core.hpp"
#include "audio/hle/replay.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/rom.hpp"
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace f3rt;
using namespace f3rt::hle;
using Packets = std::vector<std::vector<uint8_t>>;

// ---- helpers ---------------------------------------------------------------------------------------------------------

std::vector<uint8_t> hex_bytes(const std::string &hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) bytes.push_back(uint8_t(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return bytes;
}
// Length-prefixed packets back to back, as the mailbox ring holds them.
Packets split_packets(const std::vector<uint8_t> &bytes) {
    Packets packets;
    for (size_t at = 0; at < bytes.size();) {
        const size_t n = bytes[at];
        if (n < 2 || at + n > bytes.size()) throw std::runtime_error("bad packet stream");
        packets.emplace_back(bytes.begin() + ptrdiff_t(at), bytes.begin() + ptrdiff_t(at + n));
        at += n;
    }
    return packets;
}

// Runs `fn` on a fresh thread with flush-to-zero set, like the AudioEngine worker; the test thread's FP mode is not touched.
template <class F> void on_worker_thread(F &&fn) {
    std::exception_ptr failure;
    std::thread thread([&] {
        Core::enable_flush_denormals();
        try {
            fn();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    thread.join();
    if (failure) std::rethrow_exception(failure);
}

struct Collect final : PcmSink {
    std::vector<float> pcm;
    void write(const float *stereo, size_t frames) override { pcm.insert(pcm.end(), stereo, stereo + frames * 2); }
};
// FNV-1a over the float bits; the A3 property compares hashes of long renders.
struct Hash final : PcmSink {
    uint64_t value = 1469598103934665603ull, frames = 0;
    void write(const float *stereo, size_t n) override {
        for (size_t i = 0; i < n * 2; ++i) {
            uint32_t bits;
            std::memcpy(&bits, stereo + i, 4);
            value = (value ^ bits) * 1099511628211ull;
        }
        frames += n;
    }
};

::testing::AssertionResult same_bits(const std::vector<float> &a, const std::vector<float> &b) {
    if (a.size() != b.size())
        return ::testing::AssertionFailure() << "frame counts differ: " << a.size() / 2 << " vs " << b.size() / 2;
    if (!a.empty() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) != 0) {
        size_t i = 0;
        while (std::memcmp(&a[i], &b[i], sizeof(float)) == 0) ++i;
        return ::testing::AssertionFailure() << "first differing sample " << i << " (frame " << i / 2 << "): " << a[i]
                                             << " vs " << b[i];
    }
    return ::testing::AssertionSuccess();
}

// ---- ROM-free fixtures -----------------------------------------------------------------------------------------------

// A sound ROM that passes the HLE structure checks with no sequence present, and all-zero samples.
struct SyntheticRom {
    std::vector<uint8_t> sound = std::vector<uint8_t>(SOUND_ROM_SIZE, 0);
    std::vector<uint16_t> samples = std::vector<uint16_t>(SOUND_ROM_SIZE, 0);
    SyntheticRom() {
        sound[0x20000 + 2] = 0x10; // stream directory: header block at +0x1000
        sound[0x21000 + 2] = 0x04; // header block size 0x400, every sequence slot empty
    }
};

ReplayScript sample_script() {
    ReplayScript script;
    script.gain_raw = {48, 47};
    script.page = 1;
    script.packets = {{3, 0x82, 0}, {4, 0x21, 0, 0x28}, {3, 0x80, 5}};
    return script;
}

// ---- ROM-backed fixtures ---------------------------------------------------------------------------------------------

struct SetRom {
    RomSet rom;
    std::vector<uint16_t> words;
};
std::filesystem::path roms_root() {
    const char *dir = std::getenv("F3_ROM_DIR");
#ifdef F3RT_DEFAULT_ROM_DIR
    if (!dir) dir = F3RT_DEFAULT_ROM_DIR;
#endif
    if (!dir || !*dir) return {};
    return std::filesystem::path(dir).parent_path();
}
// nullptr (and `why`) when the set cannot be loaded.
const SetRom *load_set(const std::string &set, std::string &why) {
    static std::map<std::string, std::unique_ptr<SetRom>> cache;
    static std::map<std::string, std::string> failures;
    if (const auto it = cache.find(set); it != cache.end()) return it->second.get();
    if (const auto it = failures.find(set); it != failures.end()) return why = it->second, nullptr;
    const auto root = roms_root();
    if (root.empty()) return why = "F3_ROM_DIR not set", nullptr;
    try {
        auto loaded = std::make_unique<SetRom>();
        loaded->rom = RomSet::load(root / set, set);
        loaded->words.resize(loaded->rom.samples.size() / 2);
        for (size_t i = 0; i < loaded->words.size(); ++i)
            loaded->words[i] = uint16_t(loaded->rom.samples[i * 2] << 8 | loaded->rom.samples[i * 2 + 1]);
        return (cache[set] = std::move(loaded)).get();
    } catch (const std::exception &e) {
        failures[set] = std::string("ROM set ") + set + " unavailable: " + e.what();
        return why = failures[set], nullptr;
    }
}

// What the post-boot snapshot of sound-extract holds for the sound side.
struct Boot {
    std::vector<uint8_t> ring;   // mailbox ring bytes 0 .. producer
    Packets packets;             // the same, split; packets above the engine's 8-byte limit are dropped here
    std::array<uint8_t, 2> gain{};
    uint64_t commands = 0;
};
// Interpreted boot of `set` for 900 frames as sound-extract does, then the boot packet stream.
const Boot &boot_of(const std::string &set) {
    static std::map<std::string, Boot> cache;
    if (const auto it = cache.find(set); it != cache.end()) return it->second;
    const auto root = roms_root();
    Machine m(RomSet::load(root / set, set));
    m.allow_main_fallback = true;
    m.audio->set_backend(Audio::Backend::Enhanced);
    std::vector<int16_t> drain(8192);
    for (unsigned f = 0; f < 900; ++f) {
        if (!m.run_frame(false) || m.cpu.halted) throw std::runtime_error("main CPU halted during boot");
        while (m.audio->render(drain.data(), drain.size() / 2)) {}
    }
    if (m.audio->is_reset()) throw std::runtime_error("sound CPU still held in reset after boot");
    auto producer = [&] { return uint16_t(uint16_t(m.shared[MAILBOX_PRODUCER_H]) << 8 | m.shared[MAILBOX_PRODUCER_L]); };
    auto consumer = [&] { return uint16_t(uint16_t(m.shared[MAILBOX_CONSUMER_H]) << 8 | m.shared[MAILBOX_CONSUMER_L]); };
    m.cpu.cycles = m.audio->clock_ticks();
    for (uint32_t waited = 0; consumer() != producer() && waited < 16000000; waited += 1000) {
        m.audio->advance(1000);
        m.cpu.cycles += 1000;
    }
    if (consumer() != producer()) throw std::runtime_error("sound consumer did not catch up");
    Boot boot;
    const size_t end = (producer() / 2) & MAILBOX_RING_MASK;
    boot.ring.assign(m.shared.begin(), m.shared.begin() + ptrdiff_t(end));
    uint64_t parsed = 0;
    for (auto &packet : split_packets(boot.ring)) {
        ++parsed;
        if (packet.size() <= REPLAY_MAX_PACKET) boot.packets.push_back(std::move(packet));
    }
    boot.commands = m.audio->enhanced_stats().commands;
    if (parsed != boot.commands) throw std::runtime_error("boot ring does not hold the whole boot packet stream");
    boot.gain = {m.shared[MAILBOX_GAIN_CH0], m.shared[MAILBOX_GAIN_CH1]};
    return cache[set] = std::move(boot);
}

struct Song {
    const char *set, *name;
    std::optional<uint8_t> page;
    std::vector<std::string> calls; // hex of the ring bytes of each request, in order
};
// Request bytes as sound-extract captured them from the game's own request routine (Kirameki: prelude setup codes
// then the code; Land Maker: the code only, its effect setup is part of the boot stream).
const std::string KIRAMEKI_PRELUDE_A = "0421002804210100042102140421031a04210425042105000421066804210740"
                                       "0421082004210927",
                  KIRAMEKI_PRELUDE_B = "0421002804210100042102140421031a04210425042105000421066804210740"
                                       "042108140421091a";
const std::vector<Song> &songs() {
    static const std::vector<Song> list = {
        {"kirameki", "0x187", 1, {"032005", KIRAMEKI_PRELUDE_A, "03812c04862c7f"}},
        {"kirameki", "0x001", std::nullopt, {"032005", KIRAMEKI_PRELUDE_A, "0381040486047f"}},
        {"kirameki", "0x100", std::nullopt, {"032005", KIRAMEKI_PRELUDE_B, "0381090486097f"}},
        {"kirameki", "0x188", std::nullopt, {"032005", KIRAMEKI_PRELUDE_B, "03812d04862d7f"}},
        {"landmakrj", "0x50", std::nullopt, {"03810604860674"}},
        {"landmakrj", "0x51", std::nullopt, {"03810804860874"}},
        {"landmakrj", "0x52", std::nullopt, {"03810904860974"}},
        {"landmakrj", "0x53", std::nullopt, {"03810a04860a64"}},
    };
    return list;
}
// Kirameki's boot leaves its preset/parameters to the prelude; its post-boot gain bytes were 48/48 (verified:
// the replay matches sound-extract's WAV of 0x187 sample for sample with them).
struct Inputs {
    ReplayScript script;   // the F3RP script of the same requests
    Packets ring_calls;    // the request bytes, one entry per request (the mailbox path writes them as they are)
    const Boot *boot = nullptr;
};
Inputs inputs_for(const Song &song) {
    Inputs in;
    const bool landmaker = std::string(song.set) == "landmakrj";
    if (landmaker) in.boot = &boot_of(song.set);
    in.script.gain_raw = landmaker ? in.boot->gain : std::array<uint8_t, 2>{48, 48};
    in.script.page = song.page;
    if (in.boot) in.script.packets = in.boot->packets;
    for (const auto &hex : song.calls) {
        const auto bytes = hex_bytes(hex);
        in.ring_calls.push_back(bytes);
        for (auto &packet : split_packets(bytes)) in.script.packets.push_back(std::move(packet));
    }
    return in;
}

// The game-facing path of the sound-extract run: a fresh AudioEngine restored from a post-boot snapshot, reset
// pulse, gain bytes, optional bank write, request bytes through the mailbox ring, then 10 ms advance + render slices.
std::vector<float> render_engine(const SetRom &set, const Inputs &in, unsigned slices) {
    std::array<uint8_t, 0x800> shared{};
    size_t producer = 0;
    if (in.boot) {
        std::copy(in.boot->ring.begin(), in.boot->ring.end(), shared.begin());
        producer = in.boot->ring.size();
    }
    auto publish = [&] {
        const unsigned doubled = unsigned(producer & MAILBOX_RING_MASK) * 2;
        shared[MAILBOX_PRODUCER_H] = uint8_t(doubled >> 8);
        shared[MAILBOX_PRODUCER_L] = uint8_t(doubled);
    };
    shared[MAILBOX_GAIN_CH0] = in.script.gain_raw[0];
    shared[MAILBOX_GAIN_CH1] = in.script.gain_raw[1];
    publish();
    AudioEngine engine(set.rom.sound, set.words, shared.data());
    std::array<uint8_t, AudioEngine::STATE_BYTES> state{};
    const uint64_t clock = 244300337, commands = in.boot ? in.boot->commands : 0;
    for (unsigned b = 0; b < 8; ++b) {
        state[b] = uint8_t(clock >> (56 - b * 8));
        state[8 + b] = uint8_t(commands >> (56 - b * 8));
    }
    const unsigned consumer = unsigned(producer * 2) & 0x7fe;
    state[16] = uint8_t(consumer >> 8);
    state[17] = uint8_t(consumer);
    shared[MAILBOX_CONSUMER_H] = state[16];
    shared[MAILBOX_CONSUMER_L] = state[17];
    engine.load(state);
    engine.set_reset(true);
    engine.set_reset(false);
    if (in.script.page) engine.set_bank(*in.script.page);
    for (const auto &bytes : in.ring_calls) {
        for (const uint8_t byte : bytes) shared[producer++ & MAILBOX_RING_MASK] = byte;
        publish();
        engine.shared_write(MAILBOX_PRODUCER_L);
    }
    std::vector<float> pcm, block(4096 * 2);
    for (unsigned slice = 0; slice < slices; ++slice) {
        engine.advance(160000);
        while (const size_t n = engine.render(block.data(), 4096)) pcm.insert(pcm.end(), block.begin(), block.begin() + ptrdiff_t(n * 2));
    }
    return pcm;
}

std::vector<float> render_core_replay(const SetRom &set, const std::vector<uint8_t> &script, unsigned slices) {
    std::vector<float> pcm;
    on_worker_thread([&] {
        Core core(set.rom.sound, set.words);
        Collect sink;
        core.set_pcm_sink(&sink);
        replay(core, script);
        for (unsigned slice = 1; slice <= slices; ++slice) core.advance(uint64_t(slice) * 160000);
        pcm = std::move(sink.pcm);
    });
    return pcm;
}

} // namespace

// ---- ReplayCodec: no ROM needed --------------------------------------------------------------------------------------

TEST(ReplayCodec, EncodeLayout) {
    const auto bytes = encode_replay(sample_script());
    const std::vector<uint8_t> expected = {
        'F', '3', 'R', 'P', 1, 0, 7, 0,         // header, 7 items
        0x01,                                    // RESET
        0x02, 0, 48, 0x02, 1, 47,                // GAIN 0, GAIN 1
        0x03, 1,                                 // BANK 1
        0x04, 3, 3, 0x82, 0, 0x04, 4, 4, 0x21, 0, 0x28, 0x04, 3, 3, 0x80, 5};
    EXPECT_EQ(bytes, expected);
    ReplayScript bare;
    EXPECT_EQ(encode_replay(bare).size(), 8u + 1 + 6) << "no BANK and no packets: RESET and two GAIN items only";
}

TEST(ReplayCodec, EncodeRejectsBadPackets) {
    const Packets rejected = {{},           // empty
                              {1},          // one byte
                              {3, 0x80},    // prefix says 3, two bytes
                              {2, 0x80, 0}, // prefix says 2, three bytes
                              {9, 1, 2, 3, 4, 5, 6, 7, 8}}; // nine bytes: above the engine's 8-byte limit
    for (const auto &bad : rejected) {
        ReplayScript script;
        script.packets = {bad};
        EXPECT_THROW(encode_replay(script), std::invalid_argument) << "packet of " << bad.size() << " bytes";
    }
    ReplayScript longest;
    longest.packets = {{8, 0x80, 1, 2, 3, 4, 5, 6}};
    EXPECT_NO_THROW(encode_replay(longest));
    ReplayScript many;
    many.packets.assign(65535, {3, 0x82, 0}); // 3 fixed items + 65535 packets exceeds the 16-bit item count
    EXPECT_THROW(encode_replay(many), std::invalid_argument);
    many.packets.resize(65532);
    EXPECT_NO_THROW(encode_replay(many));
}

TEST(ReplayCodec, ReplayAppliesValidScriptWithoutAdvancingTime) {
    SyntheticRom rom;
    Core core(rom.sound, rom.samples);
    EXPECT_NO_THROW(replay(core, encode_replay(sample_script())));
    EXPECT_EQ(core.tick(), 0u);
    EXPECT_EQ(core.frames(), 0u);
    core.advance(160000);
    EXPECT_GT(core.frames(), 0u);
    EXPECT_THROW(replay(core, encode_replay(sample_script())), std::invalid_argument) << "Core already ran";
}

TEST(ReplayCodec, ReplayRejectsMalformedScripts) {
    const auto valid = encode_replay(sample_script());
    auto with = [&](size_t at, uint8_t value) {
        auto bytes = valid;
        bytes.at(at) = value;
        return bytes;
    };
    struct Case {
        const char *what;
        std::vector<uint8_t> bytes;
    };
    std::vector<Case> cases = {
        {"empty", {}},
        {"short header", {valid.begin(), valid.begin() + 7}},
        {"magic", with(0, 'X')},
        {"version 0", with(4, 0)},
        {"version 2", with(4, 2)},
        {"reserved", with(5, 1)},
        {"count above items", with(6, 8)},
        {"count below items", with(6, 6)},
        {"unknown op", with(8, 9)},
        {"GAIN channel 2", with(10, 2)},
        // The last item is 04 n b0 b1 b2: n at size-4, the packet's own length byte (b0) at size-3.
        {"PACKET length 1", with(valid.size() - 4, 1)},
        {"PACKET length 9", with(valid.size() - 4, 9)},
        {"PACKET prefix != length", with(valid.size() - 3, 4)},
        {"truncated payload", {valid.begin(), valid.end() - 1}},
        {"trailing byte", [&] { auto bytes = valid; bytes.push_back(0); return bytes; }()},
        {"GAIN cut short", {'F', '3', 'R', 'P', 1, 0, 1, 0, 0x02, 0}},
        {"BANK cut short", {'F', '3', 'R', 'P', 1, 0, 1, 0, 0x03}},
        {"PACKET cut short", {'F', '3', 'R', 'P', 1, 0, 1, 0, 0x04}},
    };
    SyntheticRom rom;
    for (const auto &c : cases) {
        Core core(rom.sound, rom.samples);
        EXPECT_THROW(replay(core, c.bytes), std::invalid_argument) << c.what;
    }
    // A bad script is rejected before anything is applied: a leading packet that would throw std::runtime_error when
    // applied ([3,0x20,0] selects effect preset 0, which this ROM cannot resolve) must not run.
    ReplayScript script;
    script.packets = {{3, 0x20, 0}};
    auto bad = encode_replay(script);
    {
        Core core(rom.sound, rom.samples);
        EXPECT_THROW(replay(core, bad), std::runtime_error) << "a valid script propagates the Core's exception";
    }
    bad.push_back(0);
    Core core(rom.sound, rom.samples);
    try {
        replay(core, bad);
        FAIL() << "expected rejection";
    } catch (const std::invalid_argument &) {
    } catch (...) {
        FAIL() << "the script was applied before it was validated";
    }
}

// ---- A2 and the round trip --------------------------------------------------------------------------------------------

TEST(HleCore, EncodeReplayRoundTripMatchesManualCalls) {
    std::string why;
    const auto *set = load_set("kirameki", why);
    if (!set) GTEST_SKIP() << why;
    const auto in = inputs_for(songs()[0]);
    const auto bytes = encode_replay(in.script);
    const auto via_replay = render_core_replay(*set, bytes, 200);
    std::vector<float> manual;
    on_worker_thread([&] {
        Core core(set->rom.sound, set->words);
        Collect sink;
        core.set_pcm_sink(&sink);
        core.reset(0);
        core.set_gain(0, in.script.gain_raw[0], 0);
        core.set_gain(1, in.script.gain_raw[1], 0);
        core.set_bank(*in.script.page, 0);
        uint64_t serial = 0;
        for (const auto &packet : in.script.packets) core.command(packet, ++serial, 0);
        for (unsigned slice = 1; slice <= 200; ++slice) core.advance(uint64_t(slice) * 160000);
        manual = std::move(sink.pcm);
    });
    ASSERT_FALSE(manual.empty());
    EXPECT_TRUE(same_bits(via_replay, manual));
    EXPECT_GT(*std::max_element(manual.begin(), manual.end()), 0.01f) << "the song must be audible in the first 2 s";
}

// A2: the AudioEngine path (mailbox, snapshot restore, 10 ms slices) and Core + replay render the same float stream.
// One song per set: Kirameki 0x187 with BANK, Land Maker 0x50 with its real boot packet stream.
TEST(HleCore, EngineAndCoreReplayAreBitExact) {
    for (const size_t index : {size_t(0), size_t(4)}) {
        const Song &song = songs()[index];
        SCOPED_TRACE(std::string(song.set) + " " + song.name);
        std::string why;
        const auto *set = load_set(song.set, why);
        if (!set) {
            GTEST_LOG_(INFO) << "skipped " << song.set << ": " << why;
            continue;
        }
        const auto in = inputs_for(song);
        const unsigned slices = 600; // 6 s
        const auto engine = render_engine(*set, in, slices);
        const auto core = render_core_replay(*set, encode_replay(in.script), slices);
        ASSERT_FALSE(engine.empty());
        EXPECT_TRUE(same_bits(engine, core));
        EXPECT_GT(*std::max_element(core.begin(), core.end()), 0.01f) << "the song must be audible within 6 s";
    }
}

// ---- A3: how the caller slices time ------------------------------------------------------------------------------------
//
// advance() always splits at the clock ticks (every 16000 ticks from the reset), so a schedule whose targets
// are multiples of TIMER_TICKS produces exactly the render calls of the canonical 1 ms stepping: bit-exact (property
// 1). A target inside a millisecond adds a render call, and Effects::mix keeps per-call state (the auto-bypass decision
// and the LFO phase it carries while bypassed), so a sub-millisecond schedule is not bit-exact: it agrees to within
// float rounding noise (property 2; measured on Kirameki 0x187 with 1-frame steps: 20 of 238088 floats differ, by at
// most 1.5e-8, none after int16 conversion).
struct ChunkItem {
    std::string name;
    const SetRom *set;
    std::vector<uint8_t> script;
    std::vector<float> reference;
    uint64_t reference_hash;
};
constexpr uint64_t CHUNK_TOTAL = 4 * MAIN_CLOCK; // 4 s of audio per case
const std::vector<ChunkItem> &chunk_items() {
    static const std::vector<ChunkItem> items = [] {
        std::vector<ChunkItem> list;
        for (const auto &song : songs()) {
            std::string why;
            const auto *set = load_set(song.set, why);
            if (!set) {
                GTEST_LOG_(INFO) << "skipped " << song.set << ": " << why;
                continue;
            }
            ChunkItem item{std::string(song.set) + " " + song.name, set, encode_replay(inputs_for(song).script), {}, 0};
            on_worker_thread([&] {
                Core core(set->rom.sound, set->words);
                Collect sink;
                core.set_pcm_sink(&sink);
                replay(core, item.script);
                for (uint64_t t = TIMER_TICKS; t <= CHUNK_TOTAL; t += TIMER_TICKS) core.advance(t);
                item.reference = std::move(sink.pcm);
            });
            Hash hash;
            hash.write(item.reference.data(), item.reference.size() / 2);
            item.reference_hash = hash.value;
            list.push_back(std::move(item));
        }
        return list;
    }();
    return items;
}

// Runs `property` (a callable returning void, using rc::gen and RC_ASSERT) for 24 cases and fails the test with the
// RapidCheck report otherwise.
template <class F> void check_property(const char *name, F &&property) {
    auto params = rc::detail::configuration().testParams;
    params.maxSuccess = 24;
    params.disableShrinking = true; // every case renders seconds of audio
    rc::detail::TestMetadata metadata;
    metadata.id = metadata.description = name;
    const auto result = rc::detail::checkTestable(std::forward<F>(property), metadata, params);
    if (!result.template is<rc::detail::SuccessResult>()) {
        std::ostringstream message;
        rc::detail::printResultMessage(result, message);
        FAIL() << message.str();
    }
}

// Renders the item with `schedule(core)` driving the time, returns the PCM.
template <class F> std::vector<float> render_with(const ChunkItem &item, F &&schedule) {
    std::vector<float> pcm;
    on_worker_thread([&] {
        Core core(item.set->rom.sound, item.set->words);
        Collect sink;
        core.set_pcm_sink(&sink);
        replay(core, item.script);
        schedule(core);
        pcm = std::move(sink.pcm);
    });
    return pcm;
}

TEST(HleCore, GridAlignedStepsAreBitExact) {
    const auto &items = chunk_items();
    if (items.empty()) GTEST_SKIP() << "no ROM set available";
    for (const auto &item : items) EXPECT_EQ(item.reference.size() / 2, Core::frames_at(CHUNK_TOTAL)) << item.name;
    check_property("HleCore/GridAlignedStepsAreBitExact", [&] {
        const auto &item = items[*rc::gen::inRange<size_t>(0, items.size())];
        const auto steps = *rc::gen::container<std::vector<uint32_t>>(rc::gen::inRange<uint32_t>(1, 200)); // in ms
        const uint32_t quantum = *rc::gen::inRange<uint32_t>(1, 2000);                                    // in ms
        const auto pcm = render_with(item, [&](Core &core) {
            uint64_t t = 0;
            for (const uint32_t step : steps) core.advance(t = std::min(CHUNK_TOTAL, t + uint64_t(step) * TIMER_TICKS));
            while (t < CHUNK_TOTAL) core.advance(t = std::min(CHUNK_TOTAL, t + uint64_t(quantum) * TIMER_TICKS));
        });
        Hash hash;
        hash.write(pcm.data(), pcm.size() / 2);
        RC_ASSERT(hash.frames == item.reference.size() / 2);
        RC_ASSERT(hash.value == item.reference_hash);
    });
}

TEST(HleCore, ArbitraryStepsAgreeWithinFloatNoise) {
    const auto &items = chunk_items();
    if (items.empty()) GTEST_SKIP() << "no ROM set available";
    check_property("HleCore/ArbitraryStepsAgreeWithinFloatNoise", [&] {
        const auto &item = items[*rc::gen::inRange<size_t>(0, items.size())];
        const auto steps = *rc::gen::container<std::vector<uint32_t>>(rc::gen::oneOf(
            rc::gen::inRange<uint32_t>(1, 64), rc::gen::inRange<uint32_t>(64, 5000),
            rc::gen::inRange<uint32_t>(5000, 200000)));
        const uint32_t quantum = *rc::gen::inRange<uint32_t>(64, 100000);
        const auto pcm = render_with(item, [&](Core &core) {
            uint64_t t = 0;
            for (const uint32_t step : steps) core.advance(t = std::min(CHUNK_TOTAL, t + step));
            while (t < CHUNK_TOTAL) core.advance(t = std::min(CHUNK_TOTAL, t + quantum));
        });
        RC_ASSERT(pcm.size() == item.reference.size());
        float worst = 0;
        for (size_t i = 0; i < pcm.size(); ++i) worst = std::max(worst, std::abs(pcm[i] - item.reference[i]));
        RC_LOG() << item.name << ": worst |difference| " << worst;
        RC_ASSERT(worst <= 1e-6f);
    });
}
