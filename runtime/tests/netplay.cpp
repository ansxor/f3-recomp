#include "f3rt/netplay.hpp"
#include "netplay/shared/contract.hpp"
#include "netplay/recorder.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace f3rt::netplay;

// 1. Binary Token Response Encoding and Parsing
TEST(NetplayContract, TokenResponseParseValidAndInvalid) {
    uint64_t client_id = 0x0123456789ABCDEFULL;
    std::array<uint8_t, yojimbo::ConnectTokenBytes> token{};
    for (size_t i = 0; i < token.size(); ++i) {
        token[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // Valid 2056-byte response
    auto encoded = encode_token_response(client_id, token);
    ASSERT_EQ(encoded.size(), TOKEN_RESPONSE_SIZE);

    uint64_t parsed_cid = 0;
    std::array<uint8_t, yojimbo::ConnectTokenBytes> parsed_token{};
    EXPECT_TRUE(parse_token_response(encoded, parsed_cid, parsed_token));
    EXPECT_EQ(parsed_cid, client_id);
    EXPECT_EQ(parsed_token, token);

    // Truncated responses must be rejected
    std::vector<uint8_t> empty_buf;
    EXPECT_FALSE(parse_token_response(empty_buf, parsed_cid, parsed_token));

    std::vector<uint8_t> short_buf(8, 0); // only client_id, missing token
    EXPECT_FALSE(parse_token_response(short_buf, parsed_cid, parsed_token));

    std::vector<uint8_t> one_less(TOKEN_RESPONSE_SIZE - 1, 0x55);
    EXPECT_FALSE(parse_token_response(one_less, parsed_cid, parsed_token));

    // Oversized response must be rejected
    std::vector<uint8_t> one_more(TOKEN_RESPONSE_SIZE + 1, 0x55);
    EXPECT_FALSE(parse_token_response(one_more, parsed_cid, parsed_token));
}

// 2. Chat, Room Code, and Player Name Validation
TEST(NetplayContract, ValidationHelpers) {
    // Chat validation: 1..256 bytes, no control chars (< 0x20 or == 0x7F)
    EXPECT_FALSE(validate_chat_text("")); // empty rejected
    EXPECT_TRUE(validate_chat_text("Hello, netplay!"));
    std::string max_chat(256, 'a');
    EXPECT_TRUE(validate_chat_text(max_chat));
    std::string too_long_chat(257, 'a');
    EXPECT_FALSE(validate_chat_text(too_long_chat));

    // Control characters rejected
    EXPECT_FALSE(validate_chat_text("Hello\nworld"));
    EXPECT_FALSE(validate_chat_text("Hello\rworld"));
    EXPECT_FALSE(validate_chat_text("Hello\tworld"));
    std::string with_del = "Hello";
    with_del.push_back(0x7F);
    EXPECT_FALSE(validate_chat_text(with_del));

    // Room code validation: exactly 6 characters from alphabet ABCDEFGHJKLMNPQRSTUVWXYZ23456789
    EXPECT_FALSE(validate_room_code(""));
    EXPECT_FALSE(validate_room_code("ABCDE"));  // 5 chars
    EXPECT_FALSE(validate_room_code("ABCDEFG")); // 7 chars
    EXPECT_TRUE(validate_room_code("ABCDEF"));
    EXPECT_TRUE(validate_room_code("XYZ234"));

    // Excluded characters (0, 1, I, O, lowercase, symbols)
    EXPECT_FALSE(validate_room_code("ABCD0F")); // '0' excluded
    EXPECT_FALSE(validate_room_code("ABCD1F")); // '1' excluded
    EXPECT_FALSE(validate_room_code("ABCDIF")); // 'I' excluded
    EXPECT_FALSE(validate_room_code("ABCDOF")); // 'O' excluded
    EXPECT_FALSE(validate_room_code("abcdef")); // lowercase rejected
    EXPECT_FALSE(validate_room_code("ABC-EF")); // symbol rejected

    // Player name validation: 1..24 bytes, printable
    EXPECT_FALSE(validate_player_name(""));
    EXPECT_TRUE(validate_player_name("Player1"));
    std::string max_name(24, 'x');
    EXPECT_TRUE(validate_player_name(max_name));
    std::string too_long_name(25, 'x');
    EXPECT_FALSE(validate_player_name(too_long_name));
    EXPECT_FALSE(validate_player_name("Name\n"));
}

// 3. Message Serialization Round-Trip and Bounds Enforcement
TEST(NetplayContract, MessageSerializationRoundTripAndBounds) {
    // InputsMessage: valid round-trip
    {
        InputsMessage inps;
        inps.first_frame = 42;
        inps.count = 4;
        inps.inputs[0] = 0x1111;
        inps.inputs[1] = 0x2222;
        inps.inputs[2] = 0x3333;
        inps.inputs[3] = 0x4444;

        uint8_t buffer[256]{};
        yojimbo::WriteStream write_stream(buffer, sizeof(buffer));
        EXPECT_TRUE(inps.Serialize(write_stream));
        write_stream.Flush();

        InputsMessage read_inps;
        yojimbo::ReadStream read_stream(buffer, write_stream.GetBytesProcessed());
        EXPECT_TRUE(read_inps.Serialize(read_stream));
        EXPECT_EQ(read_inps.first_frame, 42u);
        EXPECT_EQ(read_inps.count, 4);
        EXPECT_EQ(read_inps.inputs[0], 0x1111);
        EXPECT_EQ(read_inps.inputs[1], 0x2222);
        EXPECT_EQ(read_inps.inputs[2], 0x3333);
        EXPECT_EQ(read_inps.inputs[3], 0x4444);
    }

    // InputsMessage: count bounds rejected (count < 1 or count > 32)
    {
        InputsMessage invalid_zero;
        invalid_zero.count = 0;
        uint8_t buf[256]{};
        yojimbo::WriteStream write_stream(buf, sizeof(buf));
        EXPECT_FALSE(invalid_zero.Serialize(write_stream)); // count 0 rejected by serialize_int(1, 32)
    }
    {
        InputsMessage invalid_33;
        invalid_33.count = 33;
        uint8_t buf[256]{};
        yojimbo::WriteStream write_stream(buf, sizeof(buf));
        EXPECT_FALSE(invalid_33.Serialize(write_stream)); // count 33 rejected by serialize_int(1, 32)
    }

    // CreateRoomMessage: input_delay bounds (1..8)
    {
        CreateRoomMessage cr;
        cr.input_delay = 4;
        cr.flags = 1;
        uint8_t buf[64]{};
        yojimbo::WriteStream write_stream(buf, sizeof(buf));
        EXPECT_TRUE(cr.Serialize(write_stream));
        write_stream.Flush();

        CreateRoomMessage read_cr;
        yojimbo::ReadStream read_stream(buf, write_stream.GetBytesProcessed());
        EXPECT_TRUE(read_cr.Serialize(read_stream));
        EXPECT_EQ(read_cr.input_delay, 4);
        EXPECT_EQ(read_cr.flags, 1);
    }
    {
        CreateRoomMessage invalid_cr;
        invalid_cr.input_delay = 0; // below min (1)
        uint8_t buf[64]{};
        yojimbo::WriteStream write_stream(buf, sizeof(buf));
        EXPECT_FALSE(invalid_cr.Serialize(write_stream));
    }
    {
        CreateRoomMessage invalid_cr;
        invalid_cr.input_delay = 9; // above max (8)
        uint8_t buf[64]{};
        yojimbo::WriteStream write_stream(buf, sizeof(buf));
        EXPECT_FALSE(invalid_cr.Serialize(write_stream));
    }

    // HelloMessage round-trip
    {
        HelloMessage hello;
        std::strncpy(hello.game_id, "testgame", sizeof(hello.game_id) - 1);
        hello.rom_crcs[0] = 0x12345678;
        hello.rom_crcs[6] = 0x87654321;
        std::memset(hello.build_hash, 0xBE, sizeof(hello.build_hash));
        std::strncpy(hello.player_name, "Alice", sizeof(hello.player_name) - 1);

        uint8_t buf[512]{};
        yojimbo::WriteStream write_stream(buf, sizeof(buf));
        EXPECT_TRUE(hello.Serialize(write_stream));
        write_stream.Flush();

        HelloMessage read_hello;
        yojimbo::ReadStream read_stream(buf, write_stream.GetBytesProcessed());
        EXPECT_TRUE(read_hello.Serialize(read_stream));
        EXPECT_STREQ(read_hello.game_id, "testgame");
        EXPECT_EQ(read_hello.rom_crcs[0], 0x12345678u);
        EXPECT_EQ(read_hello.rom_crcs[6], 0x87654321u);
        EXPECT_EQ(read_hello.build_hash[0], 0xBE);
        EXPECT_STREQ(read_hello.player_name, "Alice");
    }

    // MatchStartMessage round-trip
    {
        MatchStartMessage ms;
        ms.session_id = 0x9988776655443322ULL;
        ms.seed = 0xAABBCCDDEEFF0011ULL;
        ms.player_index = 1;
        ms.input_delay = 3;
        std::strncpy(ms.peer_name, "Bob", sizeof(ms.peer_name) - 1);

        uint8_t buf[256]{};
        yojimbo::WriteStream write_stream(buf, sizeof(buf));
        EXPECT_TRUE(ms.Serialize(write_stream));
        write_stream.Flush();

        MatchStartMessage read_ms;
        yojimbo::ReadStream read_stream(buf, write_stream.GetBytesProcessed());
        EXPECT_TRUE(read_ms.Serialize(read_stream));
        EXPECT_EQ(read_ms.session_id, 0x9988776655443322ULL);
        EXPECT_EQ(read_ms.seed, 0xAABBCCDDEEFF0011ULL);
        EXPECT_EQ(read_ms.player_index, 1);
        EXPECT_EQ(read_ms.input_delay, 3);
        EXPECT_STREQ(read_ms.peer_name, "Bob");
    }

    // ChatTextMessage round-trip
    {
        ChatTextMessage chat;
        std::strncpy(chat.text, "gg wp!", sizeof(chat.text) - 1);

        uint8_t buf[512]{};
        yojimbo::WriteStream write_stream(buf, sizeof(buf));
        EXPECT_TRUE(chat.Serialize(write_stream));
        write_stream.Flush();

        ChatTextMessage read_chat;
        yojimbo::ReadStream read_stream(buf, write_stream.GetBytesProcessed());
        EXPECT_TRUE(read_chat.Serialize(read_stream));
        EXPECT_STREQ(read_chat.text, "gg wp!");
    }
}

// 4. Client Input Window Calculation Logic
TEST(NetplaySession, InputWindowCalculation) {
    auto compute_window = [](uint32_t current_frame, uint8_t input_delay) {
        uint32_t max_sched = current_frame + input_delay;
        uint32_t count = std::min(32u, unsigned(input_delay + 16));
        uint32_t first_frame = 0;
        if (max_sched + 1 <= count) {
            first_frame = 0;
            count = max_sched + 1;
        } else {
            first_frame = (max_sched + 1) - count;
        }
        return std::make_pair(first_frame, count);
    };

    // Frame 0 with delay 2: window covers frames [0..2], count 3
    auto w0 = compute_window(0, 2);
    EXPECT_EQ(w0.first, 0u);
    EXPECT_EQ(w0.second, 3u);

    // Frame 10 with delay 2: count is min(32, 18) = 18, max_sched = 12, covers [0..12], count 13
    auto w10 = compute_window(10, 2);
    EXPECT_EQ(w10.first, 0u);
    EXPECT_EQ(w10.second, 13u);

    // Frame 100 with delay 2: max_sched = 102, count = 18, first_frame = 103 - 18 = 85
    auto w100 = compute_window(100, 2);
    EXPECT_EQ(w100.first, 85u);
    EXPECT_EQ(w100.second, 18u);
    EXPECT_LE(w100.second, 32u);

    // High input delay (8): count is min(32, 24) = 24
    auto w_delay8 = compute_window(200, 8);
    EXPECT_LE(w_delay8.second, 32u);
    EXPECT_EQ(w_delay8.first + w_delay8.second - 1, 208u);
}

// 5. Desync bundle inputs.txt round trip
TEST(NetplayRecorder, InputsTxtRoundTrip) {
    MatchMeta meta;
    meta.trigger_machine_frame = 2065;
    meta.edge_machine_frame = meta.sync_machine_frame = 2070;
    meta.start_machine_frame = 2075;
    meta.lobby_hold_frames = 600;
    SessionFacts facts;
    facts.player_index = 1;
    facts.input_delay = 2;
    facts.seed = 0xfedcba9876543210ULL;
    std::vector<AppliedInputs> frames;
    for (uint32_t i = 0; i < 300; ++i) frames.push_back({f3rt::LocalInputWord(i * 7), f3rt::LocalInputWord(0xffff - i)});

    InputsFile parsed;
    std::string error;
    ASSERT_TRUE(parse_inputs_txt(format_inputs_txt(meta, facts, frames), parsed, error)) << error;
    EXPECT_EQ(parsed.frames, frames);
    EXPECT_EQ(header_value(parsed, "seed"), std::to_string(facts.seed));
    EXPECT_EQ(header_value(parsed, "frames_between_sync_and_start"), "5");
    EXPECT_EQ(header_value(parsed, "lobby_hold_frames"), "600");

    EXPECT_FALSE(parse_inputs_txt("0 0000 0000\n2 0000 0000\n", parsed, error)); // frame gap rejected
    const BundleInfo flat = parse_info_json("{\"a\": 1, \"frame\": 1740}");
    ASSERT_TRUE(flat.frame.has_value());
    EXPECT_EQ(*flat.frame, 1740u);
}

TEST(NetplayRecorder, InfoJsonReadsBundleFields) {
    const std::string json = R"({
  "bundle_version": 2,
  "hash_layout": 2,
  "machine_gpu": 1,
  "audio_backend": "Enhanced \"x\"",
  "seed": 12345678901234,
  "frame": 1740,
  "crc_interval": 60,
  "hashed_len": 496,
  "recorded_crc": 305419896,
  "player_index": 1,
  "state_note": "use `f3rt-tool netplay-replay --bundle <dir>`",
  "hashed_frame": -1,
  "reports": [{"frame": 60, "crc": 1, "phase": 0, "hashed_len": 496}]
})";
    const BundleInfo info = parse_info_json(json);
    ASSERT_TRUE(info.seed.has_value());
    EXPECT_EQ(*info.seed, 12345678901234ull);
    EXPECT_EQ(info.frame.value_or(0), 1740u);
    EXPECT_EQ(info.crc_interval.value_or(0), 60u);
    EXPECT_EQ(info.hashed_len.value_or(0), 496u);
    EXPECT_EQ(info.recorded_crc.value_or(0), 305419896u);
    EXPECT_EQ(info.player_index.value_or(0), 1u);
    EXPECT_EQ(info.machine_gpu.value_or(0), 1u);
    EXPECT_EQ(info.hash_layout.value_or(0), 2u);
    ASSERT_TRUE(info.audio_backend.has_value());
    EXPECT_EQ(*info.audio_backend, "Enhanced \"x\"");

    EXPECT_FALSE(parse_info_json("{\"seed\": \"12\"}").seed.has_value()); // wrong JSON type stays absent
    EXPECT_FALSE(parse_info_json("{}").seed.has_value());
    EXPECT_THROW(parse_info_json("[1]"), std::runtime_error);
}
