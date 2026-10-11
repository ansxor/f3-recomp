#include "state_io.hpp"
#include "third_party/musashi/m68k.h"
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <cstring>
#include <limits>
#include <vector>

namespace {

template <typename T>
void expect_rejected(const T &value, const char *name) {
    f3rt::StateReader reader({reinterpret_cast<const uint8_t *>(&value), sizeof(value)});
    T restored{};
    EXPECT_THROW(reader.read(restored), std::invalid_argument)
        << "ACCEPTED unsafe snapshot: " << name;
}

template <typename T>
void expect_round_trip(const T &value) {
    std::vector<uint8_t> buffer(sizeof(T));
    f3rt::StateWriter writer(buffer);
    writer.write(value);
    f3rt::StateReader reader(buffer);
    T restored{};
    reader.read(restored);
    EXPECT_EQ(std::memcmp(&value, &restored, sizeof(T)), 0);
}

class StateValidationTest : public ::testing::Test {
protected:
    void SetUp() override {
        m68k_init();
        m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    }
};

TEST_F(StateValidationTest, MusashiSoundContextUnsafeFieldsRejected) {
    std::vector<uint8_t> context(m68k_context_size());
    m68k_get_context(context.data());
    f3rt_sound_oracle_state sound{};
    f3rt_sound_core_export(context.data(), &sound);

    // Baseline clean export round-trips successfully.
    expect_round_trip(sound);

    auto bad = sound;
    bad.s_flag = 0x40000000; bad.nmi_pending = 1;
    expect_rejected(bad, "sound supervisor stack index");

    bad = sound; bad.m_flag = 0x40000000;
    expect_rejected(bad, "sound master stack index");

    bad = sound; bad.ir = 0x10000;
    expect_rejected(bad, "sound opcode table index");

    bad = sound; bad.int_level = 0xffffffff;
    expect_rejected(bad, "sound interrupt level");

    bad = sound; bad.cpu_type = 0x80000000;
    expect_rejected(bad, "sound CPU model");

    bad = sound; bad.cyc_movem_w = 0xffffffff;
    expect_rejected(bad, "sound instruction timing");
}

TEST_F(StateValidationTest, AudioSchedulerSnapshotsRejected) {
    f3rt::CanonicalAudioCore audio{};
    audio.sample_accum = 16000000;
    expect_rejected(audio, "sample deadline outside fractional range");

    audio.sample_accum = uint64_t(1) << 63;
    expect_rejected(audio, "unbounded sample generation");

    audio.sample_accum = 0; audio.cpu_accum = std::numeric_limits<int64_t>::min();
    expect_rejected(audio, "sound deadline arithmetic underflow");

    audio.cpu_accum = std::numeric_limits<int64_t>::max();
    expect_rejected(audio, "sound deadline arithmetic overflow");
}

TEST_F(StateValidationTest, ES5505ClocksRejected) {
    f3rt::CanonicalES5505 otis{};
    otis.master_clock = 15238090; otis.active_voices = 31;
    expect_rejected(otis, "zero OTIS rate");

    otis.sample_rate = 1;
    expect_rejected(otis, "OTIS crystal/divider disagreement");
}

// RapidCheck property: Valid CanonicalAudioCore state round-trips through StateWriter / StateReader.
RC_GTEST_PROP(State, AudioCoreRoundTrip, ()) {
    f3rt::CanonicalAudioCore audio{};
    audio.sample_accum = *rc::gen::inRange<uint64_t>(0, 16000000);
    audio.duart_accum = *rc::gen::inRange<uint64_t>(0, 4);
    audio.rb_count = *rc::gen::inRange<uint32_t>(0, 32769);
    audio.gain_model = *rc::gen::inRange<uint8_t>(0, 2);
    audio.reset_asserted = *rc::gen::inRange<uint8_t>(0, 2);
    audio.esp_halted = *rc::gen::inRange<uint8_t>(0, 2);
    audio.cpu_accum = *rc::gen::inRange<int64_t>(-16000000LL * 1024, 16000000LL);
    audio.bank_mask = *rc::gen::arbitrary<uint32_t>();

    std::vector<uint8_t> buffer(sizeof(audio));
    f3rt::StateWriter writer(buffer);
    writer.write(audio);

    f3rt::StateReader reader(buffer);
    f3rt::CanonicalAudioCore restored{};
    reader.read(restored);

    RC_ASSERT(restored.sample_accum == audio.sample_accum);
    RC_ASSERT(restored.duart_accum == audio.duart_accum);
    RC_ASSERT(restored.rb_count == audio.rb_count);
    RC_ASSERT(restored.gain_model == audio.gain_model);
    RC_ASSERT(restored.reset_asserted == audio.reset_asserted);
    RC_ASSERT(restored.esp_halted == audio.esp_halted);
    RC_ASSERT(restored.cpu_accum == audio.cpu_accum);
    RC_ASSERT(restored.bank_mask == audio.bank_mask);
}

// RapidCheck property: Invalid sample_accum outside fractional range is rejected.
RC_GTEST_PROP(State, AudioCoreInvalidSampleAccumRejected, ()) {
    f3rt::CanonicalAudioCore audio{};
    audio.sample_accum = *rc::gen::inRange<uint64_t>(16000000, 16000000ULL + 1000000);

    f3rt::StateReader reader({reinterpret_cast<const uint8_t *>(&audio), sizeof(audio)});
    f3rt::CanonicalAudioCore restored{};
    bool rejected = false;
    try {
        reader.read(restored);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    RC_ASSERT(rejected);
}

// RapidCheck property: Valid CanonicalES5505 clock and voice parameters round-trip.
RC_GTEST_PROP(State, ES5505RoundTrip, ()) {
    f3rt::CanonicalES5505 otis{};
    otis.active_voices = *rc::gen::inRange<uint8_t>(0, 32);
    otis.current_page = *rc::gen::inRange<uint8_t>(0, 128);
    otis.voice_index = *rc::gen::inRange<int8_t>(0, 32);
    otis.master_clock = *rc::gen::inRange<uint32_t>(1000000, 30000000);
    otis.sample_rate = otis.master_clock / (16 * (unsigned(otis.active_voices) + 1));
    for (int i = 0; i < 32; ++i) {
        otis.voices[i].index = i;
    }

    std::vector<uint8_t> buffer(sizeof(otis));
    f3rt::StateWriter writer(buffer);
    writer.write(otis);

    f3rt::StateReader reader(buffer);
    f3rt::CanonicalES5505 restored{};
    reader.read(restored);

    RC_ASSERT(restored.active_voices == otis.active_voices);
    RC_ASSERT(restored.current_page == otis.current_page);
    RC_ASSERT(restored.voice_index == otis.voice_index);
    RC_ASSERT(restored.master_clock == otis.master_clock);
    RC_ASSERT(restored.sample_rate == otis.sample_rate);
}

// RapidCheck property: ES5505 crystal/divider disagreement is rejected.
RC_GTEST_PROP(State, ES5505SampleRateDisagreementRejected, ()) {
    f3rt::CanonicalES5505 otis{};
    otis.active_voices = *rc::gen::inRange<uint8_t>(0, 32);
    otis.current_page = 0;
    otis.voice_index = 0;
    otis.master_clock = *rc::gen::inRange<uint32_t>(1000000, 30000000);
    const uint32_t correct_rate = otis.master_clock / (16 * (unsigned(otis.active_voices) + 1));
    const uint32_t delta = *rc::gen::inRange<uint32_t>(1, 1000);
    otis.sample_rate = correct_rate + delta;
    for (int i = 0; i < 32; ++i) {
        otis.voices[i].index = i;
    }

    f3rt::StateReader reader({reinterpret_cast<const uint8_t *>(&otis), sizeof(otis)});
    f3rt::CanonicalES5505 restored{};
    bool rejected = false;
    try {
        reader.read(restored);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    RC_ASSERT(rejected);
}

} // namespace
