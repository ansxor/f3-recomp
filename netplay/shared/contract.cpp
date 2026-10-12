#include "contract.hpp"
#include <algorithm>
#include <cstring>

namespace f3rt::netplay {

NetplayConfig::NetplayConfig() {
    protocolId = PROTOCOL_ID;
    timeout = CONNECT_TOKEN_TIMEOUT_SECONDS;
    maxConnectTokenLifetime = MAX_CONNECT_TOKEN_LIFETIME;
    numChannels = NumNetplayChannels;

    // Channel 0: Reliable ordered for control messages, chat, and sync blob data block
    channel[ChannelControl].type = yojimbo::CHANNEL_TYPE_RELIABLE_ORDERED;
    channel[ChannelControl].disableBlocks = false;
    channel[ChannelControl].maxBlockSize = static_cast<int>(MAX_SYNC_BLOB_SIZE);
    channel[ChannelControl].blockFragmentSize = 1024;
    channel[ChannelControl].messageResendTime = 0.05f;      // 50 ms
    channel[ChannelControl].blockFragmentResendTime = 0.1f; // 100 ms

    // Channel 1: Unreliable unordered for 60 Hz frame inputs
    channel[ChannelInputs].type = yojimbo::CHANNEL_TYPE_UNRELIABLE_UNORDERED;
    channel[ChannelInputs].disableBlocks = true;
    channel[ChannelInputs].maxMessagesPerPacket = static_cast<int>(MAX_INPUTS_COUNT);
}

bool validate_chat_text(std::string_view text) {
    if (text.size() < MIN_CHAT_LENGTH || text.size() > MAX_CHAT_LENGTH) return false;
    for (char c : text) {
        auto b = static_cast<uint8_t>(c);
        if (b < 0x20 || b == 0x7F) {
            return false;
        }
    }
    return true;
}

bool validate_room_code(std::string_view code) {
    if (code.size() != ROOM_CODE_LENGTH) return false;
    for (char c : code) {
        if (ROOM_CODE_ALPHABET.find(c) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

bool validate_player_name(std::string_view name) {
    if (name.size() < MIN_PLAYER_NAME_LENGTH || name.size() > MAX_PLAYER_NAME_LENGTH) return false;
    for (char c : name) {
        auto b = static_cast<uint8_t>(c);
        if (b < 0x20 || b == 0x7F) {
            return false;
        }
    }
    return true;
}

bool init_netplay() { return ::InitializeYojimbo(); }

void shutdown_netplay() { ::ShutdownYojimbo(); }

std::vector<uint8_t>
encode_token_response(uint64_t client_id,
                      std::span<const uint8_t, yojimbo::ConnectTokenBytes> token) {
    std::vector<uint8_t> out(TOKEN_RESPONSE_SIZE);
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((client_id >> (i * 8)) & 0xFF);
    }
    std::memcpy(out.data() + 8, token.data(), yojimbo::ConnectTokenBytes);
    return out;
}

bool parse_token_response(std::span<const uint8_t> body, uint64_t &client_id,
                          std::array<uint8_t, yojimbo::ConnectTokenBytes> &token) {
    if (body.size() != TOKEN_RESPONSE_SIZE) {
        return false;
    }
    client_id = 0;
    for (int i = 0; i < 8; ++i) {
        client_id |= (static_cast<uint64_t>(body[i]) << (i * 8));
    }
    std::memcpy(token.data(), body.data() + 8, yojimbo::ConnectTokenBytes);
    return true;
}

} // namespace f3rt::netplay
