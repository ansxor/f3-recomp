#include "protocol.hpp"

#include <algorithm>
#include <cstring>

namespace f3rt::netplay {

// ByteWriter implementations
void ByteWriter::write_u8(uint8_t val) {
    buf_->push_back(val);
}

void ByteWriter::write_u16(uint16_t val) {
    buf_->push_back(uint8_t(val >> 8));
    buf_->push_back(uint8_t(val));
}

void ByteWriter::write_u32(uint32_t val) {
    buf_->push_back(uint8_t(val >> 24));
    buf_->push_back(uint8_t(val >> 16));
    buf_->push_back(uint8_t(val >> 8));
    buf_->push_back(uint8_t(val));
}

void ByteWriter::write_u64(uint64_t val) {
    for (int i = 7; i >= 0; --i) {
        buf_->push_back(uint8_t(val >> (i * 8)));
    }
}

void ByteWriter::write_bytes(const void *data, size_t len) {
    const auto *p = static_cast<const uint8_t *>(data);
    buf_->insert(buf_->end(), p, p + len);
}

void ByteWriter::write_bytes(std::span<const uint8_t> bytes) {
    write_bytes(bytes.data(), bytes.size());
}

void ByteWriter::write_str(std::string_view sv) {
    uint8_t len = uint8_t(std::min(sv.size(), size_t(255)));
    write_u8(len);
    write_bytes(sv.data(), len);
}

// ByteReader implementations
bool ByteReader::read_u8(uint8_t &val) {
    if (pos_ + 1 > len_) return false;
    val = data_[pos_++];
    return true;
}

bool ByteReader::read_u16(uint16_t &val) {
    if (pos_ + 2 > len_) return false;
    val = (uint16_t(data_[pos_]) << 8) | uint16_t(data_[pos_ + 1]);
    pos_ += 2;
    return true;
}

bool ByteReader::read_u32(uint32_t &val) {
    if (pos_ + 4 > len_) return false;
    val = (uint32_t(data_[pos_]) << 24) |
          (uint32_t(data_[pos_ + 1]) << 16) |
          (uint32_t(data_[pos_ + 2]) << 8) |
          uint32_t(data_[pos_ + 3]);
    pos_ += 4;
    return true;
}

bool ByteReader::read_u64(uint64_t &val) {
    if (pos_ + 8 > len_) return false;
    val = 0;
    for (int i = 0; i < 8; ++i) {
        val = (val << 8) | uint64_t(data_[pos_ + i]);
    }
    pos_ += 8;
    return true;
}

bool ByteReader::read_bytes(void *out, size_t len) {
    if (pos_ + len > len_) return false;
    std::memcpy(out, data_ + pos_, len);
    pos_ += len;
    return true;
}

bool ByteReader::read_str(std::string &out) {
    uint8_t slen = 0;
    if (!read_u8(slen)) return false;
    if (pos_ + slen > len_) return false;
    out.assign(reinterpret_cast<const char *>(data_ + pos_), slen);
    pos_ += slen;
    return true;
}

// TCP framing
std::vector<uint8_t> encode_tcp_frame(uint8_t type, std::span<const uint8_t> payload) {
    std::vector<uint8_t> frame;
    ByteWriter w(frame);
    uint32_t length = uint32_t(1 + payload.size());
    w.write_u32(length);
    w.write_u8(type);
    w.write_bytes(payload);
    return frame;
}

// Client -> Server encoders
std::vector<uint8_t> encode_hello(const MsgHello &msg) {
    std::vector<uint8_t> payload;
    ByteWriter w(payload);
    w.write_u16(msg.proto_version);
    w.write_str(msg.game_id);
    for (uint32_t crc : msg.rom_crcs) {
        w.write_u32(crc);
    }
    w.write_bytes(msg.build_hash.data(), msg.build_hash.size());
    w.write_str(msg.player_name);
    return encode_tcp_frame(tcp_msg::hello, payload);
}

std::vector<uint8_t> encode_create_room(const MsgCreateRoom &msg) {
    std::vector<uint8_t> payload;
    ByteWriter w(payload);
    w.write_u8(msg.input_delay);
    w.write_u8(msg.flags);
    return encode_tcp_frame(tcp_msg::create_room, payload);
}

std::vector<uint8_t> encode_join_room(const MsgJoinRoom &msg) {
    std::vector<uint8_t> payload;
    ByteWriter w(payload);
    // 6B room_code (raw 6 bytes)
    char code_buf[6] = {' ', ' ', ' ', ' ', ' ', ' '};
    size_t copy_len = std::min(msg.room_code.size(), size_t(6));
    std::memcpy(code_buf, msg.room_code.data(), copy_len);
    w.write_bytes(code_buf, 6);
    return encode_tcp_frame(tcp_msg::join_room, payload);
}

std::vector<uint8_t> encode_crc(const MsgCrc &msg) {
    std::vector<uint8_t> payload;
    ByteWriter w(payload);
    w.write_u32(msg.frame);
    w.write_u32(msg.crc);
    return encode_tcp_frame(tcp_msg::crc, payload);
}

std::vector<uint8_t> encode_result(const MsgResult &msg) {
    std::vector<uint8_t> payload;
    ByteWriter w(payload);
    w.write_u32(msg.frame);
    w.write_u8(msg.winner);
    w.write_u32(msg.final_crc);
    return encode_tcp_frame(tcp_msg::result, payload);
}

std::vector<uint8_t> encode_bye() {
    return encode_tcp_frame(tcp_msg::bye, {});
}
std::vector<uint8_t> encode_sync_blob(std::span<const uint8_t> blob) {
    std::vector<uint8_t> payload;
    ByteWriter w(payload);
    w.write_u16(uint16_t(blob.size()));
    w.write_bytes(blob.data(), blob.size());
    return encode_tcp_frame(tcp_msg::sync_blob, payload);
}

bool validate_chat_text(std::string_view text) {
    if (text.empty() || text.size() > 256) return false;
    for (char c : text) {
        uint8_t b = static_cast<uint8_t>(c);
        if (b < 0x20 || b == 0x7F) {
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> encode_chat(std::string_view text) {
    std::vector<uint8_t> payload;
    ByteWriter w(payload);
    w.write_u16(static_cast<uint16_t>(text.size()));
    w.write_bytes(text.data(), text.size());
    return encode_tcp_frame(tcp_msg::chat, payload);
}

bool decode_sync_blob(std::span<const uint8_t> payload, std::vector<uint8_t> &out_blob) {
    ByteReader r(payload);
    uint16_t len = 0;
    if (!r.read_u16(len) || len < 1 || len > 2048) return false;
    if (r.remaining() != len) return false;
    out_blob.resize(len);
    if (!r.read_bytes(out_blob.data(), len)) return false;
    return r.remaining() == 0;
}

// Server -> Client decoders
bool decode_welcome(std::span<const uint8_t> payload, MsgWelcome &msg) {
    ByteReader r(payload);
    return r.read_u16(msg.proto_version) && r.remaining() == 0;
}

bool decode_room_created(std::span<const uint8_t> payload, MsgRoomCreated &msg) {
    if (payload.size() < 6) return false;
    msg.room_code.assign(reinterpret_cast<const char *>(payload.data()), 6);
    return true;
}

bool decode_match_start(std::span<const uint8_t> payload, MsgMatchStart &msg) {
    ByteReader r(payload);
    if (!r.read_u64(msg.session_id)) return false;
    if (!r.read_bytes(msg.session_key.data(), msg.session_key.size())) return false;
    if (!r.read_u64(msg.seed)) return false;
    if (!r.read_u8(msg.player_index)) return false;
    if (!r.read_u8(msg.input_delay)) return false;
    if (!r.read_u16(msg.udp_port)) return false;
    if (!r.read_str(msg.peer_name)) return false;
    return r.remaining() == 0;
}

bool decode_desync(std::span<const uint8_t> payload, MsgDesync &msg) {
    ByteReader r(payload);
    return r.read_u32(msg.frame) &&
           r.read_u32(msg.crc_p1) &&
           r.read_u32(msg.crc_p2) &&
           r.remaining() == 0;
}

bool decode_result_ack(std::span<const uint8_t> payload, MsgResultAck &msg) {
    ByteReader r(payload);
    return r.read_u8(msg.agreed) &&
           r.read_u8(msg.winner) &&
           r.remaining() == 0;
}

bool decode_error(std::span<const uint8_t> payload, MsgError &msg) {
    ByteReader r(payload);
    return r.read_u16(msg.code) &&
           r.read_str(msg.message) &&
           r.remaining() == 0;
}

bool decode_chat_fwd(std::span<const uint8_t> payload, MsgChat &msg) {
    ByteReader r(payload);
    if (!r.read_u8(msg.from_player)) return false;
    uint16_t len = 0;
    if (!r.read_u16(len) || len < 1 || len > 256) return false;
    if (r.remaining() != len) return false;
    msg.text.resize(len);
    if (!r.read_bytes(msg.text.data(), len)) return false;
    if (!validate_chat_text(msg.text)) return false;
    return r.remaining() == 0;
}

// UDP packet encode/decode
std::vector<uint8_t> encode_udp_register(uint64_t session_id, uint8_t player_index,
                                         std::span<const uint8_t, 32> session_key) {
    std::vector<uint8_t> pkt;
    pkt.reserve(30);
    ByteWriter w(pkt);
    w.write_bytes(udp_magic);
    w.write_u64(session_id);
    w.write_u8(player_index);
    w.write_u8(udp_kind::reg);

    // Compute 16B HMAC tag over preceding bytes
    auto tag = hmac_sha256_tag16(session_key, pkt);
    w.write_bytes(tag);
    return pkt;
}

std::vector<uint8_t> encode_udp_inputs(uint64_t session_id, uint8_t player_index,
                                       std::span<const uint8_t, 32> session_key,
                                       const UdpInputs &inputs) {
    std::vector<uint8_t> pkt;
    size_t count = std::min(inputs.inputs.size(), size_t(32));
    pkt.reserve(14 + 5 + count * 2 + 16);
    ByteWriter w(pkt);
    w.write_bytes(udp_magic);
    w.write_u64(session_id);
    w.write_u8(player_index);
    w.write_u8(udp_kind::inputs);

    // Body
    w.write_u32(inputs.first_frame);
    w.write_u8(uint8_t(count));
    for (size_t i = 0; i < count; ++i) {
        w.write_u16(inputs.inputs[i]);
    }

    // Compute 16B HMAC tag over preceding bytes
    auto tag = hmac_sha256_tag16(session_key, pkt);
    w.write_bytes(tag);
    return pkt;
}

bool decode_udp_packet(std::span<const uint8_t> packet,
                       std::span<const uint8_t, 32> session_key,
                       uint64_t expected_session_id,
                       uint8_t &out_player_index,
                       uint8_t &out_kind,
                       std::vector<uint8_t> &out_body) {
    // Min packet size: 4 magic + 8 session_id + 1 player + 1 kind + 16 tag = 30 bytes
    if (packet.size() < 30 || packet.size() > max_udp_packet_length) {
        return false;
    }

    // Verify magic
    if (std::memcmp(packet.data(), udp_magic.data(), 4) != 0) {
        return false;
    }

    // Verify tag
    size_t content_len = packet.size() - udp_tag_length;
    auto expected_tag = hmac_sha256_tag16(session_key, packet.subspan(0, content_len));
    if (std::memcmp(packet.data() + content_len, expected_tag.data(), udp_tag_length) != 0) {
        return false;
    }

    ByteReader r(packet.data() + 4, content_len - 4);
    uint64_t session_id = 0;
    if (!r.read_u64(session_id) || session_id != expected_session_id) {
        return false;
    }

    if (!r.read_u8(out_player_index)) return false;
    if (!r.read_u8(out_kind)) return false;

    out_body.assign(packet.data() + 4 + 8 + 1 + 1, packet.data() + content_len);
    return true;
}

bool decode_udp_inputs_body(std::span<const uint8_t> body, UdpInputs &out_inputs) {
    ByteReader r(body);
    if (!r.read_u32(out_inputs.first_frame)) return false;
    uint8_t count = 0;
    if (!r.read_u8(count) || count < 1 || count > 32) return false;
    out_inputs.inputs.clear();
    out_inputs.inputs.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        uint16_t val = 0;
        if (!r.read_u16(val)) return false;
        out_inputs.inputs.push_back(val);
    }
    return r.remaining() == 0;
}

} // namespace f3rt::netplay
