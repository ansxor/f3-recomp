#pragma once

#include "sha256.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace f3rt::netplay {

constexpr uint16_t current_proto_version = 1;
constexpr size_t max_tcp_frame_length = 4096;
constexpr size_t max_udp_packet_length = 512;
constexpr std::array<uint8_t, 4> udp_magic = {'N', '3', 'P', '1'};
constexpr size_t udp_tag_length = 16;

// TCP message types
namespace tcp_msg {
constexpr uint8_t hello         = 0x01;
constexpr uint8_t create_room   = 0x02;
constexpr uint8_t join_room     = 0x03;
constexpr uint8_t crc           = 0x04;
constexpr uint8_t result        = 0x05;
constexpr uint8_t bye           = 0x06;
constexpr uint8_t sync_blob   = 0x07;
constexpr uint8_t chat        = 0x08;

constexpr uint8_t welcome       = 0x81;
constexpr uint8_t room_created  = 0x82;
constexpr uint8_t match_start   = 0x83;
constexpr uint8_t go            = 0x84;
constexpr uint8_t desync        = 0x85;
constexpr uint8_t result_ack    = 0x86;
constexpr uint8_t peer_left     = 0x87;
constexpr uint8_t error         = 0x88;
constexpr uint8_t sync_blob_fwd = 0x89;
constexpr uint8_t chat_fwd      = 0x8A;
} // namespace tcp_msg

// UDP kinds
namespace udp_kind {
constexpr uint8_t reg    = 0;
constexpr uint8_t inputs = 1;
} // namespace udp_kind

class ByteWriter {
public:
    ByteWriter() = default;
    explicit ByteWriter(std::vector<uint8_t> &buf) : buf_(&buf) {}

    void write_u8(uint8_t val);
    void write_u16(uint16_t val);
    void write_u32(uint32_t val);
    void write_u64(uint64_t val);
    void write_bytes(const void *data, size_t len);
    void write_bytes(std::span<const uint8_t> bytes);
    void write_str(std::string_view sv);

    const std::vector<uint8_t> &data() const { return *buf_; }
    std::vector<uint8_t> &data() { return *buf_; }

private:
    std::vector<uint8_t> default_buf_;
    std::vector<uint8_t> *buf_ = &default_buf_;
};

class ByteReader {
public:
    ByteReader(const uint8_t *data, size_t len) : data_(data), len_(len), pos_(0) {}
    explicit ByteReader(std::span<const uint8_t> span) : data_(span.data()), len_(span.size()), pos_(0) {}

    bool read_u8(uint8_t &val);
    bool read_u16(uint16_t &val);
    bool read_u32(uint32_t &val);
    bool read_u64(uint64_t &val);
    bool read_bytes(void *out, size_t len);
    bool read_str(std::string &out);

    size_t remaining() const { return pos_ <= len_ ? len_ - pos_ : 0; }
    size_t position() const { return pos_; }

private:
    const uint8_t *data_;
    size_t len_;
    size_t pos_;
};

// Client -> Server structs
struct MsgHello {
    uint16_t proto_version = current_proto_version;
    std::string game_id;
    std::array<uint32_t, 7> rom_crcs{};
    std::array<uint8_t, 32> build_hash{};
    std::string player_name;
};

struct MsgCreateRoom {
    uint8_t input_delay = 2;
    uint8_t flags = 0; // bit 0 = sync blob required
};

struct MsgJoinRoom {
    std::string room_code; // 6 bytes
};

struct MsgCrc {
    uint32_t frame = 0;
    uint32_t crc = 0;
};

struct MsgResult {
    uint32_t frame = 0;
    uint8_t winner = 0xFF; // 0 P1, 1 P2, 0xFF draw
    uint32_t final_crc = 0;
};
struct MsgSyncBlob {
    std::vector<uint8_t> data; // 1 to 2048 bytes
};

// Server -> Client structs
struct MsgWelcome {
    uint16_t proto_version = 0;
};

struct MsgRoomCreated {
    std::string room_code;
};

struct MsgMatchStart {
    uint64_t session_id = 0;
    std::array<uint8_t, 32> session_key{};
    uint64_t seed = 0;
    uint8_t player_index = 0; // 0 host, 1 joiner
    uint8_t input_delay = 0;
    uint16_t udp_port = 0;
    std::string peer_name;
};

struct MsgDesync {
    uint32_t frame = 0;
    uint32_t crc_p1 = 0;
    uint32_t crc_p2 = 0;
};

struct MsgResultAck {
    uint8_t agreed = 0;
    uint8_t winner = 0; // 0, 1, 0xFF, or 0xFE if disagreed
};

struct MsgError {
    uint16_t code = 0;
    std::string message;
};

struct MsgChat {
    uint8_t from_player = 0;
    std::string text;
};

bool validate_chat_text(std::string_view text);
std::vector<uint8_t> encode_chat(std::string_view text);
bool decode_chat_fwd(std::span<const uint8_t> payload, MsgChat &msg);

// UDP structs
struct UdpInputs {
    uint32_t first_frame = 0;
    std::vector<uint16_t> inputs;
};

// Packet framing & serialization
std::vector<uint8_t> encode_tcp_frame(uint8_t type, std::span<const uint8_t> payload);

std::vector<uint8_t> encode_hello(const MsgHello &msg);
std::vector<uint8_t> encode_create_room(const MsgCreateRoom &msg);
std::vector<uint8_t> encode_join_room(const MsgJoinRoom &msg);
std::vector<uint8_t> encode_crc(const MsgCrc &msg);
std::vector<uint8_t> encode_result(const MsgResult &msg);
std::vector<uint8_t> encode_bye();
std::vector<uint8_t> encode_sync_blob(std::span<const uint8_t> blob);
bool decode_sync_blob(std::span<const uint8_t> payload, std::vector<uint8_t> &out_blob);

bool decode_welcome(std::span<const uint8_t> payload, MsgWelcome &msg);
bool decode_room_created(std::span<const uint8_t> payload, MsgRoomCreated &msg);
bool decode_match_start(std::span<const uint8_t> payload, MsgMatchStart &msg);
bool decode_desync(std::span<const uint8_t> payload, MsgDesync &msg);
bool decode_result_ack(std::span<const uint8_t> payload, MsgResultAck &msg);
bool decode_error(std::span<const uint8_t> payload, MsgError &msg);

// UDP packet encode/decode
std::vector<uint8_t> encode_udp_register(uint64_t session_id, uint8_t player_index,
                                         std::span<const uint8_t, 32> session_key);

std::vector<uint8_t> encode_udp_inputs(uint64_t session_id, uint8_t player_index,
                                       std::span<const uint8_t, 32> session_key,
                                       const UdpInputs &inputs);

bool decode_udp_packet(std::span<const uint8_t> packet,
                       std::span<const uint8_t, 32> session_key,
                       uint64_t expected_session_id,
                       uint8_t &out_player_index,
                       uint8_t &out_kind,
                       std::vector<uint8_t> &out_body);

bool decode_udp_inputs_body(std::span<const uint8_t> body, UdpInputs &out_inputs);

} // namespace f3rt::netplay
