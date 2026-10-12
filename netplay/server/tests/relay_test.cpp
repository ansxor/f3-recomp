#include "netplay/server/log_writer.hpp"
#include "netplay/server/relay.hpp"
#include "netplay/server/state.hpp"
#include "netplay/server/token_service.hpp"
#include "netplay/shared/contract.hpp"
#include "httplib.h"
#include "cpp_yyjson.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace f3rt::netplay {
namespace {

Identity make_test_identity(std::string_view gid = "testgame") {
    Identity id;
    id.game_id = std::string(gid);
    id.rom_crcs = {10, 20, 30, 40, 50, 60, 70};
    id.build_hash.fill(0xAA);
    return id;
}

// Helper fixture for state machine testing
class StateMachineFixture : public ::testing::Test {
protected:
    RelayStateMachine m_sm{32};
    std::vector<Action> m_actions;
    double m_current_time = 1000.0;

    void step_time(double dt) {
        m_current_time += dt;
        m_actions.clear();
        m_sm.tick(m_current_time, m_actions);
    }

    template <typename T> std::vector<T> filter_actions() {
        std::vector<T> result;
        for (const auto &act : m_actions) {
            if (std::holds_alternative<T>(act)) {
                result.push_back(std::get<T>(act));
            }
        }
        return result;
    }

    // Connects client 0 and client 1, runs Hello, CreateRoom, JoinRoom
    std::string setup_active_match(uint8_t input_delay = 2, uint8_t flags = 0) {
        m_actions.clear();
        m_sm.on_client_connected(0, m_current_time, m_actions);
        m_sm.on_client_connected(1, m_current_time, m_actions);

        auto id = make_test_identity();
        m_sm.on_hello(0, id.game_id, id.rom_crcs, id.build_hash, "alice", m_current_time,
                      m_actions);
        m_sm.on_hello(1, id.game_id, id.rom_crcs, id.build_hash, "bob", m_current_time, m_actions);

        m_actions.clear();
        m_sm.on_create_room(0, input_delay, flags, m_current_time, m_actions);
        auto cr = filter_actions<ActionSendRoomCreated>();
        EXPECT_EQ(cr.size(), 1u);
        std::string code = cr[0].room_code;

        m_actions.clear();
        m_sm.on_join_room(1, code, m_current_time, m_actions);
        auto ms = filter_actions<ActionSendMatchStart>();
        EXPECT_EQ(ms.size(), 2u);
        return code;
    }
};

} // namespace

// 1. MatchStartFields
TEST_F(StateMachineFixture, MatchStartFields) {
    setup_active_match(2, 0);
    auto ms = filter_actions<ActionSendMatchStart>();
    ASSERT_EQ(ms.size(), 2u);

    EXPECT_EQ(ms[0].slot, 0);
    EXPECT_EQ(ms[0].player_index, 0);
    EXPECT_EQ(ms[0].peer_name, "bob");
    EXPECT_EQ(ms[0].input_delay, 2);

    EXPECT_EQ(ms[1].slot, 1);
    EXPECT_EQ(ms[1].player_index, 1);
    EXPECT_EQ(ms[1].peer_name, "alice");
    EXPECT_EQ(ms[1].input_delay, 2);

    EXPECT_EQ(ms[0].session_id, ms[1].session_id);
    EXPECT_NE(ms[0].session_id, 0u);
    EXPECT_EQ(ms[0].seed, ms[1].seed);
}

// 2. IdentityMismatch
TEST_F(StateMachineFixture, IdentityMismatch) {
    auto id_host = make_test_identity("testgame");
    m_actions.clear();
    m_sm.on_client_connected(0, m_current_time, m_actions);
    m_sm.on_hello(0, id_host.game_id, id_host.rom_crcs, id_host.build_hash, "alice", m_current_time,
                  m_actions);
    m_sm.on_create_room(0, 2, 0, m_current_time, m_actions);

    // Mutate game_id
    {
        RelayStateMachine sm2{32};
        std::vector<Action> act;
        sm2.on_client_connected(0, m_current_time, act);
        sm2.on_client_connected(1, m_current_time, act);
        sm2.on_hello(0, id_host.game_id, id_host.rom_crcs, id_host.build_hash, "alice",
                     m_current_time, act);
        sm2.on_create_room(0, 2, 0, m_current_time, act);
        auto c = std::get<ActionSendRoomCreated>(act.back()).room_code;

        act.clear();
        auto bad = id_host;
        bad.game_id = "other";
        sm2.on_hello(1, bad.game_id, bad.rom_crcs, bad.build_hash, "bob", m_current_time, act);
        act.clear();
        sm2.on_join_room(1, c, m_current_time, act);

        bool found_error = false;
        for (const auto &a : act) {
            if (std::holds_alternative<ActionSendError>(a)) {
                EXPECT_EQ(std::get<ActionSendError>(a).code, NetplayErrorCode::IdentityMismatch);
                found_error = true;
            }
        }
        EXPECT_TRUE(found_error);
    }
}

// 3. InputsForwardedAndLogged
TEST_F(StateMachineFixture, InputsForwardedAndLogged) {
    setup_active_match();

    m_actions.clear();
    std::vector<uint16_t> inp = {5, 6};
    m_sm.on_inputs(0, 0, 2, inp, m_current_time, m_actions);

    auto fwd = filter_actions<ActionSendInputs>();
    ASSERT_EQ(fwd.size(), 1u);
    EXPECT_EQ(fwd[0].slot, 1);
    EXPECT_EQ(fwd[0].first_frame, 0u);
    EXPECT_EQ(fwd[0].count, 2);
    EXPECT_EQ(fwd[0].inputs, inp);

    // Conflicting re-send: frame 0 with input 9 instead of 5
    m_actions.clear();
    std::vector<uint16_t> conflict = {9};
    m_sm.on_inputs(0, 0, 1, conflict, m_current_time, m_actions);
    EXPECT_TRUE(filter_actions<ActionSendInputs>().empty());

    // Next valid packet is forwarded
    m_actions.clear();
    std::vector<uint16_t> next_inp = {7};
    m_sm.on_inputs(0, 2, 1, next_inp, m_current_time, m_actions);
    EXPECT_EQ(filter_actions<ActionSendInputs>().size(), 1u);

    // End match via Bye to inspect session snapshot
    m_actions.clear();
    m_sm.on_bye(0, m_current_time, m_actions);
    auto logs = filter_actions<ActionWriteLog>();
    ASSERT_EQ(logs.size(), 1u);
    const auto &snap = logs[0].snapshot;
    EXPECT_EQ(snap.end_reason, "bye");
    EXPECT_EQ(snap.inputs[0].size(), 3u);
    EXPECT_EQ(snap.inputs[0][0], 5);
    EXPECT_EQ(snap.inputs[0][1], 6);
    EXPECT_EQ(snap.inputs[0][2], 7);
    EXPECT_EQ(snap.protocol_errors.size(), 1u);
}

// 4. CRCDesyncOnce
TEST_F(StateMachineFixture, CRCDesyncOnce) {
    setup_active_match();

    m_actions.clear();
    m_sm.on_crc(0, 10, 1, m_current_time, m_actions);
    EXPECT_TRUE(filter_actions<ActionSendDesync>().empty());

    m_sm.on_crc(1, 10, 2, m_current_time, m_actions);
    auto desyncs = filter_actions<ActionSendDesync>();
    ASSERT_EQ(desyncs.size(), 2u); // Sent to both players
    EXPECT_EQ(desyncs[0].frame, 10u);
    EXPECT_EQ(desyncs[0].crc_p1, 1u);
    EXPECT_EQ(desyncs[0].crc_p2, 2u);

    // Second mismatch must NOT send another Desync
    m_actions.clear();
    m_sm.on_crc(0, 11, 3, m_current_time, m_actions);
    m_sm.on_crc(1, 11, 4, m_current_time, m_actions);
    EXPECT_TRUE(filter_actions<ActionSendDesync>().empty());
}

// 5. ResultAgree
TEST_F(StateMachineFixture, ResultAgree) {
    setup_active_match();

    m_actions.clear();
    m_sm.on_result(0, 100, 0, 0xABCD, m_current_time, m_actions);
    EXPECT_TRUE(filter_actions<ActionSendResultAck>().empty());

    m_sm.on_result(1, 100, 0, 0xABCD, m_current_time, m_actions);
    auto acks = filter_actions<ActionSendResultAck>();
    ASSERT_EQ(acks.size(), 2u);
    EXPECT_TRUE(acks[0].agreed);
    EXPECT_EQ(acks[0].winner, 0);
}

// 6. ResultDisagree
TEST_F(StateMachineFixture, ResultDisagree) {
    setup_active_match();

    m_actions.clear();
    m_sm.on_result(0, 100, 0, 0xABCD, m_current_time, m_actions);
    m_sm.on_result(1, 100, 1, 0xABCD, m_current_time, m_actions);
    auto acks = filter_actions<ActionSendResultAck>();
    ASSERT_EQ(acks.size(), 2u);
    EXPECT_FALSE(acks[0].agreed);
    EXPECT_EQ(acks[0].winner, 0xFE);
}

// 7. DisconnectPeerLeftAndLog
TEST_F(StateMachineFixture, DisconnectPeerLeftAndLog) {
    setup_active_match();

    m_actions.clear();
    m_sm.on_client_disconnected(0, "disconnect", m_current_time, m_actions);
    auto peer_left = filter_actions<ActionSendPeerLeft>();
    ASSERT_EQ(peer_left.size(), 1u);
    EXPECT_EQ(peer_left[0].slot, 1);

    auto logs = filter_actions<ActionWriteLog>();
    ASSERT_EQ(logs.size(), 1u);
    EXPECT_EQ(logs[0].snapshot.end_reason, "disconnect");
}

// 8. FirstMessageMustBeHello
TEST_F(StateMachineFixture, FirstMessageMustBeHello) {
    m_actions.clear();
    m_sm.on_client_connected(0, m_current_time, m_actions);
    m_sm.on_create_room(0, 2, 0, m_current_time, m_actions);

    auto errs = filter_actions<ActionSendError>();
    ASSERT_EQ(errs.size(), 1u);
    EXPECT_EQ(errs[0].code, NetplayErrorCode::NotInLobby);
}

// 9. WrongProtocolVersion (Tested at Token Endpoint)
TEST(TokenServiceTest, WrongProtocolVersion) {
    std::array<uint8_t, 32> priv{};
    TokenServer::Config cfg;
    cfg.port = 0; // Ephemeral
    TokenServer srv(cfg, priv);
    ASSERT_TRUE(srv.start());

    httplib::Client cli("127.0.0.1", srv.bound_port());
    // Missing query param
    auto res1 = cli.Get("/token");
    ASSERT_TRUE(res1);
    EXPECT_EQ(res1->status, 409);

    // Bad protocol version
    auto res2 = cli.Get("/token?v=999");
    ASSERT_TRUE(res2);
    EXPECT_EQ(res2->status, 409);

    // Correct version
    auto res3 = cli.Get("/token?v=3");
    ASSERT_TRUE(res3);
    EXPECT_EQ(res3->status, 200);
    EXPECT_EQ(res3->body.size(), TOKEN_RESPONSE_SIZE);
}

// 10. RoomNotFound
TEST_F(StateMachineFixture, RoomNotFound) {
    m_actions.clear();
    m_sm.on_client_connected(0, m_current_time, m_actions);
    auto id = make_test_identity();
    m_sm.on_hello(0, id.game_id, id.rom_crcs, id.build_hash, "bob", m_current_time, m_actions);
    m_actions.clear();
    m_sm.on_join_room(0, "ZZZZZZ", m_current_time, m_actions);

    auto errs = filter_actions<ActionSendError>();
    ASSERT_EQ(errs.size(), 1u);
    EXPECT_EQ(errs[0].code, NetplayErrorCode::RoomNotFound);
}

// 11. RoomFull
TEST_F(StateMachineFixture, RoomFull) {
    auto code = setup_active_match();

    m_actions.clear();
    m_sm.on_client_connected(2, m_current_time, m_actions);
    auto id = make_test_identity();
    m_sm.on_hello(2, id.game_id, id.rom_crcs, id.build_hash, "carol", m_current_time, m_actions);
    m_actions.clear();
    m_sm.on_join_room(2, code, m_current_time, m_actions);

    auto errs = filter_actions<ActionSendError>();
    ASSERT_EQ(errs.size(), 1u);
    EXPECT_EQ(errs[0].code, NetplayErrorCode::RoomFull);
}

// 12. ServerFullAtMaxRooms
TEST(RelayStateMachineTest, ServerFullAtMaxRooms) {
    RelayStateMachine small_sm{1};
    std::vector<Action> act;
    small_sm.on_client_connected(0, 1000.0, act);
    auto id = make_test_identity();
    small_sm.on_hello(0, id.game_id, id.rom_crcs, id.build_hash, "alice", 1000.0, act);
    small_sm.on_create_room(0, 2, 0, 1000.0, act);

    act.clear();
    small_sm.on_client_connected(1, 1000.0, act);
    small_sm.on_hello(1, id.game_id, id.rom_crcs, id.build_hash, "bob", 1000.0, act);
    small_sm.on_create_room(1, 2, 0, 1000.0, act);

    bool server_full = false;
    for (const auto &a : act) {
        if (std::holds_alternative<ActionSendError>(a)) {
            EXPECT_EQ(std::get<ActionSendError>(a).code, NetplayErrorCode::ServerFull);
            server_full = true;
        }
    }
    EXPECT_TRUE(server_full);
}

// 13. RoomExpiresAfterTTL
TEST_F(StateMachineFixture, RoomExpiresAfterTTL) {
    m_actions.clear();
    m_sm.on_client_connected(0, m_current_time, m_actions);
    auto id = make_test_identity();
    m_sm.on_hello(0, id.game_id, id.rom_crcs, id.build_hash, "alice", m_current_time, m_actions);
    m_sm.on_create_room(0, 2, 0, m_current_time, m_actions);
    auto code = filter_actions<ActionSendRoomCreated>()[0].room_code;

    // Advance time past ROOM_TTL_SECONDS
    step_time(ROOM_TTL_SECONDS + 1.0);
    EXPECT_EQ(m_sm.active_rooms_count(), 0u);

    // Host should be disconnected cleanly
    m_actions.clear();
    m_sm.on_client_connected(1, m_current_time, m_actions);
    m_sm.on_hello(1, id.game_id, id.rom_crcs, id.build_hash, "bob", m_current_time, m_actions);
    m_actions.clear();
    m_sm.on_join_room(1, code, m_current_time, m_actions);
    EXPECT_EQ(filter_actions<ActionSendError>()[0].code, NetplayErrorCode::RoomNotFound);
}

// 14. SyncBlobRelayedToJoinerOnly
TEST_F(StateMachineFixture, SyncBlobRelayedToJoinerOnly) {
    setup_active_match(2, 0x01); // Sync required

    m_actions.clear();
    std::vector<uint8_t> blob = {1, 2, 3, 4, 5};
    m_sm.on_sync_blob(0, blob, m_current_time, m_actions);

    auto fwd = filter_actions<ActionSendSyncBlobFwd>();
    ASSERT_EQ(fwd.size(), 1u);
    EXPECT_EQ(fwd[0].slot, 1);
    EXPECT_EQ(fwd[0].data, blob);

    // Host receives Go, joiner receives Go
    auto gos = filter_actions<ActionSendGo>();
    EXPECT_EQ(gos.size(), 2u);
}

// 15. GoWaitsForSyncBlob
TEST_F(StateMachineFixture, GoWaitsForSyncBlob) {
    setup_active_match(2, 0x01);
    // At match start with flag bit 0, Go is NOT sent yet
    EXPECT_TRUE(filter_actions<ActionSendGo>().empty());

    // Host sends blob -> now Go is sent
    m_actions.clear();
    std::vector<uint8_t> blob = {9};
    m_sm.on_sync_blob(0, blob, m_current_time, m_actions);
    EXPECT_EQ(filter_actions<ActionSendGo>().size(), 2u);
}

// 16. SyncBlobMaxLengthAccepted
TEST_F(StateMachineFixture, SyncBlobMaxLengthAccepted) {
    setup_active_match(2, 0x01);
    std::vector<uint8_t> max_blob(MAX_SYNC_BLOB_SIZE, 0x7F);

    m_actions.clear();
    m_sm.on_sync_blob(0, max_blob, m_current_time, m_actions);
    auto fwd = filter_actions<ActionSendSyncBlobFwd>();
    ASSERT_EQ(fwd.size(), 1u);
    EXPECT_EQ(fwd[0].data.size(), MAX_SYNC_BLOB_SIZE);
}

// 17. SyncBlobMalformedIsRejected
TEST_F(StateMachineFixture, SyncBlobMalformedIsRejected) {
    setup_active_match(2, 0x01);

    // Empty blob
    m_actions.clear();
    m_sm.on_sync_blob(0, {}, m_current_time, m_actions);
    EXPECT_EQ(filter_actions<ActionSendError>()[0].code, NetplayErrorCode::BadMessage);
}

// 18. SyncBlobFromJoinerIsRejected
TEST_F(StateMachineFixture, SyncBlobFromJoinerIsRejected) {
    setup_active_match(2, 0x01);

    m_actions.clear();
    std::vector<uint8_t> blob = {1};
    m_sm.on_sync_blob(1, blob, m_current_time, m_actions);
    EXPECT_EQ(filter_actions<ActionSendError>()[0].code, NetplayErrorCode::BadMessage);
}

// 19. SyncBlobWithoutFlagIsRejected
TEST_F(StateMachineFixture, SyncBlobWithoutFlagIsRejected) {
    setup_active_match(2, 0); // No sync flag

    m_actions.clear();
    std::vector<uint8_t> blob = {1};
    m_sm.on_sync_blob(0, blob, m_current_time, m_actions);
    EXPECT_EQ(filter_actions<ActionSendError>()[0].code, NetplayErrorCode::BadMessage);
}

// 20. DuplicateSyncBlobIsRejected
TEST_F(StateMachineFixture, DuplicateSyncBlobIsRejected) {
    setup_active_match(2, 0x01);

    std::vector<uint8_t> blob = {1};
    m_sm.on_sync_blob(0, blob, m_current_time, m_actions);

    m_actions.clear();
    m_sm.on_sync_blob(0, blob, m_current_time, m_actions);
    EXPECT_EQ(filter_actions<ActionSendError>()[0].code, NetplayErrorCode::BadMessage);
}

// 21. SyncBlobLogged
TEST_F(StateMachineFixture, SyncBlobLogged) {
    setup_active_match(2, 0x01);
    std::vector<uint8_t> blob = {0xDE, 0xAD};
    m_sm.on_sync_blob(0, blob, m_current_time, m_actions);

    m_actions.clear();
    m_sm.on_bye(0, m_current_time, m_actions);
    auto logs = filter_actions<ActionWriteLog>();
    ASSERT_EQ(logs.size(), 1u);
    const auto &blob_hex = logs[0].snapshot.sync_blob_hex;
    if (!blob_hex.has_value()) {
        FAIL() << "sync blob hex missing from log snapshot";
    }
    EXPECT_EQ(*blob_hex, "dead");
}

// 22. CreateRoomPayloadValidated
TEST_F(StateMachineFixture, CreateRoomPayloadValidated) {
    m_actions.clear();
    m_sm.on_client_connected(0, m_current_time, m_actions);
    auto id = make_test_identity();
    m_sm.on_hello(0, id.game_id, id.rom_crcs, id.build_hash, "alice", m_current_time, m_actions);

    // Invalid delay (0)
    m_actions.clear();
    m_sm.on_create_room(0, 0, 0, m_current_time, m_actions);
    EXPECT_EQ(filter_actions<ActionSendError>()[0].code, NetplayErrorCode::BadMessage);
}

// 23. ChatRelayedToPeerOnly
TEST_F(StateMachineFixture, ChatRelayedToPeerOnly) {
    setup_active_match();

    m_actions.clear();
    m_sm.on_chat(0, "hi", m_current_time, m_actions);
    auto fwd = filter_actions<ActionSendChatFwd>();
    ASSERT_EQ(fwd.size(), 1u);
    EXPECT_EQ(fwd[0].slot, 1);
    EXPECT_EQ(fwd[0].from_player, 0);
    EXPECT_EQ(fwd[0].text, "hi");
}

// 24. ChatMaxLengthAccepted
TEST_F(StateMachineFixture, ChatMaxLengthAccepted) {
    setup_active_match();

    std::string text(MAX_CHAT_LENGTH, 'a');
    m_actions.clear();
    m_sm.on_chat(0, text, m_current_time, m_actions);
    auto fwd = filter_actions<ActionSendChatFwd>();
    ASSERT_EQ(fwd.size(), 1u);
    EXPECT_EQ(fwd[0].text, text);
}

// 25. ChatBeforeMatchStartIsRejected
TEST_F(StateMachineFixture, ChatBeforeMatchStartIsRejected) {
    m_actions.clear();
    m_sm.on_client_connected(0, m_current_time, m_actions);
    auto id = make_test_identity();
    m_sm.on_hello(0, id.game_id, id.rom_crcs, id.build_hash, "alice", m_current_time, m_actions);

    m_actions.clear();
    m_sm.on_chat(0, "hi", m_current_time, m_actions);
    auto errs = filter_actions<ActionSendError>();
    ASSERT_EQ(errs.size(), 1u);
    EXPECT_EQ(errs[0].code, NetplayErrorCode::NotInLobby);
}

// 26. ChatMalformedIsRejected
TEST_F(StateMachineFixture, ChatMalformedIsRejected) {
    setup_active_match();

    // Contains newline (< 0x20)
    m_actions.clear();
    m_sm.on_chat(0, "a\nb", m_current_time, m_actions);
    EXPECT_EQ(filter_actions<ActionSendError>()[0].code, NetplayErrorCode::BadMessage);
}

// 27. ChatRateLimitDropsSixth
TEST_F(StateMachineFixture, ChatRateLimitDropsSixth) {
    setup_active_match();

    for (int i = 0; i < CHAT_BURST; ++i) {
        m_actions.clear();
        m_sm.on_chat(0, "msg", m_current_time, m_actions);
        EXPECT_EQ(filter_actions<ActionSendChatFwd>().size(), 1u);
    }

    // 6th message in same second is dropped
    m_actions.clear();
    m_sm.on_chat(0, "overflow", m_current_time, m_actions);
    EXPECT_TRUE(filter_actions<ActionSendChatFwd>().empty());

    // End match to verify chat_dropped in log
    m_actions.clear();
    m_sm.on_bye(0, m_current_time, m_actions);
    auto logs = filter_actions<ActionWriteLog>();
    ASSERT_EQ(logs.size(), 1u);
    EXPECT_EQ(logs[0].snapshot.chat_dropped[0], 1);
}

// 28. ChatRateLimitPerClient
TEST_F(StateMachineFixture, ChatRateLimitPerClient) {
    setup_active_match();

    // Both clients send 5 chats
    for (int i = 0; i < CHAT_BURST; ++i) {
        m_actions.clear();
        m_sm.on_chat(0, "p1", m_current_time, m_actions);
        EXPECT_EQ(filter_actions<ActionSendChatFwd>().size(), 1u);

        m_actions.clear();
        m_sm.on_chat(1, "p2", m_current_time, m_actions);
        EXPECT_EQ(filter_actions<ActionSendChatFwd>().size(), 1u);
    }
}

// 29. ChatInSessionLog
TEST_F(StateMachineFixture, ChatInSessionLog) {
    setup_active_match();

    m_sm.on_chat(0, "hello", m_current_time, m_actions);
    m_sm.on_chat(1, "reply", m_current_time + 0.1, m_actions);

    m_actions.clear();
    m_sm.on_bye(0, m_current_time + 0.2, m_actions);
    auto logs = filter_actions<ActionWriteLog>();
    ASSERT_EQ(logs.size(), 1u);
    const auto &chat = logs[0].snapshot.chat;
    ASSERT_EQ(chat.size(), 2u);
    EXPECT_EQ(chat[0].player, 0);
    EXPECT_EQ(chat[0].text, "hello");
    EXPECT_EQ(chat[1].player, 1);
    EXPECT_EQ(chat[1].text, "reply");
}

// 30. ByeCleanShutdown
TEST_F(StateMachineFixture, ByeCleanShutdown) {
    setup_active_match();

    m_actions.clear();
    m_sm.on_bye(0, m_current_time, m_actions);

    EXPECT_EQ(filter_actions<ActionSendPeerLeft>().size(), 1u);
    auto logs = filter_actions<ActionWriteLog>();
    ASSERT_EQ(logs.size(), 1u);
    EXPECT_EQ(logs[0].snapshot.end_reason, "bye");
}

// 31. HttpTokenEndpoint & Rate Limiting
TEST(TokenServiceTest, RateLimiting) {
    std::array<uint8_t, 32> priv{};
    TokenServer::Config cfg;
    cfg.port = 0;
    cfg.rate_burst = 10;
    cfg.rate_refill = 1.0;
    TokenServer srv(cfg, priv);
    ASSERT_TRUE(srv.start());

    httplib::Client cli("127.0.0.1", srv.bound_port());
    // First 10 requests allowed
    for (int i = 0; i < 10; ++i) {
        auto res = cli.Get("/token?v=3");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 200);
    }

    // 11th request exceeds burst limit
    auto res_overflow = cli.Get("/token?v=3");
    ASSERT_TRUE(res_overflow);
    EXPECT_EQ(res_overflow->status, 429);
}

// --- End-to-End Loopback Fixture with real Yojimbo Clients ---

namespace {
class LoopbackFixture : public ::testing::Test {
protected:
    std::filesystem::path m_log_dir = "/tmp/f3rt-relay-loopback-tests";
    std::unique_ptr<RelayServer> m_server;
    double m_current_time = 100.0;

    NetplayAdapter m_host_adapter;
    NetplayConfig m_host_config;
    std::unique_ptr<yojimbo::Client> m_host_client;

    NetplayAdapter m_joiner_adapter;
    NetplayConfig m_joiner_config;
    std::unique_ptr<yojimbo::Client> m_joiner_client;

    void SetUp() override {
        std::error_code ec;
        std::filesystem::remove_all(m_log_dir, ec);
        std::filesystem::create_directories(m_log_dir, ec);

        RelayConfig cfg;
        cfg.port = 0; // Ephemeral ports for UDP and HTTP
        cfg.logdir = m_log_dir.string();
        m_server = std::make_unique<RelayServer>(cfg);
        ASSERT_TRUE(m_server->start());

        m_current_time = 100.0;
        m_host_client = std::make_unique<yojimbo::Client>(
            yojimbo::GetDefaultAllocator(), yojimbo::Address("0.0.0.0"), m_host_config,
            m_host_adapter, m_current_time);
        m_joiner_client = std::make_unique<yojimbo::Client>(
            yojimbo::GetDefaultAllocator(), yojimbo::Address("0.0.0.0"), m_joiner_config,
            m_joiner_adapter, m_current_time);
    }

    void TearDown() override {
        if (m_host_client) m_host_client->Disconnect();
        if (m_joiner_client) m_joiner_client->Disconnect();
        if (m_server) m_server->stop();
        std::error_code ec;
        std::filesystem::remove_all(m_log_dir, ec);
    }

    bool fetch_token_and_connect(yojimbo::Client &client) {
        httplib::Client cli("127.0.0.1", m_server->http_port());
        auto res = cli.Get(std::string(TOKEN_ROUTE) + "?v=" + std::to_string(PROTOCOL_VERSION));
        if (!res || res->status != 200) {
            return false;
        }

        uint64_t client_id = 0;
        std::array<uint8_t, yojimbo::ConnectTokenBytes> token_buf{};
        if (!parse_token_response(
                std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(res->body.data()),
                                         res->body.size()),
                client_id, token_buf)) {
            return false;
        }

        return client.Connect(client_id, token_buf.data());
    }

    void pump(double dt = 0.01) {
        m_current_time += dt;
        m_server->step(m_current_time, 1);
        if (m_host_client) {
            m_host_client->AdvanceTime(m_current_time);
            m_host_client->ReceivePackets();
        }
        if (m_joiner_client) {
            m_joiner_client->AdvanceTime(m_current_time);
            m_joiner_client->ReceivePackets();
        }
        if (m_host_client) {
            m_host_client->SendPackets();
        }
        if (m_joiner_client) {
            m_joiner_client->SendPackets();
        }
    }

    bool pump_until(const std::function<bool()> &cond, double timeout_sec = 2.0) {
        auto start = std::chrono::steady_clock::now();
        while (!cond()) {
            pump(0.01);
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >
                timeout_sec) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    bool connect_both_clients() {
        if (!fetch_token_and_connect(*m_host_client)) return false;
        if (!fetch_token_and_connect(*m_joiner_client)) return false;

        return pump_until(
            [this]() { return m_host_client->IsConnected() && m_joiner_client->IsConnected(); },
            3.0);
    }
    std::string establish_wire_match(uint8_t input_delay = 2, uint8_t flags = 0) {
        auto *h = static_cast<HelloMessage *>(m_host_client->CreateMessage(MsgHello));
        std::strncpy(h->game_id, "testgame", sizeof(h->game_id));
        std::strncpy(h->player_name, "alice", sizeof(h->player_name));
        for (size_t i = 0; i < ROM_CRCS_COUNT; ++i) h->rom_crcs[i] = 100 + i;
        std::memset(h->build_hash, 0xAA, sizeof(h->build_hash));
        m_host_client->SendMessage(ChannelControl, h);

        auto *j = static_cast<HelloMessage *>(m_joiner_client->CreateMessage(MsgHello));
        std::strncpy(j->game_id, "testgame", sizeof(j->game_id));
        std::strncpy(j->player_name, "bob", sizeof(j->player_name));
        for (size_t i = 0; i < ROM_CRCS_COUNT; ++i) j->rom_crcs[i] = 100 + i;
        std::memset(j->build_hash, 0xAA, sizeof(j->build_hash));
        m_joiner_client->SendMessage(ChannelControl, j);

        bool host_welcomed = false;
        bool joiner_welcomed = false;
        EXPECT_TRUE(pump_until(
            [&]() {
                if (!host_welcomed) {
                    if (auto *msg = m_host_client->ReceiveMessage(ChannelControl)) {
                        if (msg->GetType() == MsgWelcome) host_welcomed = true;
                        m_host_client->ReleaseMessage(msg);
                    }
                }
                if (!joiner_welcomed) {
                    if (auto *msg = m_joiner_client->ReceiveMessage(ChannelControl)) {
                        if (msg->GetType() == MsgWelcome) joiner_welcomed = true;
                        m_joiner_client->ReleaseMessage(msg);
                    }
                }
                return host_welcomed && joiner_welcomed;
            },
            3.0));

        // Create room
        auto *cr = static_cast<CreateRoomMessage *>(m_host_client->CreateMessage(MsgCreateRoom));
        cr->input_delay = input_delay;
        cr->flags = flags;
        m_host_client->SendMessage(ChannelControl, cr);

        std::string room_code;
        EXPECT_TRUE(pump_until(
            [&]() {
                if (auto *msg = m_host_client->ReceiveMessage(ChannelControl)) {
                    if (msg->GetType() == MsgRoomCreated) {
                        auto *rc = static_cast<RoomCreatedMessage *>(msg);
                        room_code = rc->room_code;
                    }
                    m_host_client->ReleaseMessage(msg);
                }
                return !room_code.empty();
            },
            3.0));

        // Join room
        auto *jr = static_cast<JoinRoomMessage *>(m_joiner_client->CreateMessage(MsgJoinRoom));
        std::strncpy(jr->room_code, room_code.c_str(), sizeof(jr->room_code));
        m_joiner_client->SendMessage(ChannelControl, jr);

        bool host_started = false;
        bool joiner_started = false;
        EXPECT_TRUE(pump_until(
            [&]() {
                if (!host_started) {
                    if (auto *msg = m_host_client->ReceiveMessage(ChannelControl)) {
                        if (msg->GetType() == MsgMatchStart) host_started = true;
                        m_host_client->ReleaseMessage(msg);
                    }
                }
                if (!joiner_started) {
                    if (auto *msg = m_joiner_client->ReceiveMessage(ChannelControl)) {
                        if (msg->GetType() == MsgMatchStart) joiner_started = true;
                        m_joiner_client->ReleaseMessage(msg);
                    }
                }
                return host_started && joiner_started;
            },
            3.0));

        return room_code;
    }
};

} // namespace

// 32. Full Match & SyncBlob Gating Over Loopback Wire
TEST_F(LoopbackFixture, LoopbackFullMatchSyncBlob) {
    ASSERT_TRUE(connect_both_clients());
    std::string code = establish_wire_match(3, 0x01); // flagSyncBlob = 1
    EXPECT_FALSE(code.empty());

    // Verify Go is NOT received yet while waiting for SyncBlob
    pump(0.1);
    EXPECT_EQ(m_host_client->ReceiveMessage(ChannelControl), nullptr);
    EXPECT_EQ(m_joiner_client->ReceiveMessage(ChannelControl), nullptr);

    // Host sends 2048-byte SyncBlob block
    std::vector<uint8_t> expected_blob(MAX_SYNC_BLOB_SIZE);
    for (size_t i = 0; i < expected_blob.size(); ++i) {
        expected_blob[i] = static_cast<uint8_t>(i ^ 0x5A);
    }

    auto *sb = static_cast<SyncBlobMessage *>(m_host_client->CreateMessage(MsgSyncBlob));
    uint8_t *block = m_host_client->AllocateBlock(static_cast<int>(expected_blob.size()));
    ASSERT_NE(block, nullptr);
    std::memcpy(block, expected_blob.data(), expected_blob.size());
    m_host_client->AttachBlockToMessage(sb, block, static_cast<int>(expected_blob.size()));
    m_host_client->SendMessage(ChannelControl, sb);

    // Pump until joiner gets SyncBlobFwd and both receive Go
    bool joiner_got_blob = false;
    bool host_got_go = false;
    bool joiner_got_go = false;

    EXPECT_TRUE(pump_until(
        [&]() {
            if (!joiner_got_blob) {
                if (auto *msg = m_joiner_client->ReceiveMessage(ChannelControl)) {
                    if (msg->GetType() == MsgSyncBlobFwd) {
                        auto *fwd = static_cast<SyncBlobFwdMessage *>(msg);
                        if (fwd->GetBlockSize() == static_cast<int>(MAX_SYNC_BLOB_SIZE) &&
                            std::memcmp(fwd->GetBlockData(), expected_blob.data(),
                                        expected_blob.size()) == 0) {
                            joiner_got_blob = true;
                        }
                    }
                    m_joiner_client->ReleaseMessage(msg);
                }
            }
            if (!host_got_go) {
                if (auto *msg = m_host_client->ReceiveMessage(ChannelControl)) {
                    if (msg->GetType() == MsgGo) host_got_go = true;
                    m_host_client->ReleaseMessage(msg);
                }
            }
            if (!joiner_got_go) {
                if (auto *msg = m_joiner_client->ReceiveMessage(ChannelControl)) {
                    if (msg->GetType() == MsgGo) joiner_got_go = true;
                    m_joiner_client->ReleaseMessage(msg);
                }
            }
            return joiner_got_blob && host_got_go && joiner_got_go;
        },
        3.0));
}

// 33. Inputs Exchange Over Loopback Wire
TEST_F(LoopbackFixture, LoopbackInputsExchange) {
    ASSERT_TRUE(connect_both_clients());
    establish_wire_match(2, 0); // No sync blob, starts immediately

    // Host sends Inputs on ChannelInputs
    auto *inp_h = static_cast<InputsMessage *>(m_host_client->CreateMessage(MsgInputs));
    inp_h->first_frame = 100;
    inp_h->count = 3;
    inp_h->inputs[0] = 11;
    inp_h->inputs[1] = 22;
    inp_h->inputs[2] = 33;
    m_host_client->SendMessage(ChannelInputs, inp_h);

    // Joiner sends Inputs on ChannelInputs
    auto *inp_j = static_cast<InputsMessage *>(m_joiner_client->CreateMessage(MsgInputs));
    inp_j->first_frame = 200;
    inp_j->count = 2;
    inp_j->inputs[0] = 44;
    inp_j->inputs[1] = 55;
    m_joiner_client->SendMessage(ChannelInputs, inp_j);

    bool joiner_got_inputs = false;
    bool host_got_inputs = false;

    EXPECT_TRUE(pump_until(
        [&]() {
            if (!joiner_got_inputs) {
                if (auto *msg = m_joiner_client->ReceiveMessage(ChannelInputs)) {
                    if (msg->GetType() == MsgInputs) {
                        auto *inp = static_cast<InputsMessage *>(msg);
                        if (inp->first_frame == 100 && inp->count == 3 && inp->inputs[0] == 11 &&
                            inp->inputs[1] == 22 && inp->inputs[2] == 33) {
                            joiner_got_inputs = true;
                        }
                    }
                    m_joiner_client->ReleaseMessage(msg);
                }
            }
            if (!host_got_inputs) {
                if (auto *msg = m_host_client->ReceiveMessage(ChannelInputs)) {
                    if (msg->GetType() == MsgInputs) {
                        auto *inp = static_cast<InputsMessage *>(msg);
                        if (inp->first_frame == 200 && inp->count == 2 && inp->inputs[0] == 44 &&
                            inp->inputs[1] == 55) {
                            host_got_inputs = true;
                        }
                    }
                    m_host_client->ReleaseMessage(msg);
                }
            }
            return joiner_got_inputs && host_got_inputs;
        },
        3.0));
}

// 34. Chat, CRC Desync, and Result Agreement Over Loopback Wire
TEST_F(LoopbackFixture, LoopbackChatCrcResult) {
    ASSERT_TRUE(connect_both_clients());
    establish_wire_match(2, 0);

    // 1. Chat
    auto *chat_msg = static_cast<ChatTextMessage *>(m_host_client->CreateMessage(MsgChat));
    std::strncpy(chat_msg->text, "glhf", sizeof(chat_msg->text));
    m_host_client->SendMessage(ChannelControl, chat_msg);

    bool joiner_got_chat = false;
    EXPECT_TRUE(pump_until(
        [&]() {
            if (auto *msg = m_joiner_client->ReceiveMessage(ChannelControl)) {
                if (msg->GetType() == MsgChatFwd) {
                    auto *cf = static_cast<ChatFwdMessage *>(msg);
                    if (cf->from_player == 0 && std::string(cf->text) == "glhf") {
                        joiner_got_chat = true;
                    }
                }
                m_joiner_client->ReleaseMessage(msg);
            }
            return joiner_got_chat;
        },
        2.0));

    // 2. CRC Mismatch -> Desync
    auto *crc_h = static_cast<CrcMessage *>(m_host_client->CreateMessage(MsgCrc));
    crc_h->frame = 50;
    crc_h->crc = 111;
    m_host_client->SendMessage(ChannelControl, crc_h);

    auto *crc_j = static_cast<CrcMessage *>(m_joiner_client->CreateMessage(MsgCrc));
    crc_j->frame = 50;
    crc_j->crc = 222;
    m_joiner_client->SendMessage(ChannelControl, crc_j);

    bool host_got_desync = false;
    bool joiner_got_desync = false;
    EXPECT_TRUE(pump_until(
        [&]() {
            if (!host_got_desync) {
                if (auto *msg = m_host_client->ReceiveMessage(ChannelControl)) {
                    if (msg->GetType() == MsgDesync) host_got_desync = true;
                    m_host_client->ReleaseMessage(msg);
                }
            }
            if (!joiner_got_desync) {
                if (auto *msg = m_joiner_client->ReceiveMessage(ChannelControl)) {
                    if (msg->GetType() == MsgDesync) joiner_got_desync = true;
                    m_joiner_client->ReleaseMessage(msg);
                }
            }
            return host_got_desync && joiner_got_desync;
        },
        2.0));

    // 3. Result Agreement -> ResultAck
    auto *res_h = static_cast<ResultMessage *>(m_host_client->CreateMessage(MsgResult));
    res_h->frame = 300;
    res_h->winner = 0;
    res_h->final_crc = 999;
    m_host_client->SendMessage(ChannelControl, res_h);

    auto *res_j = static_cast<ResultMessage *>(m_joiner_client->CreateMessage(MsgResult));
    res_j->frame = 300;
    res_j->winner = 0;
    res_j->final_crc = 999;
    m_joiner_client->SendMessage(ChannelControl, res_j);

    bool host_got_ack = false;
    bool joiner_got_ack = false;
    EXPECT_TRUE(pump_until(
        [&]() {
            if (!host_got_ack) {
                if (auto *msg = m_host_client->ReceiveMessage(ChannelControl)) {
                    if (msg->GetType() == MsgResultAck) {
                        auto *ack = static_cast<ResultAckMessage *>(msg);
                        if (ack->agreed && ack->winner == 0) host_got_ack = true;
                    }
                    m_host_client->ReleaseMessage(msg);
                }
            }
            if (!joiner_got_ack) {
                if (auto *msg = m_joiner_client->ReceiveMessage(ChannelControl)) {
                    if (msg->GetType() == MsgResultAck) {
                        auto *ack = static_cast<ResultAckMessage *>(msg);
                        if (ack->agreed && ack->winner == 0) joiner_got_ack = true;
                    }
                    m_joiner_client->ReleaseMessage(msg);
                }
            }
            return host_got_ack && joiner_got_ack;
        },
        2.0));
}

// 35. Bye / Disconnect and Log File Output Over Loopback Wire
TEST_F(LoopbackFixture, LoopbackByeAndLogFile) {
    ASSERT_TRUE(connect_both_clients());
    establish_wire_match(2, 0);

    // Send input from host
    auto *inp = static_cast<InputsMessage *>(m_host_client->CreateMessage(MsgInputs));
    inp->first_frame = 0;
    inp->count = 1;
    inp->inputs[0] = 42;
    m_host_client->SendMessage(ChannelInputs, inp);
    bool joiner_got_inputs = false;
    EXPECT_TRUE(pump_until(
        [&]() {
            if (m_joiner_client->IsConnected()) {
                if (auto *msg = m_joiner_client->ReceiveMessage(ChannelInputs)) {
                    if (msg->GetType() == MsgInputs) joiner_got_inputs = true;
                    m_joiner_client->ReleaseMessage(msg);
                }
            }
            return joiner_got_inputs;
        },
        2.0));
    // Host sends Bye
    auto *bye = static_cast<ByeMessage *>(m_host_client->CreateMessage(MsgBye));
    m_host_client->SendMessage(ChannelControl, bye);
    bool joiner_got_peer_left = false;
    EXPECT_TRUE(pump_until(
        [&]() {
            if (m_joiner_client->IsConnected()) {
                if (auto *msg = m_joiner_client->ReceiveMessage(ChannelControl)) {
                    if (msg->GetType() == MsgPeerLeft) joiner_got_peer_left = true;
                    m_joiner_client->ReleaseMessage(msg);
                }
            }
            return joiner_got_peer_left;
        },
        2.0));

    // Allow server tick to finish log writing
    pump_until(
        [&]() {
            for (const auto &entry : std::filesystem::directory_iterator(m_log_dir)) {
                if (entry.path().extension() == ".json") return true;
            }
            return false;
        },
        2.0);

    // Check that JSON log was written
    bool found_log = false;
    for (const auto &entry : std::filesystem::directory_iterator(m_log_dir)) {
        if (entry.path().extension() == ".json") {
            found_log = true;
            std::ifstream ifs(entry.path());
            std::string content((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
            const auto doc = yyjson::read(content, yyjson::ReadFlag::AllowInvalidUnicode);
            const auto obj = doc.as_object();
            if (!obj.has_value()) {
                FAIL() << "end-to-end log is not a JSON object";
            }

            const auto it_end = obj->find("end_reason");
            ASSERT_NE(it_end, obj->end());
            const auto end_reason = it_end->second.as_string();
            if (!end_reason.has_value()) {
                FAIL() << "end_reason is not a string";
            }
            EXPECT_EQ(*end_reason, "bye");

            const auto it_game = obj->find("game_id");
            ASSERT_NE(it_game, obj->end());
            const auto game_id = it_game->second.as_string();
            if (!game_id.has_value()) {
                FAIL() << "game_id is not a string";
            }
            EXPECT_EQ(*game_id, "testgame");

            const auto it_names = obj->find("player_names");
            ASSERT_NE(it_names, obj->end());
            const auto names_arr = it_names->second.as_array();
            if (!names_arr.has_value()) {
                FAIL() << "player_names is not an array";
            }
            ASSERT_GE(names_arr->size(), 2u);
            const auto name0 = (*names_arr)[0].as_string();
            const auto name1 = (*names_arr)[1].as_string();
            if (!name0.has_value() || !name1.has_value()) {
                FAIL() << "player_names entries are not strings";
            }
            EXPECT_EQ(*name0, "alice");
            EXPECT_EQ(*name1, "bob");

            const auto it_inputs = obj->find("inputs");
            ASSERT_NE(it_inputs, obj->end());
            const auto inputs_arr = it_inputs->second.as_array();
            if (!inputs_arr.has_value()) {
                FAIL() << "inputs is not an array";
            }
            const auto p0_inputs = (*inputs_arr)[0].as_array();
            if (!p0_inputs.has_value()) {
                FAIL() << "inputs[0] is not an array";
            }
            ASSERT_FALSE(p0_inputs->empty());
            const auto first_input = (*p0_inputs)[0].as_int();
            if (!first_input.has_value()) {
                FAIL() << "inputs[0][0] is not an integer";
            }
            EXPECT_EQ(*first_input, 42);
        }
    }
    EXPECT_TRUE(found_log);
}

// 36. Token Version Enforcement and Malformed Client Disconnect
TEST_F(LoopbackFixture, LoopbackTokenVersionAndMalformedClient) {
    httplib::Client cli("127.0.0.1", m_server->http_port());

    // 1. Wrong version query param gives 409
    auto res_bad = cli.Get(std::string(TOKEN_ROUTE) + "?v=999");
    ASSERT_TRUE(res_bad);
    EXPECT_EQ(res_bad->status, 409);
    EXPECT_EQ(res_bad->body, "version mismatch\n");

    // 2. Connect client and send invalid first message (CreateRoom without Hello)
    ASSERT_TRUE(fetch_token_and_connect(*m_host_client));
    ASSERT_TRUE(pump_until([this]() { return m_host_client->IsConnected(); }, 2.0));

    auto *cr = static_cast<CreateRoomMessage *>(m_host_client->CreateMessage(MsgCreateRoom));
    cr->input_delay = 2;
    m_host_client->SendMessage(ChannelControl, cr);

    bool got_error = false;
    EXPECT_TRUE(pump_until(
        [&]() {
            if (m_host_client->IsConnected()) {
                if (auto *msg = m_host_client->ReceiveMessage(ChannelControl)) {
                    if (msg->GetType() == MsgError) {
                        auto *err = static_cast<ErrorMessage *>(msg);
                        if (err->code == static_cast<uint16_t>(NetplayErrorCode::NotInLobby)) {
                            got_error = true;
                        }
                    }
                    m_host_client->ReleaseMessage(msg);
                }
            }
            return got_error;
        },
        2.0));
    EXPECT_TRUE(pump_until(
        [&]() { return m_host_client->IsDisconnected() || m_host_client->ConnectionFailed(); },
        2.0));
}

} // namespace f3rt::netplay
