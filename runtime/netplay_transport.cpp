#include "f3rt/netplay_transport.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <array>

namespace f3rt::netplay {

namespace {

constexpr uint32_t kMagic = 0x46334E50; // 'F', '3', 'N', 'P'
constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kHeaderSize = 20;
constexpr size_t kIdentitySize = 72;
constexpr size_t kMaxPacketSize = 1400;
constexpr uint16_t kInputMask = 0x07FF;
constexpr size_t kMaxUnackedInputs = 512;
constexpr size_t kInputRingCapacity = 2048;
constexpr size_t kChecksumRingCapacity = 512;
constexpr size_t kPingTableCapacity = 64;
constexpr int64_t kMinSendIntervalMs = 8; // Pace outgoing transmissions (max 125 pps)

// Packet Types
constexpr uint8_t kPktJoinReq = 1;
constexpr uint8_t kPktJoinWait = 2;
constexpr uint8_t kPktJoinReject = 4;
constexpr uint8_t kPktMatchStart = 5;
constexpr uint8_t kPktGameData = 6;
constexpr uint8_t kPktHeartbeat = 7;
constexpr uint8_t kPktLeave = 8;
constexpr uint8_t kPktMatchTerminated = 9;
constexpr uint8_t kPktMatchComplete = 10; // Server-origin persistent match completion verdict

// Flags
constexpr uint16_t kFlagFinishReq = 1 << 0;
constexpr uint16_t kFlagFinishAck = 1 << 1;

// Leave codes
constexpr uint8_t kLeaveAbort = 0;
constexpr uint8_t kLeaveNormalFinished = 1;

// Alignment-safe big-endian readers (avoids unaligned reinterpret_cast UB)
inline uint16_t read_u16_be(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                                 static_cast<uint16_t>(p[1]));
}

inline uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
            static_cast<uint32_t>(p[3]);
}

inline uint64_t read_u64_be(const uint8_t* p) {
    return (static_cast<uint64_t>(read_u32_be(p)) << 32) |
            static_cast<uint64_t>(read_u32_be(p + 4));
}

// Alignment-safe big-endian writers
inline void write_u16_be(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

inline void write_u32_be(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

inline void write_u64_be(uint8_t* p, uint64_t v) {
    write_u32_be(p, static_cast<uint32_t>(v >> 32));
    write_u32_be(p + 4, static_cast<uint32_t>(v));
}

std::string reject_reason_to_string(uint8_t code) {
    switch (code) {
        case 1: return "protocol mismatch";
        case 2: return "room full (max 2 players)";
        case 3: return "requested slot already taken";
        case 4: return "ROM CRC mismatch";
        case 5: return "build hash mismatch";
        case 6: return "settings mismatch";
        case 7: return "EEPROM CRC mismatch";
        case 8: return "initial state CRC mismatch";
        case 9: return "delay configuration mismatch";
        case 10: return "invalid room name";
        case 11: return "rate limited";
        case 12: return "match already in progress";
        case 13: return "invalid identity payload";
        default: return "unknown reject code (" + std::to_string(code) + ")";
    }
}

} // namespace

struct Transport::Impl {
    TransportOptions opts;
    Identity identity;

    int sock_fd = -1;

    enum class State {
        Connecting,
        Connected,
        Terminated
    };
    State state = State::Connecting;

    uint64_t client_nonce = 0;
    uint64_t session_id = 0;
    unsigned assigned_slot = 0;
    bool is_ready = false;
    bool is_finished = false;

    // Server-origin verified match completion state
    bool server_match_completed = false;
    uint32_t server_final_frame = 0;
    uint32_t server_final_crc = 0;

    // Timing
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    TimePoint start_time;
    TimePoint last_send_time;
    TimePoint last_recv_time;
    TimePoint last_server_reply_time;

    // Ping / RTT tracking (fixed ring buffer, zero dynamic allocation)
    struct PingEntry {
        uint32_t id = 0;
        TimePoint sent_time;
    };
    std::array<PingEntry, kPingTableCapacity> active_pings{};
    uint32_t ping_counter = 0;
    uint32_t latest_peer_ping_id = 0;
    double smoothed_rtt_ms = 0.0;

    // Simulation & frame state
    uint32_t local_simulated_frame = 0;
    uint32_t local_confirmed_frame = 0;
    uint32_t peer_simulated_frame = 0; // Monotonically increasing
    uint32_t peer_ack_frame = 0xFFFFFFFF; // Highest input frame confirmed by peer

    // Local inputs ring buffer: frame -> word (fixed size, zero heap allocation)
    struct LocalInputSlot {
        uint32_t frame = 0xFFFFFFFF;
        uint16_t word = 0;
    };
    std::array<LocalInputSlot, kInputRingCapacity> submitted_inputs{};
    uint32_t latest_submitted_frame = 0;
    bool has_submitted_inputs = false;

    // Remote inputs ring buffer (fixed size, zero heap allocation; delivered flag preserves history)
    struct RemoteInputSlot {
        uint32_t frame = 0xFFFFFFFF;
        uint16_t word = 0;
        bool delivered = false;
    };
    uint32_t next_expected_receive_frame = 0;
    std::array<RemoteInputSlot, kInputRingCapacity> received_inputs{};

    // Checksum tracking (fixed rings, zero heap allocation)
    struct ChecksumSlot {
        uint32_t frame = 0xFFFFFFFF;
        uint32_t crc = 0;
    };
    std::array<ChecksumSlot, kChecksumRingCapacity> local_checksums{};
    bool has_submitted_checksums = false;
    uint32_t latest_submitted_checksum_frame = 0;

    // Reliable checksum frontier
    uint32_t peer_ack_checksum_frame = 0xFFFFFFFF; // Highest checksum frame confirmed by peer
    uint32_t latest_received_checksum_frame = 0xFFFFFFFF; // Highest contiguous checksum frame received from peer

    // Outgoing unacked checksums queue (fixed ring)
    std::array<Checksum, kChecksumRingCapacity> outgoing_checksums{};
    size_t out_cs_head = 0;
    size_t out_cs_tail = 0;
    size_t out_cs_count = 0;

    // Incoming checksums queue (fixed ring)
    std::array<Checksum, kChecksumRingCapacity> incoming_checksums{};
    size_t in_cs_head = 0;
    size_t in_cs_tail = 0;
    size_t in_cs_count = 0;

    // Finish handshake
    bool finish_requested = false;
    uint32_t local_finish_frame = 0;
    uint32_t local_finish_crc = 0;

    bool peer_finish_received = false;
    uint32_t peer_finish_frame = 0;
    uint32_t peer_finish_crc = 0;
    bool peer_finish_acked = false;

    // Stack/member transmission buffer (zero allocation per send)
    std::array<uint8_t, kMaxPacketSize> packet_buf{};

    Impl(const TransportOptions& o, const Identity& id)
        : opts(o), identity(id) {
        if (opts.room.empty()) {
            throw std::invalid_argument("netplay room name cannot be empty");
        }
        if (opts.room.size() > 32) {
            throw std::invalid_argument("netplay room name must be 1 to 32 characters, got " + std::to_string(opts.room.size()));
        }
        for (char c : opts.room) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
                throw std::invalid_argument("netplay room name contains invalid characters: " + opts.room);
            }
        }
        if (opts.player > 2) {
            throw std::invalid_argument("netplay player option must be 0 (auto), 1, or 2, got " + std::to_string(opts.player));
        }
        if (opts.delay > 8) {
            throw std::invalid_argument("netplay delay must be between 0 and 8, got " + std::to_string(opts.delay));
        }

        next_expected_receive_frame = opts.delay;
        peer_ack_frame = (opts.delay == 0) ? 0xFFFFFFFF : (opts.delay - 1);

        // Generate client nonce
        std::random_device rd;
        std::mt19937_64 gen(rd());
        client_nonce = gen();
        if (client_nonce == 0) client_nonce = 1;

        init_socket();

        start_time = Clock::now();
        last_send_time = start_time;
        last_recv_time = start_time;
        last_server_reply_time = start_time;

        // Send initial JoinReq
        send_join_req();
    }

    ~Impl() {
        if (sock_fd >= 0) {
            if (state == State::Connected && session_id != 0) {
                // If finished cleanly, inform server with LeaveNormalFinished so peer is not interrupted!
                uint8_t leave_code = is_finished ? kLeaveNormalFinished : kLeaveAbort;
                send_leave(leave_code);
            }
            close(sock_fd);
            sock_fd = -1;
        }
    }

    void init_socket() {
        std::string host = opts.server;
        std::string port_str = "9000";

        auto colon_pos = host.rfind(':');
        if (colon_pos != std::string::npos) {
            port_str = host.substr(colon_pos + 1);
            host = host.substr(0, colon_pos);
        }

        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        struct addrinfo* res = nullptr;
        int status = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
        if (status != 0 || res == nullptr) {
            throw std::runtime_error("failed to resolve netplay server address: " + host + ":" + port_str + " (" + gai_strerror(status) + ")");
        }

        sock_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock_fd < 0) {
            freeaddrinfo(res);
            throw std::runtime_error("failed to create UDP socket: " + std::string(strerror(errno)));
        }

        // Set non-blocking
        int flags = fcntl(sock_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            freeaddrinfo(res);
            close(sock_fd);
            throw std::runtime_error("failed to set non-blocking UDP socket: " + std::string(strerror(errno)));
        }

        // Connect UDP socket to server address:
        // Enforces that recv() ONLY accepts packets from the resolved server endpoint!
        if (connect(sock_fd, res->ai_addr, res->ai_addrlen) < 0) {
            freeaddrinfo(res);
            close(sock_fd);
            throw std::runtime_error("failed to connect UDP socket to server: " + std::string(strerror(errno)));
        }

        freeaddrinfo(res);
    }

    void send_raw(size_t len) {
        if (sock_fd < 0 || len == 0 || len > kMaxPacketSize) return;
        send(sock_fd, packet_buf.data(), len, 0);
        last_send_time = Clock::now();
    }

    void send_join_req() {
        size_t total_len = kHeaderSize + 8 + 1 + 1 + 1 + 32 + kIdentitySize;

        // Header
        write_u32_be(&packet_buf[0], kMagic);
        packet_buf[4] = kProtocolVersion;
        packet_buf[5] = kPktJoinReq;
        write_u16_be(&packet_buf[6], 0);
        write_u64_be(&packet_buf[8], 0);
        packet_buf[16] = 0xFF; // Unassigned slot
        packet_buf[17] = packet_buf[18] = packet_buf[19] = 0;

        // Payload
        size_t off = kHeaderSize;
        write_u64_be(&packet_buf[off], client_nonce); off += 8;

        // opts.player: 0=auto, 1=slot 0, 2=slot 1
        packet_buf[off++] = static_cast<uint8_t>(opts.player);
        packet_buf[off++] = static_cast<uint8_t>(opts.delay);

        // Room name (length + 32 bytes ASCII)
        uint8_t rlen = static_cast<uint8_t>(opts.room.size());
        packet_buf[off++] = rlen;
        std::memset(&packet_buf[off], 0, 32);
        std::memcpy(&packet_buf[off], opts.room.data(), rlen);
        off += 32;

        // Identity
        for (size_t i = 0; i < 7; ++i) {
            write_u32_be(&packet_buf[off + i * 4], identity.rom_crc[i]);
        }
        off += 28;
        std::memcpy(&packet_buf[off], identity.build_hash.data(), 32);
        off += 32;
        write_u32_be(&packet_buf[off], identity.settings); off += 4;
        write_u32_be(&packet_buf[off], identity.eeprom_crc); off += 4;
        write_u32_be(&packet_buf[off], identity.initial_crc); off += 4;

        send_raw(total_len);
    }

    void send_game_data() {
        if (state != State::Connected) return;

        // Record ping in fixed ring
        ping_counter++;
        size_t ping_idx = ping_counter % kPingTableCapacity;
        active_pings[ping_idx] = {ping_counter, Clock::now()};

        uint16_t flags = 0;
        if (finish_requested) {
            flags |= kFlagFinishReq;
            if (peer_finish_received) {
                flags |= kFlagFinishAck;
            }
        }

        // Determine input range to send: from peer_ack_frame + 1 up to latest_submitted_frame
        uint32_t input_start = (peer_ack_frame == 0xFFFFFFFF) ? opts.delay : (peer_ack_frame + 1);
        if (input_start < opts.delay) input_start = opts.delay;

        uint16_t input_count = 0;
        std::array<uint16_t, 128> inputs_staging{};
        if (has_submitted_inputs && input_start <= latest_submitted_frame) {
            for (uint32_t f = input_start; f <= latest_submitted_frame && input_count < 128; ++f) {
                size_t idx = f % kInputRingCapacity;
                if (submitted_inputs[idx].frame == f) {
                    inputs_staging[input_count++] = submitted_inputs[idx].word;
                } else {
                    break;
                }
            }
        }

        // Checksums to send: send up to 8 unacked outgoing checksums
        uint16_t cs_count = 0;
        std::array<Checksum, 8> cs_staging{};
        for (size_t i = 0; i < out_cs_count && cs_count < 8; ++i) {
            size_t idx = (out_cs_head + i) % kChecksumRingCapacity;
            if (peer_ack_checksum_frame == 0xFFFFFFFF || outgoing_checksums[idx].frame > peer_ack_checksum_frame) {
                cs_staging[cs_count++] = outgoing_checksums[idx];
            }
        }

        size_t payload_len = 4 + 4 + 4 + 4 + 4 + 2 + 4 + 2 + input_count * 2 + 2 + cs_count * 8;
        if (flags & kFlagFinishReq) {
            payload_len += 8;
        }

        size_t total_len = kHeaderSize + payload_len;
        if (total_len > kMaxPacketSize) return;

        // Header
        write_u32_be(&packet_buf[0], kMagic);
        packet_buf[4] = kProtocolVersion;
        packet_buf[5] = kPktGameData;
        write_u16_be(&packet_buf[6], flags);
        write_u64_be(&packet_buf[8], session_id);
        packet_buf[16] = static_cast<uint8_t>(assigned_slot);
        packet_buf[17] = packet_buf[18] = packet_buf[19] = 0;

        // Payload
        size_t off = kHeaderSize;
        write_u32_be(&packet_buf[off], local_simulated_frame); off += 4;

        // Ack frame: highest contiguous input frame received from peer
        uint32_t ack_f = (next_expected_receive_frame == 0) ? 0xFFFFFFFF : (next_expected_receive_frame - 1);
        write_u32_be(&packet_buf[off], ack_f); off += 4;

        // Reliable checksum ACK frontier: highest contiguous checksum received
        write_u32_be(&packet_buf[off], latest_received_checksum_frame); off += 4;

        write_u32_be(&packet_buf[off], ping_counter); off += 4;
        write_u32_be(&packet_buf[off], latest_peer_ping_id); off += 4;
        write_u16_be(&packet_buf[off], flags); off += 2;
        write_u32_be(&packet_buf[off], input_start); off += 4;
        write_u16_be(&packet_buf[off], input_count); off += 2;

        for (uint16_t i = 0; i < input_count; ++i) {
            write_u16_be(&packet_buf[off], inputs_staging[i]); off += 2;
        }

        write_u16_be(&packet_buf[off], cs_count); off += 2;
        for (uint16_t i = 0; i < cs_count; ++i) {
            write_u32_be(&packet_buf[off], cs_staging[i].frame); off += 4;
            write_u32_be(&packet_buf[off], cs_staging[i].crc); off += 4;
        }

        if (flags & kFlagFinishReq) {
            write_u32_be(&packet_buf[off], local_finish_frame); off += 4;
            write_u32_be(&packet_buf[off], local_finish_crc); off += 4;
        }

        send_raw(total_len);
    }

    void send_leave(uint8_t leave_code) {
        write_u32_be(&packet_buf[0], kMagic);
        packet_buf[4] = kProtocolVersion;
        packet_buf[5] = kPktLeave;
        write_u16_be(&packet_buf[6], 0);
        write_u64_be(&packet_buf[8], session_id);
        packet_buf[16] = static_cast<uint8_t>(assigned_slot);
        packet_buf[17] = packet_buf[18] = packet_buf[19] = 0;

        packet_buf[kHeaderSize] = leave_code;
        std::memset(&packet_buf[kHeaderSize + 1], 0, 64);
        send_raw(kHeaderSize + 65);
    }

    void handle_packet(const uint8_t* data, size_t len) {
        if (len < kHeaderSize) return;

        uint32_t magic = read_u32_be(&data[0]);
        if (magic != kMagic) return;

        uint8_t version = data[4];
        if (version != kProtocolVersion) return;

        uint8_t type = data[5];
        uint16_t flags = read_u16_be(&data[6]);
        uint64_t sess = read_u64_be(&data[8]);
        uint8_t slot = data[16];

        switch (type) {
            case kPktJoinWait: {
                if (state == State::Connecting) {
                    if (len >= kHeaderSize + 8 + 1) {
                        uint64_t nonce = read_u64_be(&data[kHeaderSize]);
                        if (nonce == client_nonce) {
                            assigned_slot = data[kHeaderSize + 8];
                            last_server_reply_time = Clock::now();
                            last_recv_time = last_server_reply_time;
                        }
                    }
                }
                break;
            }

            case kPktJoinReject: {
                if (len >= kHeaderSize + 8 + 1) {
                    uint64_t nonce = read_u64_be(&data[kHeaderSize]);
                    if (nonce == client_nonce) {
                        uint8_t code = data[kHeaderSize + 8];
                        std::string msg;
                        if (len > kHeaderSize + 9) {
                            size_t msg_len = std::min(size_t(64), len - (kHeaderSize + 9));
                            msg = std::string(reinterpret_cast<const char*>(&data[kHeaderSize + 9]), msg_len);
                            auto np = msg.find('\0');
                            if (np != std::string::npos) msg.resize(np);
                        }
                        state = State::Terminated;
                        throw std::runtime_error("netplay join rejected: " + reject_reason_to_string(code) + " (" + msg + ")");
                    }
                }
                break;
            }

            case kPktMatchStart: {
                if (state == State::Connecting) {
                    if (len < kHeaderSize + 8 + 1 + 1 + 2 + kIdentitySize) return;
                    uint64_t nonce = read_u64_be(&data[kHeaderSize]);
                    if (nonce != client_nonce) return;

                    uint8_t slot_val = data[kHeaderSize + 8];
                    if (slot_val > 1) {
                        state = State::Terminated;
                        throw std::runtime_error("netplay invalid slot assigned: " + std::to_string(slot_val));
                    }
                    if (sess == 0) {
                        state = State::Terminated;
                        throw std::runtime_error("netplay invalid session id 0");
                    }

                    uint8_t negotiated_delay = data[kHeaderSize + 9];
                    if (negotiated_delay != opts.delay) {
                        state = State::Terminated;
                        throw std::runtime_error("netplay delay negotiation mismatch: expected " +
                                                 std::to_string(opts.delay) + ", got " + std::to_string(negotiated_delay));
                    }

                    // Validate peer identity from payload
                    size_t id_off = kHeaderSize + 8 + 1 + 1 + 2;
                    Identity peer_id{};
                    for (size_t i = 0; i < 7; ++i) {
                        peer_id.rom_crc[i] = read_u32_be(&data[id_off + i * 4]);
                    }
                    id_off += 28;
                    std::memcpy(peer_id.build_hash.data(), &data[id_off], 32);
                    id_off += 32;
                    peer_id.settings = read_u32_be(&data[id_off]); id_off += 4;
                    peer_id.eeprom_crc = read_u32_be(&data[id_off]); id_off += 4;
                    peer_id.initial_crc = read_u32_be(&data[id_off]); id_off += 4;

                    if (peer_id.rom_crc != identity.rom_crc ||
                        peer_id.build_hash != identity.build_hash ||
                        peer_id.settings != identity.settings ||
                        peer_id.eeprom_crc != identity.eeprom_crc ||
                        peer_id.initial_crc != identity.initial_crc) {
                        state = State::Terminated;
                        throw std::runtime_error("netplay handshake peer identity mismatch");
                    }

                    assigned_slot = slot_val;
                    session_id = sess;
                    state = State::Connected;
                    is_ready = true;
                    last_recv_time = Clock::now();
                }
                break;
            }

            case kPktGameData: {
                // Must match active session and be from opposite slot
                if (state != State::Connected || sess != session_id) return;
                if (slot != (1 - assigned_slot)) return;

                // Validate and process without updating liveness if malformed
                validate_and_handle_game_data(data + kHeaderSize, len - kHeaderSize, flags);
                break;
            }

            case kPktHeartbeat: {
                if (state != State::Connected || sess != session_id) return;
                if (slot != (1 - assigned_slot)) return;
                if (len < kHeaderSize + 16) return;

                last_recv_time = Clock::now();
                const uint8_t* p = data + kHeaderSize;
                uint32_t peer_ping = read_u32_be(&p[0]);
                uint32_t peer_pong = read_u32_be(&p[4]);
                uint32_t sim_f = read_u32_be(&p[8]);
                uint32_t ack_f = read_u32_be(&p[12]);

                latest_peer_ping_id = peer_ping;
                if (sim_f > peer_simulated_frame) {
                    peer_simulated_frame = sim_f; // Monotonic max
                }
                process_pong(peer_pong);
                process_peer_ack(ack_f);
                break;
            }

            case kPktMatchComplete: {
                // Server-origin verified match verdict
                if (sess != session_id) return;
                if (len < kHeaderSize + 8) return;

                last_recv_time = Clock::now();
                uint32_t final_frame = read_u32_be(&data[kHeaderSize]);
                uint32_t final_crc = read_u32_be(&data[kHeaderSize + 4]);

                server_match_completed = true;
                server_final_frame = final_frame;
                server_final_crc = final_crc;

                if (finish_requested) {
                    if (final_frame != local_finish_frame || final_crc != local_finish_crc) {
                        state = State::Terminated;
                        throw std::runtime_error("netplay finish CRC mismatch with server verdict: local frame " +
                                                 std::to_string(local_finish_frame) + " crc 0x" + to_hex(local_finish_crc) +
                                                 " != server frame " + std::to_string(final_frame) +
                                                 " crc 0x" + to_hex(final_crc));
                    }
                    is_finished = true;
                }
                break;
            }

            case kPktMatchTerminated: {
                if (sess == session_id) {
                    state = State::Terminated;
                    std::string msg = "match terminated by server";
                    if (len > kHeaderSize + 1) {
                        size_t msg_len = std::min(size_t(64), len - (kHeaderSize + 1));
                        msg = std::string(reinterpret_cast<const char*>(&data[kHeaderSize + 1]), msg_len);
                        auto np = msg.find('\0');
                        if (np != std::string::npos) msg.resize(np);
                    }
                    throw std::runtime_error("netplay disconnected: " + msg);
                }
                break;
            }

            default:
                break;
        }
    }

    bool validate_and_handle_game_data(const uint8_t* p, size_t len, uint16_t flags) {
        // Minimum payload: 4 (sim) + 4 (ack_f) + 4 (ack_cs) + 4 (ping) + 4 (pong) + 2 (flags) + 4 (input_start) + 2 (input_count) + 2 (cs_count) = 30 bytes
        if (len < 30) return false;

        // Flags validation: no unknown bits
        if (flags & ~(kFlagFinishReq | kFlagFinishAck)) return false;

        size_t off = 0;
        uint32_t peer_sim = read_u32_be(&p[off]); off += 4;
        uint32_t ack_f = read_u32_be(&p[off]); off += 4;
        uint32_t ack_cs = read_u32_be(&p[off]); off += 4;
        uint32_t peer_ping = read_u32_be(&p[off]); off += 4;
        uint32_t peer_pong = read_u32_be(&p[off]); off += 4;
        uint16_t p_flags = read_u16_be(&p[off]); off += 2;
        if (p_flags != flags) return false;

        uint32_t input_start = read_u32_be(&p[off]); off += 4;
        uint16_t input_count = read_u16_be(&p[off]); off += 2;
        if (input_count > 128) return false;

        // Exact bounds check for inputs
        if (off + input_count * 2 > len) return false;
        const size_t input_offset = off;

        // Verify input start frame
        if (input_count > 0 && input_start < opts.delay) return false;
        // Verify no overflow in frame range
        if (uint64_t(input_start) + input_count > 0xFFFFFFFFULL) return false;

        // Verify input word bitmasks
        for (uint16_t i = 0; i < input_count; ++i) {
            uint16_t word = read_u16_be(&p[off + i * 2]);
            if (word & ~kInputMask) {
                throw std::runtime_error("netplay peer sent invalid input word: 0x" + to_hex(word));
            }
        }
        off += input_count * 2;

        if (off + 2 > len) return false;
        uint16_t cs_count = read_u16_be(&p[off]); off += 2;
        if (cs_count > 32) return false;
        if (off + cs_count * 8 > len) return false;
        const size_t checksum_offset = off;

        // Validate checksum frames
        for (uint16_t i = 0; i < cs_count; ++i) {
            uint32_t cs_f = read_u32_be(&p[off + i * 8]);
            if (cs_f < opts.delay) return false;
        }
        off += cs_count * 8;

        // Finish payload validation
        bool has_finish = (flags & kFlagFinishReq) != 0;
        const size_t finish_offset = off;
        if (has_finish) {
            if (off + 8 > len) return false;
            off += 8;
        }

        // Exact payload length check
        if (off != len) return false;

        // Validate ACK frame
        if (ack_f != 0xFFFFFFFF) {
            if (has_submitted_inputs && ack_f > latest_submitted_frame) {
                return false; // Invalid ACK exceeding latest submitted
            }
        }

        // Validate Checksum ACK frame
        if (ack_cs != 0xFFFFFFFF) {
            if (has_submitted_checksums && ack_cs > latest_submitted_checksum_frame) {
                return false; // Invalid Checksum ACK exceeding latest submitted
            }
        }

        // -------------------------------------------------------------
        // PASS 2: All checks succeeded! Now perform state mutations:
        // -------------------------------------------------------------
        last_recv_time = Clock::now();

        if (peer_sim > peer_simulated_frame) {
            peer_simulated_frame = peer_sim; // Monotonic max
        }

        latest_peer_ping_id = peer_ping;
        process_pong(peer_pong);

        if (ack_f != 0xFFFFFFFF) {
            process_peer_ack(ack_f);
        }
        if (ack_cs != 0xFFFFFFFF) {
            process_peer_checksum_ack(ack_cs);
        }

        // Apply inputs and check mutation against preserved history
        size_t in_off = input_offset;
        for (uint16_t i = 0; i < input_count; ++i) {
            uint16_t word = read_u16_be(&p[in_off]); in_off += 2;
            uint32_t frame = input_start + i;

            size_t idx = frame % kInputRingCapacity;
            if (received_inputs[idx].frame == frame) {
                // Consistency check against history!
                if (received_inputs[idx].word != word) {
                    throw std::runtime_error("netplay input mutation detected at frame " + std::to_string(frame) +
                                             ": original 0x" + to_hex(received_inputs[idx].word) +
                                             " != new 0x" + to_hex(word));
                }
            } else {
                received_inputs[idx] = {frame, word, false};
            }
        }

        // Apply checksums: dedup and contiguous frontier
        size_t cs_off = checksum_offset;
        for (uint16_t i = 0; i < cs_count; ++i) {
            uint32_t cs_f = read_u32_be(&p[cs_off]); cs_off += 4;
            uint32_t cs_c = read_u32_be(&p[cs_off]); cs_off += 4;

            // Check against local computed checksum if present
            size_t loc_idx = cs_f % kChecksumRingCapacity;
            if (local_checksums[loc_idx].frame == cs_f) {
                if (local_checksums[loc_idx].crc != cs_c) {
                    throw std::runtime_error("netplay desync detected at frame " + std::to_string(cs_f) +
                                             ": local CRC 0x" + to_hex(local_checksums[loc_idx].crc) +
                                             " != remote CRC 0x" + to_hex(cs_c));
                }
            }

            // Dedup check in incoming checksums queue
            bool already_queued = false;
            for (size_t q = 0; q < in_cs_count; ++q) {
                if (incoming_checksums[(in_cs_head + q) % kChecksumRingCapacity].frame == cs_f) {
                    already_queued = true;
                    break;
                }
            }

            if (!already_queued && in_cs_count < kChecksumRingCapacity) {
                incoming_checksums[in_cs_tail] = {cs_f, cs_c};
                in_cs_tail = (in_cs_tail + 1) % kChecksumRingCapacity;
                in_cs_count++;

                // Contiguous frontier: only advance if cs_f is contiguous or monotonic
                if (latest_received_checksum_frame == 0xFFFFFFFF || cs_f > latest_received_checksum_frame) {
                    latest_received_checksum_frame = cs_f;
                }
            }
        }

        // Process finish payload if present
        if (has_finish) {
            uint32_t f_frame = read_u32_be(&p[finish_offset]);
            uint32_t f_crc = read_u32_be(&p[finish_offset + 4]);

            peer_finish_received = true;
            peer_finish_frame = f_frame;
            peer_finish_crc = f_crc;

            if (flags & kFlagFinishAck) {
                peer_finish_acked = true;
            }

            if (finish_requested) {
                if (peer_finish_frame != local_finish_frame || peer_finish_crc != local_finish_crc) {
                    throw std::runtime_error("netplay finish CRC mismatch: local frame " +
                                             std::to_string(local_finish_frame) + " crc 0x" + to_hex(local_finish_crc) +
                                             " != peer frame " + std::to_string(peer_finish_frame) +
                                             " crc 0x" + to_hex(peer_finish_crc));
                }
            }
        }

        return true;
    }

    void process_pong(uint32_t pong_id) {
        if (pong_id == 0) return;
        size_t idx = pong_id % kPingTableCapacity;
        if (active_pings[idx].id == pong_id) {
            auto now = Clock::now();
            double sample = std::chrono::duration<double, std::milli>(now - active_pings[idx].sent_time).count();
            if (smoothed_rtt_ms <= 0.0) {
                smoothed_rtt_ms = sample;
            } else {
                smoothed_rtt_ms = smoothed_rtt_ms * 0.8 + sample * 0.2;
            }
            active_pings[idx].id = 0; // Mark matched
        }
    }

    void process_peer_ack(uint32_t ack) {
        if (ack == 0xFFFFFFFF) return;
        if (peer_ack_frame == 0xFFFFFFFF || ack > peer_ack_frame) {
            peer_ack_frame = ack;
        }
    }

    void process_peer_checksum_ack(uint32_t ack_cs) {
        if (ack_cs == 0xFFFFFFFF) return;
        if (peer_ack_checksum_frame == 0xFFFFFFFF || ack_cs > peer_ack_checksum_frame) {
            peer_ack_checksum_frame = ack_cs;
            // Advance out_cs_head past acknowledged checksums
            while (out_cs_count > 0 && outgoing_checksums[out_cs_head].frame <= peer_ack_checksum_frame) {
                out_cs_head = (out_cs_head + 1) % kChecksumRingCapacity;
                out_cs_count--;
            }
        }
    }

    static std::string to_hex(uint32_t val) {
        std::ostringstream ss;
        ss << std::hex << val;
        return ss.str();
    }
};

Transport::Transport(const TransportOptions& opts, const Identity& identity)
    : impl(std::make_unique<Impl>(opts, identity)) {}

Transport::~Transport() = default;
Transport::Transport(Transport&&) noexcept = default;
Transport& Transport::operator=(Transport&&) noexcept = default;

void Transport::pump(uint32_t simulated_frame, uint32_t confirmed_frame) {
    if (!impl) return;

    impl->local_simulated_frame = simulated_frame;
    impl->local_confirmed_frame = confirmed_frame;

    auto now = Impl::Clock::now();

    // 1. Drain incoming UDP socket packets (using connected socket)
    uint8_t buf[kMaxPacketSize + 128];
    while (true) {
        ssize_t n = recv(impl->sock_fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            break;
        }

        impl->handle_packet(buf, static_cast<size_t>(n));
    }

    // 2. State-specific background maintenance
    if (impl->state == Impl::State::Connecting) {
        // Upper bound wait: 120s max. But do NOT timeout after 12s if server is actively replying JoinWait!
        auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - impl->start_time).count();
        if (total_elapsed > 120) {
            impl->state = Impl::State::Terminated;
            throw std::runtime_error("netplay room wait timeout: exceeded 120 seconds waiting for opponent");
        }

        auto since_last_reply = std::chrono::duration_cast<std::chrono::seconds>(now - impl->last_server_reply_time).count();
        if (since_last_reply > 10) {
            impl->state = Impl::State::Terminated;
            throw std::runtime_error("netplay handshake timeout: no response from server for 10 seconds");
        }

        auto since_last_send = std::chrono::duration_cast<std::chrono::milliseconds>(now - impl->last_send_time).count();
        if (since_last_send >= 80) {
            impl->send_join_req();
        }
        return;
    }

    if (impl->state == Impl::State::Connected) {
        // Disconnect timeout check (8 seconds)
        auto since_last_recv = std::chrono::duration_cast<std::chrono::milliseconds>(now - impl->last_recv_time).count();
        if (since_last_recv > 8000) {
            impl->state = Impl::State::Terminated;
            throw std::runtime_error("netplay connection timeout: peer unreachable for 8 seconds");
        }

        // Send rate pacing: limit outgoing transmissions to >= 8ms (bounded real-time <= 125 pps)
        auto since_last_send = std::chrono::duration_cast<std::chrono::milliseconds>(now - impl->last_send_time).count();
        if (since_last_send >= kMinSendIntervalMs) {
            bool has_unacked = (impl->has_submitted_inputs &&
                               (impl->peer_ack_frame == 0xFFFFFFFF || impl->peer_ack_frame < impl->latest_submitted_frame));
            bool has_unacked_cs = (impl->out_cs_count > 0);

            if (has_unacked || has_unacked_cs || since_last_send >= 20 || impl->finish_requested) {
                impl->send_game_data();
            }
        }

        // Check if server match completion was received and validates
        if (impl->finish_requested && impl->server_match_completed) {
            if (impl->local_finish_frame != impl->server_final_frame || impl->local_finish_crc != impl->server_final_crc) {
                throw std::runtime_error("netplay finish CRC mismatch: local CRC 0x" + Impl::to_hex(impl->local_finish_crc) +
                                         " != server CRC 0x" + Impl::to_hex(impl->server_final_crc));
            }
            impl->is_finished = true;
        }
    }
}

bool Transport::ready() const {
    return impl && impl->is_ready;
}

unsigned Transport::slot() const {
    if (!impl || !impl->is_ready) {
        throw std::runtime_error("netplay transport not ready");
    }
    return impl->assigned_slot;
}

void Transport::submit(Input input) {
    if (!impl) return;

    if (input.word & ~kInputMask) {
        throw std::runtime_error("netplay submit rejected unknown input bits: 0x" + Impl::to_hex(input.word));
    }

    if (input.frame < impl->opts.delay) {
        throw std::runtime_error("netplay submit rejected pre-delay frame " + std::to_string(input.frame) +
                                 " < delay " + std::to_string(impl->opts.delay));
    }

    // Bounded buffer backpressure check
    uint32_t ack = (impl->peer_ack_frame == 0xFFFFFFFF) ? impl->opts.delay : impl->peer_ack_frame;
    if (input.frame > ack + kMaxUnackedInputs) {
        throw std::runtime_error("netplay input buffer overflow (peer stalled/backpressure): unacked frames > " +
                                 std::to_string(kMaxUnackedInputs));
    }

    size_t idx = input.frame % kInputRingCapacity;
    impl->submitted_inputs[idx] = {input.frame, input.word};

    if (!impl->has_submitted_inputs || input.frame > impl->latest_submitted_frame) {
        impl->latest_submitted_frame = input.frame;
        impl->has_submitted_inputs = true;
    }
}

bool Transport::receive(Input& input) {
    if (!impl) return false;

    size_t idx = impl->next_expected_receive_frame % kInputRingCapacity;
    if (impl->received_inputs[idx].frame == impl->next_expected_receive_frame && !impl->received_inputs[idx].delivered) {
        input.frame = impl->next_expected_receive_frame;
        input.word = impl->received_inputs[idx].word;
        impl->received_inputs[idx].delivered = true; // Mark delivered, keep frame in history to detect mutations
        impl->next_expected_receive_frame++;
        return true;
    }

    return false;
}

void Transport::checksum(Checksum cs) {
    if (!impl) return;

    size_t idx = cs.frame % kChecksumRingCapacity;
    impl->local_checksums[idx] = {cs.frame, cs.crc};

    impl->has_submitted_checksums = true;
    if (cs.frame > impl->latest_submitted_checksum_frame) {
        impl->latest_submitted_checksum_frame = cs.frame;
    }

    // Add to outgoing unacked checksums queue if space
    if (impl->out_cs_count < kChecksumRingCapacity) {
        impl->outgoing_checksums[impl->out_cs_tail] = cs;
        impl->out_cs_tail = (impl->out_cs_tail + 1) % kChecksumRingCapacity;
        impl->out_cs_count++;
    }

    // Check against received peer checksums
    for (size_t i = 0; i < impl->in_cs_count; ++i) {
        size_t in_idx = (impl->in_cs_head + i) % kChecksumRingCapacity;
        if (impl->incoming_checksums[in_idx].frame == cs.frame) {
            if (impl->incoming_checksums[in_idx].crc != cs.crc) {
                throw std::runtime_error("netplay desync detected at frame " + std::to_string(cs.frame) +
                                         ": local CRC 0x" + Impl::to_hex(cs.crc) +
                                         " != remote CRC 0x" + Impl::to_hex(impl->incoming_checksums[in_idx].crc));
            }
            break;
        }
    }
}

bool Transport::receive_checksum(Checksum& cs) {
    if (!impl || impl->in_cs_count == 0) return false;

    cs = impl->incoming_checksums[impl->in_cs_head];
    impl->in_cs_head = (impl->in_cs_head + 1) % kChecksumRingCapacity;
    impl->in_cs_count--;
    return true;
}

void Transport::finish(uint32_t frame, uint32_t crc) {
    if (!impl) return;

    impl->finish_requested = true;
    impl->local_finish_frame = frame;
    impl->local_finish_crc = crc;

    if (impl->server_match_completed) {
        if (impl->server_final_frame != frame || impl->server_final_crc != crc) {
            throw std::runtime_error("netplay finish CRC mismatch: local frame " + std::to_string(frame) +
                                     " crc 0x" + Impl::to_hex(crc) +
                                     " != server frame " + std::to_string(impl->server_final_frame) +
                                     " crc 0x" + Impl::to_hex(impl->server_final_crc));
        }
        impl->is_finished = true;
    }

    // Trigger immediate send
    impl->send_game_data();
}

bool Transport::finished() const {
    return impl && impl->is_finished;
}

double Transport::rtt_ms() const {
    return impl ? impl->smoothed_rtt_ms : 0.0;
}

int Transport::frame_advantage() const {
    if (!impl) return 0;
    return static_cast<int>(impl->local_simulated_frame) - static_cast<int>(impl->peer_simulated_frame);
}

std::string Transport::status() const {
    if (!impl) return "uninitialized";
    std::ostringstream ss;
    if (impl->state == Impl::State::Connecting) {
        ss << "connecting room=" << impl->opts.room;
    } else if (impl->state == Impl::State::Connected) {
        ss << "ready slot=" << impl->assigned_slot
           << " rtt=" << impl->smoothed_rtt_ms << "ms"
           << " adv=" << frame_advantage()
           << " sim=" << impl->local_simulated_frame
           << " ack=" << impl->peer_ack_frame;
        if (impl->is_finished) {
            ss << " [finished]";
        }
    } else {
        ss << "terminated";
    }
    return ss.str();
}

} // namespace f3rt::netplay
