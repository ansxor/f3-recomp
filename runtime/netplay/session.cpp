#include "session.hpp"
#include "recorder.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/game_netplay.hpp"

#include <algorithm>
#include <iostream>

namespace f3rt::netplay {

SessionImpl::SessionImpl(SessionConfig config)
    : config_(std::move(config)), input_delay_(config_.input_delay) {}

SessionImpl::~SessionImpl() {
    close();
}

void SessionImpl::start() {
    if (state_ != SessionState::Idle) return;

    if (!client_.connect_tcp(config_.server_host, config_.server_port)) {
        last_error_ = client_.last_error();
        state_ = SessionState::Ended;
        end_reason_ = EndReason::Disconnected;
        return;
    }

    state_ = SessionState::Connecting;
    hello_sent_ = false;
}

void SessionImpl::send_udp_register() {
    if (!client_.has_session()) return;
    auto pkt = encode_udp_register(session_id_, player_index_, session_key_);
    client_.send_udp_raw(pkt);
    stats_.packets_sent++;
}

void SessionImpl::send_udp_inputs_packet(uint32_t current_frame) {
    if (!client_.has_session()) return;

    uint32_t max_sched = current_frame + input_delay_;
    uint32_t count = std::min(32u, unsigned(input_delay_ + 16));
    uint32_t first_frame = 0;
    if (max_sched + 1 <= count) {
        first_frame = 0;
        count = max_sched + 1;
    } else {
        first_frame = (max_sched + 1) - count;
    }

    UdpInputs inps;
    inps.first_frame = first_frame;
    inps.inputs.reserve(count);
    for (uint32_t k = first_frame; k < first_frame + count; ++k) {
        if (k < input_delay_) {
            inps.inputs.push_back(0);
        } else if (k < local_inputs_.size()) {
            inps.inputs.push_back(local_inputs_[k]);
        } else {
            inps.inputs.push_back(0);
        }
    }

    auto pkt = encode_udp_inputs(session_id_, player_index_, session_key_, inps);
    client_.send_udp_raw(pkt);
    stats_.packets_sent++;
    last_inputs_send_time_ = std::chrono::steady_clock::now();
}

void SessionImpl::record_remote_input(uint32_t frame, uint16_t word) {
    if (remote_inputs_.size() <= frame) {
        remote_inputs_.resize(frame + 1, 0);
        remote_known_.resize(frame + 1, false);
    }
    remote_inputs_[frame] = word;
    remote_known_[frame] = true;
}

bool SessionImpl::handle_running_tcp(const IncomingTcpMsg &msg) {
    if (msg.type == tcp_msg::desync) {
        MsgDesync ds;
        if (decode_desync(msg.payload, ds)) {
            desync_info_ = {true, ds.frame, ds.crc_p1, ds.crc_p2};
        }
        state_ = SessionState::Ended;
        end_reason_ = EndReason::Desync;
        close();
        return true;
    }
    if (msg.type == tcp_msg::peer_left) {
        state_ = SessionState::Ended;
        end_reason_ = EndReason::PeerLeft;
        close();
        return true;
    }
    if (msg.type == tcp_msg::chat_fwd) {
        MsgChat cmsg;
        if (decode_chat_fwd(msg.payload, cmsg)) {
            received_chat_.push_back({cmsg.from_player, std::move(cmsg.text)});
        }
        return false;
    }
    if (msg.type == tcp_msg::error) {
        MsgError err;
        decode_error(msg.payload, err);
        last_error_ = "Server error " + std::to_string(err.code) + ": " + err.message;
        state_ = SessionState::Ended;
        end_reason_ = EndReason::Error;
        close();
        return true;
    }
    return false; // result_ack and unknown types are handled elsewhere / ignored
}

void SessionImpl::poll() {
    client_.poll();

    if (state_ == SessionState::Idle) return;

    if (client_.is_tcp_closed()) {
        if (state_ == SessionState::Running) {
            // The server sends DESYNC / PEER_LEFT / ERROR and may then close the socket; both can arrive in one poll.
            // Classify what was already received before reporting a plain disconnect.
            IncomingTcpMsg msg;
            while (client_.pop_tcp_msg(msg)) {
                if (handle_running_tcp(msg)) return;
            }
        }
        if (state_ != SessionState::Ended) {
            state_ = SessionState::Ended;
            end_reason_ = EndReason::Disconnected;
            if (!client_.last_error().empty()) {
                last_error_ = client_.last_error();
            }
        }
        return;
    }

    // 1. Connecting state
    if (state_ == SessionState::Connecting) {
        if (client_.is_tcp_connected()) {
            if (!hello_sent_) {
                MsgHello hello;
                hello.proto_version = current_proto_version;
                hello.game_id = config_.game_id;
                hello.rom_crcs = config_.rom_crcs;
                hello.build_hash = config_.build_hash;
                hello.player_name = config_.player_name;
                client_.send_tcp_raw(encode_hello(hello));
                hello_sent_ = true;
            }

            IncomingTcpMsg msg;
            while (client_.pop_tcp_msg(msg)) {
                if (msg.type == tcp_msg::welcome) {
                    if (config_.is_host) {
                        MsgCreateRoom cr;
                        cr.input_delay = config_.input_delay;
                        cr.flags = config_.capture_sync ? 1 : config_.flags;
                        client_.send_tcp_raw(encode_create_room(cr));
                        state_ = SessionState::Lobby;
                    } else {
                        MsgJoinRoom jr;
                        jr.room_code = config_.room_code;
                        client_.send_tcp_raw(encode_join_room(jr));
                        state_ = SessionState::Lobby;
                    }
                } else if (msg.type == tcp_msg::error) {
                    MsgError err;
                    decode_error(msg.payload, err);
                    last_error_ = "Server error " + std::to_string(err.code) + ": " + err.message;
                    state_ = SessionState::Ended;
                    end_reason_ = EndReason::Error;
                    close();
                    return;
                }
            }
        }
        return;
    }

    // 2. Lobby state
    if (state_ == SessionState::Lobby) {
        IncomingTcpMsg msg;
        while (client_.pop_tcp_msg(msg)) {
            if (msg.type == tcp_msg::room_created) {
                MsgRoomCreated rc;
                if (decode_room_created(msg.payload, rc)) {
                    room_code_ = rc.room_code;
                }
            } else if (msg.type == tcp_msg::match_start) {
                MsgMatchStart ms;
                if (decode_match_start(msg.payload, ms)) {
                    session_id_ = ms.session_id;
                    session_key_ = ms.session_key;
                    seed_ = ms.seed;
                    player_index_ = ms.player_index;
                    input_delay_ = ms.input_delay;
                    udp_port_ = ms.udp_port;
                    peer_name_ = ms.peer_name;

                    SocketAddress udp_addr;
                    SocketAddress::resolve(config_.server_host, udp_port_, udp_addr);
                    client_.set_session(session_id_, session_key_, udp_addr);
                    if (player_index_ == 0) {
                        if (config_.capture_sync) {
                            auto blob = config_.capture_sync();
                            if (!blob.empty() && blob.size() <= 2048) {
                                client_.send_tcp_raw(encode_sync_blob(blob));
                                sync_blob_sent_ = true;
                            }
                        }
                        state_ = SessionState::Registering;
                        last_register_time_ = std::chrono::steady_clock::now();
                        send_udp_register();
                    } else {
                        state_ = SessionState::Registering;
                        if (config_.apply_sync) {
                            sync_blob_applied_ = false;
                        } else {
                            sync_blob_applied_ = true;
                            last_register_time_ = std::chrono::steady_clock::now();
                            send_udp_register();
                        }
                    }
                    for (const auto &txt : pending_chat_) {
                        client_.send_tcp_raw(encode_chat(txt));
                    }
                    pending_chat_.clear();
                }
            } else if (msg.type == tcp_msg::error) {
                MsgError err;
                decode_error(msg.payload, err);
                last_error_ = "Server error " + std::to_string(err.code) + ": " + err.message;
                state_ = SessionState::Ended;
                end_reason_ = EndReason::Error;
                close();
                return;
            }
        }
        return;
    }

    // 3. Registering state
    if (state_ == SessionState::Registering) {
        IncomingTcpMsg msg;
        while (client_.pop_tcp_msg(msg)) {
            if (msg.type == tcp_msg::sync_blob_fwd) {
                std::vector<uint8_t> blob;
                if (decode_sync_blob(msg.payload, blob)) {
                    if (config_.apply_sync) {
                        config_.apply_sync(blob, seed_);
                    }
                    sync_blob_applied_ = true;
                    last_register_time_ = std::chrono::steady_clock::now();
                    send_udp_register();
                }
            } else if (msg.type == tcp_msg::go) {
                state_ = SessionState::Running;
                break;
            } else if (msg.type == tcp_msg::peer_left) {
                state_ = SessionState::Ended;
                end_reason_ = EndReason::PeerLeft;
                close();
                return;
            } else if (msg.type == tcp_msg::chat_fwd) {
                MsgChat cmsg;
                if (decode_chat_fwd(msg.payload, cmsg)) {
                    received_chat_.push_back({cmsg.from_player, std::move(cmsg.text)});
                }
            } else if (msg.type == tcp_msg::error) {
                MsgError err;
                decode_error(msg.payload, err);
                last_error_ = "Server error " + std::to_string(err.code) + ": " + err.message;
                end_reason_ = EndReason::Error;
                close();
                return;
            }
        }

        IncomingUdpPkt upkt;
        while (client_.pop_udp_pkt(upkt)) {
            if (upkt.kind == udp_kind::reg) {
                auto now = std::chrono::steady_clock::now();
                auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_register_time_).count();
                stats_.ping_ms = uint32_t(rtt);
                stats_.packets_received++;
            }
        }

        if (state_ == SessionState::Registering) {
            bool can_register = (player_index_ == 0) || (!config_.apply_sync || sync_blob_applied_);
            if (can_register) {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_register_time_).count() >= 100) {
                    last_register_time_ = now;
                    send_udp_register();
                }
            }
        }
        return;
    }

    // 4. Running state
    if (state_ == SessionState::Running) {
        IncomingTcpMsg msg;
        while (client_.pop_tcp_msg(msg)) {
            if (handle_running_tcp(msg)) return;
        }

        IncomingUdpPkt upkt;
        while (client_.pop_udp_pkt(upkt)) {
            stats_.packets_received++;
            if (upkt.kind == udp_kind::inputs) {
                UdpInputs inps;
                if (decode_udp_inputs_body(upkt.body, inps)) {
                    for (size_t i = 0; i < inps.inputs.size(); ++i) {
                        uint32_t fr = inps.first_frame + uint32_t(i);
                        record_remote_input(fr, inps.inputs[i]);
                    }
                }
            }
        }

        // Retransmit scheduled inputs periodically if stalled or running
        if (has_sent_inputs_) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_inputs_send_time_).count() >= 8) {
                send_udp_inputs_packet(last_sent_frame_);
            }
        }
    }
}

bool SessionImpl::ready_to_step(uint32_t frame) const {
    if (state_ != SessionState::Running) {
        return false;
    }

    bool local_ok = false;
    if (frame < input_delay_) {
        local_ok = true;
    } else {
        local_ok = (frame < local_submitted_.size() && local_submitted_[frame]);
    }

    bool remote_ok = false;
    if (frame < input_delay_) {
        remote_ok = true;
    } else {
        remote_ok = (frame < remote_known_.size() && remote_known_[frame]);
    }

    if (local_ok && remote_ok) {
        return true;
    }

    stats_.stall_count++;
    return false;
}

std::array<LocalInputWord, 2> SessionImpl::inputs_for(uint32_t frame) const {
    LocalInputWord local_w = 0;
    if (frame >= input_delay_ && frame < local_inputs_.size()) {
        local_w = local_inputs_[frame];
    }
    LocalInputWord remote_w = 0;
    if (frame >= input_delay_ && frame < remote_inputs_.size()) {
        remote_w = remote_inputs_[frame];
    }

    std::array<LocalInputWord, 2> out{};
    if (player_index_ == 0) {
        out[0] = local_w;
        out[1] = remote_w;
    } else {
        out[0] = remote_w;
        out[1] = local_w;
    }
    return out;
}

std::array<LocalInputWord, local_player_count> SessionImpl::all_inputs_for(uint32_t frame) const {
    auto two = inputs_for(frame);
    std::array<LocalInputWord, local_player_count> out{};
    out[0] = two[0];
    out[1] = two[1];
    return out;
}

void SessionImpl::submit_local(uint32_t frame, LocalInputWord word) {
    uint32_t target_frame = frame + input_delay_;
    if (local_inputs_.size() <= target_frame) {
        local_inputs_.resize(target_frame + 1, 0);
        local_submitted_.resize(target_frame + 1, false);
    }
    local_inputs_[target_frame] = word;
    local_submitted_[target_frame] = true;

    current_local_frame_ = std::max(current_local_frame_, frame + 1);
    last_sent_frame_ = frame;
    has_sent_inputs_ = true;

    send_udp_inputs_packet(frame);
}

void SessionImpl::submit_local(LocalInputWord word) {
    submit_local(current_local_frame_, word);
}

void SessionImpl::report_crc(uint32_t frame, uint32_t crc) {
    if (state_ != SessionState::Running && state_ != SessionState::Ended) return;
    MsgCrc msg{frame, crc};
    client_.send_tcp_raw(encode_crc(msg));
}

void SessionImpl::send_result(uint32_t frame, uint8_t winner, uint32_t final_crc) {
    MsgResult msg{frame, winner, final_crc};
    client_.send_tcp_raw(encode_result(msg));
}

void SessionImpl::send_bye() {
    client_.send_tcp_raw(encode_bye());
}

void SessionImpl::close() {
    client_.close();
}

bool SessionImpl::send_chat(std::string_view text) {
    if (!validate_chat_text(text)) return false;
    if (state_ != SessionState::Lobby && state_ != SessionState::Registering && state_ != SessionState::Running) {
        return false;
    }
    if (session_id_ != 0 && client_.is_tcp_connected()) {
        client_.send_tcp_raw(encode_chat(text));
    } else {
        pending_chat_.push_back(std::string(text));
    }
    received_chat_.push_back({player_index_, std::string(text)});
    return true;
}

std::vector<ChatMessage> SessionImpl::drain_chat() {
    std::vector<ChatMessage> res;
    res.swap(received_chat_);
    return res;
}

// Session public wrapper implementation
Session::Session(SessionConfig config)
    : impl_(std::make_unique<SessionImpl>(std::move(config))) {}

Session::~Session() = default;

Session::Session(Session &&) noexcept = default;
Session &Session::operator=(Session &&) noexcept = default;

void Session::start() { impl_->start(); }
void Session::poll() { impl_->poll(); }

SessionState Session::state() const { return impl_->state(); }
EndReason Session::end_reason() const { return impl_->end_reason(); }

const std::string &Session::room_code() const { return impl_->room_code(); }
uint64_t Session::seed() const { return impl_->seed(); }
uint8_t Session::player_index() const { return impl_->player_index(); }
uint8_t Session::input_delay() const { return impl_->input_delay(); }
const std::string &Session::peer_name() const { return impl_->peer_name(); }
const std::string &Session::last_error() const { return impl_->last_error(); }
const DesyncInfo &Session::desync_info() const { return impl_->desync_info(); }
const NetplayStats &Session::stats() const { return impl_->stats(); }
uint16_t Session::local_udp_port() const { return impl_->local_udp_port(); }
uint64_t Session::session_id() const { return impl_->session_id(); }

bool Session::ready_to_step(uint32_t frame) const { return impl_->ready_to_step(frame); }
std::array<LocalInputWord, 2> Session::inputs_for(uint32_t frame) const { return impl_->inputs_for(frame); }
std::array<LocalInputWord, local_player_count> Session::all_inputs_for(uint32_t frame) const { return impl_->all_inputs_for(frame); }

void Session::submit_local(uint32_t frame, LocalInputWord word) { impl_->submit_local(frame, word); }
void Session::submit_local(LocalInputWord word) { impl_->submit_local(word); }

void Session::report_crc(uint32_t frame, uint32_t crc) { impl_->report_crc(frame, crc); }
void Session::send_result(uint32_t frame, uint8_t winner, uint32_t final_crc) { impl_->send_result(frame, winner, final_crc); }
void Session::send_bye() { impl_->send_bye(); }
void Session::close() { impl_->close(); }
bool Session::send_chat(std::string_view text) { return impl_->send_chat(text); }
std::vector<ChatMessage> Session::drain_chat() { return impl_->drain_chat(); }

bool run_lockstep_frame(Machine &m, const std::array<LocalInputWord, local_player_count> &words, uint64_t seed,
                        uint32_t match_frame, const GameNetplay *hook) {
    apply_local_inputs(m, words);
    if (hook && hook->on_match_frame) {
        hook->on_match_frame(m, seed, match_frame);
    }
    if (!m.run_frame(true)) {
        return false;
    }
    if (m.audio) {
        m.audio->finish_frame();
    }
    return true;
}

bool step_lockstep_frame(Machine &m, Session &session, uint32_t match_frame,
                         const GameNetplay *hook, uint32_t crc_interval, MatchRecorder *recorder) {
    const auto words = session.all_inputs_for(match_frame);
    if (recorder) {
        recorder->record_inputs(match_frame, {words[0], words[1]});
    }
    if (!run_lockstep_frame(m, words, session.seed(), match_frame, hook)) {
        return false;
    }
    const uint32_t completed = match_frame + 1;
    if (crc_interval > 0 && completed % crc_interval == 0) {
        uint32_t crc_val;
        if (recorder) {
            // One hashed-region copy + one snapshot into preallocated ring storage; the CRC is computed from that copy.
            crc_val = recorder->on_report(m, completed);
        } else {
            crc_val = (hook && hook->sync_hash) ? hook->sync_hash(m) : m.state_crc();
        }
        session.report_crc(completed, crc_val);
    }
    return true;
}

} // namespace f3rt::netplay
