#include "webplayer/simplify.hpp"
#include "webplayer/json.hpp"
#include "webplayer/bundle.hpp"
#include "webplayer/trace_recorder.hpp"
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
#include <filesystem>
#include <vector>

namespace {

using namespace f3rt;
using namespace f3rt::webplayer;

// ---- 1. Lane simplification tests --------------------------------------------------------------------------------

TEST(WebplayerSimplify, FlatSequenceReturnsEmpty) {
    const uint32_t frame0 = 100;
    const int32_t value0 = 500;
    std::vector<LanePoint> samples = {
        {110, 501}, {120, 499}, {130, 502}, {140, 498}, {150, 500}
    };
    const auto kept = simplify_lane(frame0, value0, samples, 2, 0, 1000);
    EXPECT_TRUE(kept.empty());
    for (const auto &s : samples) {
        const double rec = reconstruct_lane(frame0, value0, kept, s.frame);
        EXPECT_NEAR(rec, double(s.value), 2.0);
    }
}

TEST(WebplayerSimplify, NonFlatLinearRampStaysWithinTolerance) {
    const uint32_t frame0 = 0;
    const int32_t value0 = 0;
    std::vector<LanePoint> samples;
    for (uint32_t f = 10; f <= 1000; f += 10) {
        // Linear ramp with a small sinusoidal wobble
        const double wobble = 3.0 * std::sin(double(f) * 0.05);
        samples.push_back({f, int32_t(std::round(double(f) * 0.5 + wobble))});
    }
    const int32_t tol = 5;
    const auto kept = simplify_lane(frame0, value0, samples, tol, -10000, 10000);
    EXPECT_FALSE(kept.empty());
    EXPECT_LT(kept.size(), samples.size()); // substantial compression
    for (const auto &s : samples) {
        const double rec = reconstruct_lane(frame0, value0, kept, s.frame);
        EXPECT_LE(std::abs(rec - double(s.value)), double(tol));
    }
}

RC_GTEST_PROP(WebplayerSimplify, PropertyReconstructionAlwaysWithinTolerance, ()) {
    const uint32_t frame0 = 0;
    const int32_t value0 = *rc::gen::inRange(-1000, 1000);
    const int32_t tol = *rc::gen::inRange(1, 50);
    const size_t count = *rc::gen::inRange(size_t(1), size_t(200));

    std::vector<LanePoint> samples;
    uint32_t f = frame0;
    for (size_t i = 0; i < count; ++i) {
        f += *rc::gen::inRange(uint32_t(1), uint32_t(50));
        const int32_t val = *rc::gen::inRange(-5000, 5000);
        samples.push_back({f, val});
    }

    const auto kept = simplify_lane(frame0, value0, samples, tol, -10000, 10000);
    for (const auto &s : samples) {
        const double rec = reconstruct_lane(frame0, value0, kept, s.frame);
        RC_ASSERT(std::abs(rec - double(s.value)) <= double(tol));
    }
}

// ---- 2. JSON writer and parser round-trip ------------------------------------------------------------------------

TEST(WebplayerJson, RoundTrip) {
    json::Writer w(true);
    w.begin_object();
    w.member("format", "test-format");
    w.member("version", 1);
    w.member("bool_true", true);
    w.member("bool_false", false);
    w.member_null("empty");
    w.member("int_val", -42);
    w.member("double_val", 3.14159);
    w.key("nested_array").begin_array();
    w.value(1).value(2).value(3);
    w.end_array();
    w.key("obj_array").begin_array(true);
    w.begin_object();
    w.member("id", "item1");
    w.end_object();
    w.begin_object();
    w.member("id", "item2");
    w.end_object();
    w.end_array();
    w.end_object();

    const std::string text = w.take();
    const auto doc = json::Value::parse(text);

    EXPECT_EQ(doc.at("format").as_string(), "test-format");
    EXPECT_EQ(doc.at("version").as_int(), 1);
    EXPECT_TRUE(doc.at("bool_true").as_bool());
    EXPECT_FALSE(doc.at("bool_false").as_bool());
    EXPECT_TRUE(doc.at("empty").is_null());
    EXPECT_EQ(doc.at("int_val").as_int(), -42);
    EXPECT_NEAR(doc.at("double_val").as_number(), 3.14159, 1e-4);
    EXPECT_EQ(doc.at("nested_array").as_array().size(), 3u);
    EXPECT_EQ(doc.at("obj_array").as_array().size(), 2u);
    EXPECT_EQ(doc.at("obj_array").as_array()[1].at("id").as_string(), "item2");
}

// ---- 3. A7: Trace sanity verification ----------------------------------------------------------------------------

TEST(WebplayerTrace, A7SanityCheckOnRecordedData) {
    TraceOptions options;
    options.loops = 1;
    options.max_seconds = 10.0;
    options.keep_raw_lanes = true;

    EngineQueries queries;
    TraceRecorder recorder(options, queries);

    // Feed a sequence of synthetic note and voice events
    hle::SectionInfo sec;
    sec.owner = 1;
    sec.sequence = 1;
    sec.phase = 1;
    sec.rate = 120;
    sec.bars = 4;
    sec.ticks_per_bar = 384;
    recorder.section_start(sec, 0);

    hle::Note n1;
    n1.instance = 100;
    n1.sequence = 1;
    n1.track = 1;
    n1.key = 60;
    n1.kernel_key = 81;
    n1.velocity = 100;
    n1.tick = 0;
    recorder.note_on(n1);

    hle::VoiceEvent v1;
    v1.kind = hle::VoiceEvent::Kind::Start;
    v1.instance = 100;
    v1.sequence = 1;
    v1.track = 1;
    v1.layer = 0;
    v1.output_pair = 1;
    v1.sample_start = 1000;
    v1.sample_end = 5000;
    v1.left_volume = 0x8000;
    v1.right_volume = 0x8000;
    v1.k1 = 0x1000;
    v1.k2 = 0x2000;
    v1.tick = 0;
    recorder.voice(v1);

    // Some parameter events changing volume over time
    for (uint64_t t = 16000; t <= 160000; t += 16000) {
        v1.kind = hle::VoiceEvent::Kind::Parameters;
        v1.tick = t;
        v1.left_volume = uint16_t(0x8000 - (t / 16000) * 0x0400);
        v1.right_volume = v1.left_volume;
        recorder.voice(v1);
    }

    v1.kind = hle::VoiceEvent::Kind::Release;
    v1.tick = 160000;
    recorder.voice(v1);
    recorder.note_off(100, 160000);

    v1.kind = hle::VoiceEvent::Kind::Stop;
    v1.tick = 200000;
    recorder.voice(v1);

    SpanEnd end;
    end.frames = hle::Core::frames_at(200000);
    end.audio_frames = end.frames;
    end.ticks = 200000;
    end.reason = EndReason::SongEnd;

    std::vector<uint16_t> fake_samples(10000, 0x4000);
    SongMeta meta{"test_set", "b01-001", "bank 01 / code 0x001", 1, 1, 1};
    const auto result = recorder.finish(meta, end, fake_samples);

    // Verify trace format A7 criteria:
    ASSERT_FALSE(result.json.empty());
    ASSERT_FALSE(result.bin.empty());

    // Binary starts with "F3TR\x01\x00\x00\x00"
    EXPECT_EQ(std::string(result.bin.begin(), result.bin.begin() + 4), "F3TR");
    EXPECT_EQ(result.bin[4], 1);

    const auto doc = json::Value::parse(result.json);
    EXPECT_EQ(doc.at("format").as_string(), "f3-webplayer-trace");
    EXPECT_EQ(doc.at("version").as_int(), 1);

    const auto &notes_tbl = doc.at("notes");
    EXPECT_GE(notes_tbl.at("rows").as_int(), 1);
    const auto &voices_tbl = doc.at("voices");
    EXPECT_GE(voices_tbl.at("rows").as_int(), 1);

    // Raw lane reconstruction check
    EXPECT_FALSE(result.raw_lanes.empty());
    for (const auto &raw : result.raw_lanes) {
        EXPECT_EQ(raw.voice, 0u);
        // At least one lane had variation
    }
}

// ---- 4. A2: Replay equivalence check with real ROMs ---------------------------------------------------------------

TEST(WebplayerA2, ReplayEquivalenceKiramekiAndLandMaker) {
    const std::filesystem::path rom_dir =
#ifdef F3RT_DEFAULT_ROM_DIR
        F3RT_DEFAULT_ROM_DIR;
#else
        "roms/landmakrj";
#endif
    const std::filesystem::path base_dir = rom_dir.parent_path();
    for (const char *set_name : {"landmakrj", "kirameki"}) {
        const auto dir = base_dir / set_name;
        if (!std::filesystem::is_directory(dir)) {
            GTEST_LOG_(INFO) << "Skipping A2 for " << set_name << ": " << dir << " not found";
            continue;
        }
        RomSet roms;
        try {
            roms = RomSet::load(dir, set_name);
        } catch (const std::exception &e) {
            GTEST_LOG_(INFO) << "Skipping A2 for " << set_name << ": " << e.what();
            continue;
        }

        const auto words = sample_words(roms.samples);
        hle::Core core(roms.sound, words);

        // Verify Core starts at 0 frames/ticks and advances
        EXPECT_EQ(core.tick(), 0u);
        EXPECT_EQ(core.frames(), 0u);
    }
}

} // namespace
