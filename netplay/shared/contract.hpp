#pragma once

#include "netplay/shared/default_port.hpp"
#include "yojimbo.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace f3rt::netplay {
constexpr uint16_t PROTOCOL_VERSION = 3;
constexpr uint64_t PROTOCOL_ID = 0xF3F3'0000'0000'0000ULL | PROTOCOL_VERSION;

// Token exchange constants
constexpr std::string_view TOKEN_ROUTE = "/token";
constexpr std::string_view TOKEN_VERSION_QUERY_PARAM = "v";
constexpr int HTTP_STATUS_VERSION_MISMATCH = 409;
constexpr size_t TOKEN_RESPONSE_SIZE = 8 + yojimbo::ConnectTokenBytes; // 2056 bytes
constexpr int CONNECT_TOKEN_EXPIRY_SECONDS = 30;
constexpr int CONNECT_TOKEN_TIMEOUT_SECONDS = 10;
constexpr int MAX_CONNECT_TOKEN_LIFETIME = 30;

// Protocol limits
constexpr size_t MIN_SYNC_BLOB_SIZE = 1;
constexpr size_t MAX_SYNC_BLOB_SIZE = 2048;

constexpr size_t MIN_CHAT_LENGTH = 1;
constexpr size_t MAX_CHAT_LENGTH = 256;

constexpr size_t MIN_PLAYER_NAME_LENGTH = 1;
constexpr size_t MAX_PLAYER_NAME_LENGTH = 24;

constexpr size_t ROOM_CODE_LENGTH = 6;
constexpr std::string_view ROOM_CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

constexpr size_t MIN_INPUTS_COUNT = 1;
constexpr size_t MAX_INPUTS_COUNT = 32;

constexpr size_t BUILD_HASH_SIZE = 32;
constexpr size_t ROM_CRCS_COUNT = 7;
constexpr size_t MAX_GAME_ID_LENGTH = 32;

constexpr size_t MAX_ERROR_MESSAGE_LENGTH = 256;

// Channel definitions
enum NetplayChannel {
    ChannelControl = 0, // Reliable ordered: control messages, chat, sync blob
    ChannelInputs = 1,  // Unreliable unordered: inputs
    NumNetplayChannels = 2
};

// Error codes
enum class NetplayErrorCode : uint16_t {
    None = 0,
    BadMessage = 2,
    RoomNotFound = 3,
    RoomFull = 4,
    IdentityMismatch = 5,
    NotInLobby = 6,
    ServerFull = 7,
};

// Token response binary encoding & parsing:
// Body = u64 client_id (little-endian) + 2048-byte connect token = 2056 bytes exactly.
std::vector<uint8_t>
encode_token_response(uint64_t client_id,
                      std::span<const uint8_t, yojimbo::ConnectTokenBytes> token);

bool parse_token_response(std::span<const uint8_t> body, uint64_t &client_id,
                          std::array<uint8_t, yojimbo::ConnectTokenBytes> &token);

// Validation helpers
bool validate_chat_text(std::string_view text);
bool validate_room_code(std::string_view code);
bool validate_player_name(std::string_view name);

// Library init / shutdown helpers
bool init_netplay();
void shutdown_netplay();

// Message Types
enum NetplayMessageType {
    MsgHello = 0,
    MsgCreateRoom,
    MsgJoinRoom,
    MsgWelcome,
    MsgRoomCreated,
    MsgMatchStart,
    MsgSyncBlob,
    MsgSyncBlobFwd,
    MsgGo,
    MsgCrc,
    MsgDesync,
    MsgResult,
    MsgResultAck,
    MsgChat,
    MsgChatFwd,
    MsgPeerLeft,
    MsgError,
    MsgBye,
    MsgInputs,
    NumNetplayMessageTypes
};

// --- Messages ---

struct HelloMessage : public yojimbo::Message {
    char game_id[MAX_GAME_ID_LENGTH + 1]{};
    uint32_t rom_crcs[ROM_CRCS_COUNT]{};
    uint8_t build_hash[BUILD_HASH_SIZE]{};
    char player_name[MAX_PLAYER_NAME_LENGTH + 1]{};

    HelloMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream &stream) {
        serialize_string(stream, game_id, sizeof(game_id));
        for (size_t i = 0; i < ROM_CRCS_COUNT; ++i) {
            serialize_uint32(stream, rom_crcs[i]);
        }
        serialize_bytes(stream, build_hash, sizeof(build_hash));
        serialize_string(stream, player_name, sizeof(player_name));
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct CreateRoomMessage : public yojimbo::Message {
    uint8_t input_delay = 2; // 1 to 8
    uint8_t flags = 0;       // bit 0 = sync blob required

    CreateRoomMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream &stream) {
        if (Stream::IsWriting && (input_delay < 1 || input_delay > 8)) {
            return false;
        }
        serialize_int(stream, input_delay, 1, 8);
        if (Stream::IsReading && (input_delay < 1 || input_delay > 8)) {
            return false;
        }
        serialize_bits(stream, flags, 8);
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct JoinRoomMessage : public yojimbo::Message {
    char room_code[ROOM_CODE_LENGTH + 1]{};

    JoinRoomMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream &stream) {
        serialize_string(stream, room_code, sizeof(room_code));
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct WelcomeMessage : public yojimbo::Message {
    WelcomeMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream & /*stream*/) {
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct RoomCreatedMessage : public yojimbo::Message {
    char room_code[ROOM_CODE_LENGTH + 1]{};

    RoomCreatedMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream &stream) {
        serialize_string(stream, room_code, sizeof(room_code));
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct MatchStartMessage : public yojimbo::Message {
    uint64_t session_id = 0;
    uint64_t seed = 0;
    uint8_t player_index = 0; // 0 host, 1 joiner
    uint8_t input_delay = 2;
    char peer_name[MAX_PLAYER_NAME_LENGTH + 1]{};

    MatchStartMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream &stream) {
        serialize_uint64(stream, session_id);
        serialize_uint64(stream, seed);
        serialize_int(stream, player_index, 0, 1);
        serialize_int(stream, input_delay, 1, 8);
        serialize_string(stream, peer_name, sizeof(peer_name));
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct SyncBlobMessage : public yojimbo::BlockMessage {
    SyncBlobMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream & /*stream*/) {
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct SyncBlobFwdMessage : public yojimbo::BlockMessage {
    SyncBlobFwdMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream & /*stream*/) {
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct GoMessage : public yojimbo::Message {
    GoMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream & /*stream*/) {
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct CrcMessage : public yojimbo::Message {
    uint32_t frame = 0;
    uint32_t crc = 0;

    CrcMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream &stream) {
        serialize_uint32(stream, frame);
        serialize_uint32(stream, crc);
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct DesyncMessage : public yojimbo::Message {
    uint32_t frame = 0;
    uint32_t crc_p1 = 0;
    uint32_t crc_p2 = 0;

    DesyncMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream &stream) {
        serialize_uint32(stream, frame);
        serialize_uint32(stream, crc_p1);
        serialize_uint32(stream, crc_p2);
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct ResultMessage : public yojimbo::Message {
    uint32_t frame = 0;
    uint8_t winner = 0xFF; // 0 P1, 1 P2, 0xFF draw
    uint32_t final_crc = 0;

    ResultMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream &stream) {
        serialize_uint32(stream, frame);
        serialize_uint8(stream, winner);
        serialize_uint32(stream, final_crc);
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct ResultAckMessage : public yojimbo::Message {
    bool agreed = false;
    uint8_t winner = 0xFE; // 0 P1, 1 P2, 0xFF draw, 0xFE disagreed

    ResultAckMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream &stream) {
        serialize_bool(stream, agreed);
        serialize_uint8(stream, winner);
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct ChatTextMessage : public yojimbo::Message {
    char text[MAX_CHAT_LENGTH + 1]{};

    ChatTextMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream &stream) {
        serialize_string(stream, text, sizeof(text));
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct ChatFwdMessage : public yojimbo::Message {
    uint8_t from_player = 0;
    char text[MAX_CHAT_LENGTH + 1]{};

    ChatFwdMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream &stream) {
        serialize_int(stream, from_player, 0, 1);
        serialize_string(stream, text, sizeof(text));
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct PeerLeftMessage : public yojimbo::Message {
    PeerLeftMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream & /*stream*/) {
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct ErrorMessage : public yojimbo::Message {
    uint16_t code = 0;
    char message[MAX_ERROR_MESSAGE_LENGTH + 1]{};

    ErrorMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream &stream) {
        serialize_uint16(stream, code);
        serialize_string(stream, message, sizeof(message));
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct ByeMessage : public yojimbo::Message {
    ByeMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream & /*stream*/) {
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

struct InputsMessage : public yojimbo::Message {
    uint32_t first_frame = 0;
    uint8_t count = 0;
    uint16_t inputs[MAX_INPUTS_COUNT]{};

    InputsMessage() = default;

    template <typename Stream>
    bool Serialize( // NOLINT(readability-identifier-naming): required by Yojimbo message interface
        Stream &stream) {
        if (Stream::IsWriting && (count < static_cast<int>(MIN_INPUTS_COUNT) ||
                                  count > static_cast<int>(MAX_INPUTS_COUNT))) {
            return false;
        }
        serialize_uint32(stream, first_frame);
        serialize_int(stream, count, static_cast<int>(MIN_INPUTS_COUNT),
                      static_cast<int>(MAX_INPUTS_COUNT));
        if (Stream::IsReading && (count < static_cast<int>(MIN_INPUTS_COUNT) ||
                                  count > static_cast<int>(MAX_INPUTS_COUNT))) {
            return false;
        }
        for (int i = 0; i < count; ++i) {
            serialize_uint16(stream, inputs[i]);
        }
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

// Message Factory
YOJIMBO_MESSAGE_FACTORY_START(NetplayMessageFactory, NumNetplayMessageTypes);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgHello, HelloMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgCreateRoom, CreateRoomMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgJoinRoom, JoinRoomMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgWelcome, WelcomeMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgRoomCreated, RoomCreatedMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgMatchStart, MatchStartMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgSyncBlob, SyncBlobMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgSyncBlobFwd, SyncBlobFwdMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgGo, GoMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgCrc, CrcMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgDesync, DesyncMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgResult, ResultMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgResultAck, ResultAckMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgChat, ChatTextMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgChatFwd, ChatFwdMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgPeerLeft, PeerLeftMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgError, ErrorMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgBye, ByeMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(MsgInputs, InputsMessage);
YOJIMBO_MESSAGE_FACTORY_FINISH();

// Adapter
class NetplayAdapter : public yojimbo::Adapter {
public:
    yojimbo::MessageFactory *CreateMessageFactory(yojimbo::Allocator &allocator) override {
        return YOJIMBO_NEW(allocator, NetplayMessageFactory, allocator);
    }
};

// ClientServerConfig subclass
struct NetplayConfig : public yojimbo::ClientServerConfig {
    NetplayConfig();
};

} // namespace f3rt::netplay
