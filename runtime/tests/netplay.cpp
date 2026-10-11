#include "f3rt/netplay.hpp"
#include "netplay/protocol.hpp"
#include "netplay/sha256.hpp"
#include "netplay/socket.hpp"
#include <thread>
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#endif
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace f3rt::netplay;

namespace {

std::string to_hex(std::span<const uint8_t> bytes) {
    std::ostringstream oss;
    for (uint8_t b : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << int(b);
    }
    return oss.str();
}

std::vector<uint8_t> from_hex(std::string_view hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        uint8_t val = static_cast<uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16));
        bytes.push_back(val);
    }
    return bytes;
}

} // namespace

// 1. SHA-256 FIPS 180-2 Vectors
TEST(NetplaySha256, Fips180_2Vectors) {
    // Vector 1: Empty string
    EXPECT_EQ(to_hex(sha256("")),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // Vector 2: "abc"
    EXPECT_EQ(to_hex(sha256("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // Vector 3: 56-byte string
    std::string_view v3 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    EXPECT_EQ(to_hex(sha256(v3)),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // Incremental update matches one-shot
    Sha256 inc;
    for (char c : v3) {
        inc.update(&c, 1);
    }
    EXPECT_EQ(to_hex(inc.finalize()), to_hex(sha256(v3)));
}

// 2. HMAC-SHA256 RFC 4231 Vectors
TEST(NetplayHmacSha256, Rfc4231Vectors) {
    // Case 1
    std::vector<uint8_t> key1(20, 0x0b);
    std::string_view data1 = "Hi There";
    EXPECT_EQ(to_hex(hmac_sha256(key1, {reinterpret_cast<const uint8_t *>(data1.data()), data1.size()})),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    // Case 2
    std::string_view key2 = "Jefe";
    std::string_view data2 = "what do ya want for nothing?";
    EXPECT_EQ(to_hex(hmac_sha256({reinterpret_cast<const uint8_t *>(key2.data()), key2.size()},
                                 {reinterpret_cast<const uint8_t *>(data2.data()), data2.size()})),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // Case 3
    std::vector<uint8_t> key3(20, 0xaa);
    std::vector<uint8_t> data3(50, 0xdd);
    EXPECT_EQ(to_hex(hmac_sha256(key3, data3)),
              "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

    // Case 4
    std::vector<uint8_t> key4;
    for (uint8_t i = 1; i <= 25; ++i) key4.push_back(i);
    std::vector<uint8_t> data4(50, 0xcd);
    EXPECT_EQ(to_hex(hmac_sha256(key4, data4)),
              "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");

    // Case 5 (truncation to 16 bytes / tag)
    std::vector<uint8_t> key5(20, 0x0c);
    std::string_view data5 = "Test With Truncation";
    auto tag5 = hmac_sha256_tag16(key5, {reinterpret_cast<const uint8_t *>(data5.data()), data5.size()});
    EXPECT_EQ(to_hex(tag5), "a3b6167473100ee06e0c796c2955552b");

    // Case 6 (key > 64B)
    std::vector<uint8_t> key6(131, 0xaa);
    std::string_view data6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    EXPECT_EQ(to_hex(hmac_sha256(key6, {reinterpret_cast<const uint8_t *>(data6.data()), data6.size()})),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    // Case 7 (key > 64B and large data)
    std::vector<uint8_t> key7(131, 0xaa);
    std::string_view data7 = "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm.";
    EXPECT_EQ(to_hex(hmac_sha256(key7, {reinterpret_cast<const uint8_t *>(data7.data()), data7.size()})),
              "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

// 3. TCP Protocol Message Encoding & Decoding
TEST(NetplayProtocol, TcpMessagesEncodeDecode) {
    // WELCOME
    MsgWelcome welcome{1};
    std::vector<uint8_t> welcome_payload = {0x00, 0x01};
    MsgWelcome dec_welcome;
    EXPECT_TRUE(decode_welcome(welcome_payload, dec_welcome));
    EXPECT_EQ(dec_welcome.proto_version, 1);

    // ROOM_CREATED
    std::vector<uint8_t> rc_payload = {'X', 'Y', 'Z', '2', '3', '4'};
    MsgRoomCreated dec_rc;
    EXPECT_TRUE(decode_room_created(rc_payload, dec_rc));
    EXPECT_EQ(dec_rc.room_code, "XYZ234");

    // MATCH_START
    MsgMatchStart ms;
    ms.session_id = 0x1122334455667788ULL;
    ms.session_key.fill(0x5a);
    ms.seed = 0x99aabbccddeeff00ULL;
    ms.player_index = 1;
    ms.input_delay = 3;
    ms.udp_port = 7845;
    ms.peer_name = "challenger";

    std::vector<uint8_t> ms_payload;
    ByteWriter w(ms_payload);
    w.write_u64(ms.session_id);
    w.write_bytes(ms.session_key.data(), 32);
    w.write_u64(ms.seed);
    w.write_u8(ms.player_index);
    w.write_u8(ms.input_delay);
    w.write_u16(ms.udp_port);
    w.write_str(ms.peer_name);

    MsgMatchStart dec_ms;
    EXPECT_TRUE(decode_match_start(ms_payload, dec_ms));
    EXPECT_EQ(dec_ms.session_id, ms.session_id);
    EXPECT_EQ(dec_ms.session_key, ms.session_key);
    EXPECT_EQ(dec_ms.seed, ms.seed);
    EXPECT_EQ(dec_ms.player_index, ms.player_index);
    EXPECT_EQ(dec_ms.input_delay, ms.input_delay);
    EXPECT_EQ(dec_ms.udp_port, ms.udp_port);
    EXPECT_EQ(dec_ms.peer_name, ms.peer_name);

    // DESYNC
    std::vector<uint8_t> desync_payload;
    ByteWriter dw(desync_payload);
    dw.write_u32(120);
    dw.write_u32(0x12345678);
    dw.write_u32(0x87654321);
    MsgDesync dec_desync;
    EXPECT_TRUE(decode_desync(desync_payload, dec_desync));
    EXPECT_EQ(dec_desync.frame, 120u);
    EXPECT_EQ(dec_desync.crc_p1, 0x12345678u);
    EXPECT_EQ(dec_desync.crc_p2, 0x87654321u);

    // RESULT_ACK
    std::vector<uint8_t> ack_payload = {1, 0xFF};
    MsgResultAck dec_ack;
    EXPECT_TRUE(decode_result_ack(ack_payload, dec_ack));
    EXPECT_EQ(dec_ack.agreed, 1);
    EXPECT_EQ(dec_ack.winner, 0xFF);

    // ERROR
    std::vector<uint8_t> err_payload;
    ByteWriter ew(err_payload);
    ew.write_u16(5);
    ew.write_str("identity mismatch");
    MsgError dec_err;
    EXPECT_TRUE(decode_error(err_payload, dec_err));
    EXPECT_EQ(dec_err.code, 5);
    EXPECT_EQ(dec_err.message, "identity mismatch");
    // CREATE_ROOM (2 bytes: delay + flags)
    MsgCreateRoom cr{3, 1};
    auto cr_frame = encode_create_room(cr);
    EXPECT_EQ(cr_frame.size(), 7u);
    EXPECT_EQ(cr_frame[4], tcp_msg::create_room);
    EXPECT_EQ(cr_frame[5], 3);
    EXPECT_EQ(cr_frame[6], 1);

    // SYNC_BLOB encode & decode
    std::vector<uint8_t> blob = {0x01, 0x02, 0x03, 0x04};
    auto blob_frame = encode_sync_blob(blob);
    EXPECT_EQ(blob_frame.size(), 4 + 1 + 2 + 4);
    EXPECT_EQ(blob_frame[4], tcp_msg::sync_blob);
    std::vector<uint8_t> dec_blob;
    std::span<const uint8_t> blob_payload(blob_frame.data() + 5, blob_frame.size() - 5);
    EXPECT_TRUE(decode_sync_blob(blob_payload, dec_blob));
    EXPECT_EQ(dec_blob, blob);

    // Reject empty blob (< 1 byte)
    std::vector<uint8_t> empty_blob_payload = {0x00, 0x00};
    EXPECT_FALSE(decode_sync_blob(empty_blob_payload, dec_blob));

    // Reject oversized blob (> 2048 bytes)
    std::vector<uint8_t> huge_blob_payload = {0x08, 0x01}; // 2049
    EXPECT_FALSE(decode_sync_blob(huge_blob_payload, dec_blob));

    // Reject trailing bytes
    std::vector<uint8_t> trailing_payload = {0x00, 0x01, 0xaa, 0xbb};
    EXPECT_FALSE(decode_sync_blob(trailing_payload, dec_blob));
}

TEST(NetplayProtocol, ChatEncodeDecodeAndValidation) {
    // 1. Validation tests
    EXPECT_FALSE(validate_chat_text("")); // empty rejected
    EXPECT_TRUE(validate_chat_text("Hello, netplay!")); // standard ASCII
    EXPECT_TRUE(validate_chat_text("こんにちは世界")); // valid UTF-8 multibyte

    // Max length 256 allowed, 257 rejected
    std::string s256(256, 'a');
    EXPECT_TRUE(validate_chat_text(s256));
    std::string s257(257, 'a');
    EXPECT_FALSE(validate_chat_text(s257));

    // Control chars < 0x20 rejected
    EXPECT_FALSE(validate_chat_text("hello\nworld"));
    EXPECT_FALSE(validate_chat_text("hello\tworld"));
    EXPECT_FALSE(validate_chat_text(std::string_view("\x1f", 1)));

    // 0x7F (DEL) rejected
    EXPECT_FALSE(validate_chat_text("bad\x7f"));

    // 2. Encode and decode round-trip
    std::string msg_text = "Good luck, have fun!";
    auto frame = encode_chat(msg_text);
    EXPECT_EQ(frame[4], tcp_msg::chat);
    uint16_t enc_len = (uint16_t(frame[5]) << 8) | frame[6];
    EXPECT_EQ(enc_len, msg_text.size());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(frame.data() + 7), enc_len), msg_text);

    // Decode chat_fwd payload
    std::vector<uint8_t> fwd_payload;
    ByteWriter fw(fwd_payload);
    fw.write_u8(1); // from_player 1
    fw.write_u16(uint16_t(msg_text.size()));
    fw.write_bytes(msg_text.data(), msg_text.size());

    MsgChat dec_chat;
    EXPECT_TRUE(decode_chat_fwd(fwd_payload, dec_chat));
    EXPECT_EQ(dec_chat.from_player, 1);
    EXPECT_EQ(dec_chat.text, msg_text);

    // Decode rejects invalid text or truncated payload
    fwd_payload[3 + 4] = 0x0a; // newline injected
    EXPECT_FALSE(decode_chat_fwd(fwd_payload, dec_chat));
}

// 4. UDP REGISTER Packet
TEST(NetplayProtocol, UdpRegisterPacket) {
    uint64_t session_id = 0xabcdef1234567890ULL;
    uint8_t player_index = 0;
    std::array<uint8_t, 32> session_key{};
    session_key.fill(0x33);

    auto pkt = encode_udp_register(session_id, player_index, session_key);
    EXPECT_EQ(pkt.size(), 30u);

    uint8_t out_p = 0xFF, out_kind = 0xFF;
    std::vector<uint8_t> out_body;
    EXPECT_TRUE(decode_udp_packet(pkt, session_key, session_id, out_p, out_kind, out_body));
    EXPECT_EQ(out_p, 0);
    EXPECT_EQ(out_kind, udp_kind::reg);
    EXPECT_TRUE(out_body.empty());

    // Bad tag rejected
    auto bad_pkt = pkt;
    bad_pkt.back() ^= 0xff;
    EXPECT_FALSE(decode_udp_packet(bad_pkt, session_key, session_id, out_p, out_kind, out_body));

    // Wrong session rejected
    EXPECT_FALSE(decode_udp_packet(pkt, session_key, session_id + 1, out_p, out_kind, out_body));
}

// 5. UDP INPUTS Packet
TEST(NetplayProtocol, UdpInputsPacket) {
    uint64_t session_id = 0x123456789abcdef0ULL;
    uint8_t player_index = 1;
    std::array<uint8_t, 32> session_key{};
    session_key.fill(0x77);

    UdpInputs inps;
    inps.first_frame = 100;
    inps.inputs = {0x0001, 0x0010, 0x0100, 0x1000};

    auto pkt = encode_udp_inputs(session_id, player_index, session_key, inps);
    EXPECT_EQ(pkt.size(), 14 + 5 + 4 * 2 + 16);

    uint8_t out_p = 0xFF, out_kind = 0xFF;
    std::vector<uint8_t> out_body;
    EXPECT_TRUE(decode_udp_packet(pkt, session_key, session_id, out_p, out_kind, out_body));
    EXPECT_EQ(out_p, 1);
    EXPECT_EQ(out_kind, udp_kind::inputs);

    UdpInputs dec_inps;
    EXPECT_TRUE(decode_udp_inputs_body(out_body, dec_inps));
    EXPECT_EQ(dec_inps.first_frame, 100u);
    EXPECT_EQ(dec_inps.inputs, inps.inputs);

    // Corrupted payload rejected
    auto corrupted = pkt;
    corrupted[14] ^= 0x01; // flip bit in first_frame
    EXPECT_FALSE(decode_udp_packet(corrupted, session_key, session_id, out_p, out_kind, out_body));
}

// 6. RapidCheck Property: UDP Inputs Roundtrip
RC_GTEST_PROP(NetplayProtocol, UdpInputsRoundTrip, (uint64_t session_id, uint8_t player_index, uint32_t first_frame)) {
    const size_t count = *rc::gen::inRange(1u, 33u);
    std::vector<uint16_t> inputs(count);
    for (size_t i = 0; i < count; ++i) {
        inputs[i] = *rc::gen::inRange(uint16_t(0), uint16_t(0xffff));
    }
    std::array<uint8_t, 32> session_key{};
    for (size_t i = 0; i < 32; ++i) {
        session_key[i] = *rc::gen::inRange(uint8_t(0), uint8_t(0xff));
    }

    UdpInputs orig{first_frame, inputs};
    auto pkt = encode_udp_inputs(session_id, player_index, session_key, orig);

    uint8_t dec_p = 0, dec_kind = 0;
    std::vector<uint8_t> dec_body;
    RC_ASSERT(decode_udp_packet(pkt, session_key, session_id, dec_p, dec_kind, dec_body));
    RC_ASSERT(dec_p == player_index);
    RC_ASSERT(dec_kind == udp_kind::inputs);

    UdpInputs dec_inps;
    RC_ASSERT(decode_udp_inputs_body(dec_body, dec_inps));
    RC_ASSERT(dec_inps.first_frame == first_frame);
    RC_ASSERT(dec_inps.inputs == inputs);
}

// 7. Scheduler Delay and Initial Frames
TEST(NetplayScheduler, DelayAndInitialFrames) {
    SessionConfig cfg;
    cfg.is_host = true;
    cfg.input_delay = 2;

    Session session(cfg);
    // Before match running: not ready
    EXPECT_FALSE(session.ready_to_step(0));
}

// 8. Scheduler Out-of-Order and Duplicate Packet Handling
TEST(NetplayScheduler, OutOfOrderAndDuplicatePackets) {
    std::array<uint8_t, 32> key{};
    key.fill(0x42);
    uint64_t sess = 0x999;

    UdpInputs p1{2, {0x10, 0x20, 0x30}};
    auto pkt1 = encode_udp_inputs(sess, 1, key, p1);

    UdpInputs p2{4, {0x30, 0x40, 0x50}};
    auto pkt2 = encode_udp_inputs(sess, 1, key, p2);

    uint8_t p = 0, k = 0;
    std::vector<uint8_t> body;
    EXPECT_TRUE(decode_udp_packet(pkt2, key, sess, p, k, body));
    UdpInputs dec2;
    EXPECT_TRUE(decode_udp_inputs_body(body, dec2));
    EXPECT_EQ(dec2.first_frame, 4u);
    EXPECT_EQ(dec2.inputs.size(), 3u);

    // Duplicate decode
    EXPECT_TRUE(decode_udp_packet(pkt2, key, sess, p, k, body));
    EXPECT_TRUE(decode_udp_inputs_body(body, dec2));
    EXPECT_EQ(dec2.first_frame, 4u);

    // Out of order: older packet arrived afterwards
    EXPECT_TRUE(decode_udp_packet(pkt1, key, sess, p, k, body));
    UdpInputs dec1;
    EXPECT_TRUE(decode_udp_inputs_body(body, dec1));
    EXPECT_EQ(dec1.first_frame, 2u);
    EXPECT_EQ(dec1.inputs.size(), 3u);
}

// 9. Scheduler Stall and Input Retrieval
TEST(NetplayScheduler, StallAndInputRetrieval) {
    Socket listener = Socket::create_tcp();
    ASSERT_TRUE(listener.bind(0, true));
    uint16_t tcp_port = listener.local_port();
    ASSERT_GT(tcp_port, 0u);
    ASSERT_EQ(::listen(listener.handle(), 1), 0);

    SessionConfig cfg;
    cfg.server_host = "127.0.0.1";
    cfg.server_port = tcp_port;
    cfg.is_host = true;
    cfg.input_delay = 2;
    cfg.game_id = "test";
    cfg.player_name = "host";

    Session session(cfg);
    session.start();

    Socket client_tcp;
    for (int i = 0; i < 50; ++i) {
        session.poll();
        sockaddr_in sa{};
        socklen_t slen = sizeof(sa);
        socket_handle_t h = ::accept(listener.handle(), reinterpret_cast<sockaddr *>(&sa), &slen);
        if (h != invalid_socket_handle) {
            client_tcp = Socket(h);
            client_tcp.set_nonblocking(true);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(client_tcp.is_valid());

    std::vector<uint8_t> in_buf;
    bool wb = false;
    for (int i = 0; i < 50; ++i) {
        session.poll();
        uint8_t tmp[256];
        ssize_t n = client_tcp.recv(tmp, sizeof(tmp), wb);
        if (n > 0) in_buf.insert(in_buf.end(), tmp, tmp + n);
        if (in_buf.size() >= 5) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_GE(in_buf.size(), 5u);

    std::vector<uint8_t> welcome_payload = {0x00, 0x01};
    auto welcome_frame = encode_tcp_frame(tcp_msg::welcome, welcome_payload);
    client_tcp.send(welcome_frame.data(), welcome_frame.size(), wb);

    in_buf.clear();
    for (int i = 0; i < 50; ++i) {
        session.poll();
        uint8_t tmp[256];
        ssize_t n = client_tcp.recv(tmp, sizeof(tmp), wb);
        if (n > 0) in_buf.insert(in_buf.end(), tmp, tmp + n);
        if (in_buf.size() >= 5) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_GE(in_buf.size(), 5u);

    uint64_t session_id = 0x11223344ULL;
    std::array<uint8_t, 32> session_key{};
    session_key.fill(0x55);

    Socket server_udp = Socket::create_udp();
    ASSERT_TRUE(server_udp.bind(0, true));
    uint16_t udp_port = server_udp.local_port();

    MsgMatchStart ms;
    ms.session_id = session_id;
    ms.session_key = session_key;
    ms.seed = 12345;
    ms.player_index = 0;
    ms.input_delay = 2;
    ms.udp_port = udp_port;
    ms.peer_name = "joiner";

    std::vector<uint8_t> ms_payload;
    ByteWriter msw(ms_payload);
    msw.write_u64(ms.session_id);
    msw.write_bytes(ms.session_key.data(), 32);
    msw.write_u64(ms.seed);
    msw.write_u8(ms.player_index);
    msw.write_u8(ms.input_delay);
    msw.write_u16(ms.udp_port);
    msw.write_str(ms.peer_name);

    auto ms_frame = encode_tcp_frame(tcp_msg::match_start, ms_payload);
    client_tcp.send(ms_frame.data(), ms_frame.size(), wb);

    for (int i = 0; i < 50; ++i) {
        session.poll();
        if (session.state() == SessionState::Registering) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(session.state(), SessionState::Registering);

    auto go_frame = encode_tcp_frame(tcp_msg::go, {});
    client_tcp.send(go_frame.data(), go_frame.size(), wb);

    for (int i = 0; i < 50; ++i) {
        session.poll();
        if (session.state() == SessionState::Running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(session.state(), SessionState::Running);

    // Frame 0: delay=2, inputs are 0
    session.submit_local(0, 0x1111);
    EXPECT_TRUE(session.ready_to_step(0));
    EXPECT_EQ(session.inputs_for(0)[0], 0u);
    EXPECT_EQ(session.inputs_for(0)[1], 0u);

    // Frame 1: delay=2, inputs are 0
    session.submit_local(1, 0x2222);
    EXPECT_TRUE(session.ready_to_step(1));
    EXPECT_EQ(session.inputs_for(1)[0], 0u);
    EXPECT_EQ(session.inputs_for(1)[1], 0u);

    // Frame 2: local submitted, peer NOT yet received -> STALL!
    session.submit_local(2, 0x3333);
    EXPECT_FALSE(session.ready_to_step(2));
    EXPECT_GT(session.stats().stall_count, 0u);

    // Peer sends UDP packet with frame 2 input
    UdpInputs peer_inps;
    peer_inps.first_frame = 2;
    peer_inps.inputs = {0x4444};
    auto pkt = encode_udp_inputs(session_id, 1, session_key, peer_inps);
    SocketAddress session_udp_addr;
    SocketAddress::resolve("127.0.0.1", session.local_udp_port(), session_udp_addr);
    server_udp.sendto(pkt.data(), pkt.size(), session_udp_addr, wb);

    for (int i = 0; i < 50; ++i) {
        session.poll();
        if (session.ready_to_step(2)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(session.ready_to_step(2));
    auto in2 = session.inputs_for(2);
    EXPECT_EQ(in2[0], 0x1111); // P1 (local)
    EXPECT_EQ(in2[1], 0x4444); // P2 (peer)

    // Frame 3: local submitted, peer NOT received -> STALL!
    EXPECT_FALSE(session.ready_to_step(3));

    session.close();
}

// 10. Sync Blob Sequencing
TEST(NetplayScheduler, SyncBlobSequencing) {
    Socket listener = Socket::create_tcp();
    ASSERT_TRUE(listener.bind(0, true));
    uint16_t tcp_port = listener.local_port();
    ASSERT_GT(tcp_port, 0u);
    ASSERT_EQ(::listen(listener.handle(), 2), 0);

    std::vector<uint8_t> host_blob = {0x12, 0x34, 0x56, 0x78};
    SessionConfig host_cfg;
    host_cfg.server_host = "127.0.0.1";
    host_cfg.server_port = tcp_port;
    host_cfg.is_host = true;
    host_cfg.input_delay = 2;
    host_cfg.game_id = "test";
    host_cfg.player_name = "host";
    host_cfg.capture_sync = [&]() { return host_blob; };

    Session host_session(host_cfg);
    host_session.start();

    Socket host_tcp;
    for (int i = 0; i < 50; ++i) {
        host_session.poll();
        sockaddr_in sa{};
        socklen_t slen = sizeof(sa);
        socket_handle_t h = ::accept(listener.handle(), reinterpret_cast<sockaddr *>(&sa), &slen);
        if (h != invalid_socket_handle) {
            host_tcp = Socket(h);
            host_tcp.set_nonblocking(true);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(host_tcp.is_valid());

    std::vector<uint8_t> in_buf;
    bool wb = false;
    for (int i = 0; i < 50; ++i) {
        host_session.poll();
        uint8_t tmp[256];
        ssize_t n = host_tcp.recv(tmp, sizeof(tmp), wb);
        if (n > 0) in_buf.insert(in_buf.end(), tmp, tmp + n);
        if (in_buf.size() >= 5) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_GE(in_buf.size(), 5u);

    auto welcome_frame = encode_tcp_frame(tcp_msg::welcome, std::vector<uint8_t>{0x00, 0x01});
    host_tcp.send(welcome_frame.data(), welcome_frame.size(), wb);

    in_buf.clear();
    for (int i = 0; i < 50; ++i) {
        host_session.poll();
        uint8_t tmp[256];
        ssize_t n = host_tcp.recv(tmp, sizeof(tmp), wb);
        if (n > 0) in_buf.insert(in_buf.end(), tmp, tmp + n);
        if (in_buf.size() >= 7) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_GE(in_buf.size(), 7u);
    EXPECT_EQ(in_buf[4], tcp_msg::create_room);
    EXPECT_EQ(in_buf[5], 2);
    EXPECT_EQ(in_buf[6], 1);

    auto rc_frame = encode_tcp_frame(tcp_msg::room_created, std::vector<uint8_t>{'A', 'B', 'C', 'D', 'E', 'F'});
    host_tcp.send(rc_frame.data(), rc_frame.size(), wb);

    std::vector<uint8_t> joiner_applied_blob;
    uint64_t joiner_applied_seed = 0;
    SessionConfig joiner_cfg;
    joiner_cfg.server_host = "127.0.0.1";
    joiner_cfg.server_port = tcp_port;
    joiner_cfg.is_host = false;
    joiner_cfg.room_code = "ABCDEF";
    joiner_cfg.game_id = "test";
    joiner_cfg.player_name = "joiner";
    joiner_cfg.apply_sync = [&](std::span<const uint8_t> b, uint64_t s) {
        joiner_applied_blob.assign(b.begin(), b.end());
        joiner_applied_seed = s;
    };

    Session joiner_session(joiner_cfg);
    joiner_session.start();

    Socket joiner_tcp;
    for (int i = 0; i < 50; ++i) {
        host_session.poll();
        joiner_session.poll();
        sockaddr_in sa{};
        socklen_t slen = sizeof(sa);
        socket_handle_t h = ::accept(listener.handle(), reinterpret_cast<sockaddr *>(&sa), &slen);
        if (h != invalid_socket_handle) {
            joiner_tcp = Socket(h);
            joiner_tcp.set_nonblocking(true);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(joiner_tcp.is_valid());

    in_buf.clear();
    for (int i = 0; i < 50; ++i) {
        joiner_session.poll();
        uint8_t tmp[256];
        ssize_t n = joiner_tcp.recv(tmp, sizeof(tmp), wb);
        if (n > 0) in_buf.insert(in_buf.end(), tmp, tmp + n);
        if (in_buf.size() >= 5) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_GE(in_buf.size(), 5u);
    joiner_tcp.send(welcome_frame.data(), welcome_frame.size(), wb);

    in_buf.clear();
    for (int i = 0; i < 50; ++i) {
        joiner_session.poll();
        uint8_t tmp[256];
        ssize_t n = joiner_tcp.recv(tmp, sizeof(tmp), wb);
        if (n > 0) in_buf.insert(in_buf.end(), tmp, tmp + n);
        if (in_buf.size() >= 11) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_GE(in_buf.size(), 11u);
    EXPECT_EQ(in_buf[4], tcp_msg::join_room);

    uint64_t session_id = 0x8899aabbULL;
    std::array<uint8_t, 32> session_key{};
    session_key.fill(0x77);

    Socket server_udp = Socket::create_udp();
    ASSERT_TRUE(server_udp.bind(0, true));
    uint16_t udp_port = server_udp.local_port();

    auto make_ms = [&](uint8_t pidx, const std::string &peer) {
        std::vector<uint8_t> p;
        ByteWriter pw(p);
        pw.write_u64(session_id);
        pw.write_bytes(session_key.data(), 32);
        pw.write_u64(0x12345678ULL);
        pw.write_u8(pidx);
        pw.write_u8(2);
        pw.write_u16(udp_port);
        pw.write_str(peer);
        return encode_tcp_frame(tcp_msg::match_start, p);
    };

    auto send_all = [&](Socket &s, const std::vector<uint8_t> &data) {
        size_t sent = 0;
        for (int i = 0; i < 100 && sent < data.size(); ++i) {
            bool would_block = false;
            ssize_t n = s.send(data.data() + sent, data.size() - sent, would_block);
            if (n > 0) sent += n;
            else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ASSERT_EQ(sent, data.size());
    };

    auto host_ms = make_ms(0, "joiner");
    auto joiner_ms = make_ms(1, "host");
    send_all(host_tcp, host_ms);
    send_all(joiner_tcp, joiner_ms);
    in_buf.clear();
    for (int i = 0; i < 50; ++i) {
        host_session.poll();
        uint8_t tmp[256];
        ssize_t n = host_tcp.recv(tmp, sizeof(tmp), wb);
        if (n > 0) in_buf.insert(in_buf.end(), tmp, tmp + n);
        if (in_buf.size() >= 4 + 1 + 2 + host_blob.size()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_GE(in_buf.size(), 4 + 1 + 2 + host_blob.size());
    EXPECT_EQ(in_buf[4], tcp_msg::sync_blob);
    std::vector<uint8_t> host_blob_received;
    EXPECT_TRUE(decode_sync_blob(std::span<const uint8_t>(in_buf.data() + 5, in_buf.size() - 5), host_blob_received));
    EXPECT_EQ(host_blob_received, host_blob);

    EXPECT_TRUE(joiner_applied_blob.empty());
    uint8_t utmp[256];
    SocketAddress ufrom;
    ssize_t un = server_udp.recvfrom(utmp, sizeof(utmp), ufrom, wb);
    if (un > 0) {
        uint8_t uidx = 0xFF, ukind = 0xFF;
        std::vector<uint8_t> ubody;
        if (decode_udp_packet(std::span<const uint8_t>(utmp, size_t(un)), session_key, session_id, uidx, ukind, ubody)) {
            EXPECT_NE(uidx, 1);
        }
    }

    std::vector<uint8_t> blob_payload;
    ByteWriter bw(blob_payload);
    bw.write_u16(uint16_t(host_blob.size()));
    bw.write_bytes(host_blob.data(), host_blob.size());
    auto sync_fwd_frame = encode_tcp_frame(tcp_msg::sync_blob_fwd, blob_payload);
    send_all(joiner_tcp, sync_fwd_frame);

    for (int i = 0; i < 100; ++i) {
        joiner_session.poll();
        if (!joiner_applied_blob.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(joiner_applied_blob, host_blob);
    EXPECT_EQ(joiner_applied_seed, 0x12345678ULL);

    bool joiner_registered = false;
    for (int i = 0; i < 50; ++i) {
        joiner_session.poll();
        ssize_t n = server_udp.recvfrom(utmp, sizeof(utmp), ufrom, wb);
        if (n > 0) {
            uint8_t uidx = 0xFF, ukind = 0xFF;
            std::vector<uint8_t> ubody;
            if (decode_udp_packet(std::span<const uint8_t>(utmp, size_t(n)), session_key, session_id, uidx, ukind, ubody)) {
                if (uidx == 1 && ukind == udp_kind::reg) {
                    joiner_registered = true;
                    break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(joiner_registered);

    auto go_frame = encode_tcp_frame(tcp_msg::go, {});
    host_tcp.send(go_frame.data(), go_frame.size(), wb);
    joiner_tcp.send(go_frame.data(), go_frame.size(), wb);

    for (int i = 0; i < 50; ++i) {
        host_session.poll();
        joiner_session.poll();
        if (host_session.state() == SessionState::Running && joiner_session.state() == SessionState::Running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(host_session.state(), SessionState::Running);
    EXPECT_EQ(joiner_session.state(), SessionState::Running);

    // Test in-session chat: send from host, local echo, relay to joiner
    EXPECT_TRUE(host_session.send_chat("gg wp!"));
    auto host_echo = host_session.drain_chat();
    ASSERT_EQ(host_echo.size(), 1u);
    EXPECT_EQ(host_echo[0].from_player, 0);
    EXPECT_EQ(host_echo[0].text, "gg wp!");

    uint8_t ctmp[512];
    ssize_t cn = -1;
    for (int i = 0; i < 50; ++i) {
        host_session.poll();
        cn = host_tcp.recv(ctmp, sizeof(ctmp), wb);
        if (cn > 5) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_GT(cn, 5);
    EXPECT_EQ(ctmp[4], tcp_msg::chat);
    uint16_t clen = (uint16_t(ctmp[5]) << 8) | ctmp[6];
    std::string ctxt(reinterpret_cast<const char *>(ctmp + 7), clen);

    std::vector<uint8_t> fwd_pl;
    ByteWriter cfw(fwd_pl);
    cfw.write_u8(0); // from host (player 0)
    cfw.write_u16(clen);
    cfw.write_bytes(ctxt.data(), clen);
    auto fwd_frame = encode_tcp_frame(tcp_msg::chat_fwd, fwd_pl);
    joiner_tcp.send(fwd_frame.data(), fwd_frame.size(), wb);

    for (int i = 0; i < 50; ++i) {
        joiner_session.poll();
        auto jchats = joiner_session.drain_chat();
        if (!jchats.empty()) {
            ASSERT_EQ(jchats.size(), 1u);
            EXPECT_EQ(jchats[0].from_player, 0);
            EXPECT_EQ(jchats[0].text, "gg wp!");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    host_session.close();
    joiner_session.close();
}

// 11. Desync bundle inputs.txt round trip
#include "netplay/recorder.hpp"
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
    uint64_t v = 0;
    EXPECT_TRUE(json_find_u64("{\"a\": 1, \"frame\": 1740}", "frame", v));
    EXPECT_EQ(v, 1740u);
}
