#include "session.hpp"
#include "recorder.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/game_netplay.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace f3rt::netplay {

SessionImpl::SessionImpl(SessionConfig config)
    : config_(std::move(config)), input_delay_(config_.input_delay) {}

SessionImpl::~SessionImpl() {
    close();
}

void SessionImpl::start() {
    if (state_ != SessionState::Idle) return;

    if (!client_.connect(config_.server_host, config_.server_port)) {
        last_error_ = client_.last_error();
        state_ = SessionState::Ended;
        end_reason_ = EndReason::Disconnected;
        return;
    }

    state_ = SessionState::Connecting;
    hello_sent_ = false;
}

void SessionImpl::close() {
    client_.disconnect();
}

void SessionImpl::send_inputs_packet(uint32_t current_frame) {
    if (!client_.is_connected()) return;

    uint32_t max_sched = current_frame + input_delay_;
    uint32_t count = std::min(32u, unsigned(input_delay_ + 16));
    uint32_t first_frame = 0;
    if (max_sched + 1 <= count) {
        first_frame = 0;
        count = max_sched + 1;
    } else {
        first_frame = (max_sched + 1) - count;
    }

    auto *inps = client_.create_message<InputsMessage>(MSG_INPUTS);
    if (!inps) return;

    inps->first_frame = first_frame;
    inps->count = static_cast<uint8_t>(count);
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t fr = first_frame + k;
        if (fr < input_delay_) {
            inps->inputs[k] = 0;
        } else if (fr < local_inputs_.size()) {
            inps->inputs[k] = local_inputs_[fr];
        } else {
            inps->inputs[k] = 0;
        }
    }

    client_.send_inputs_message(inps);
    client_.send_packets();
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

    send_inputs_packet(frame);
}

void SessionImpl::submit_local(LocalInputWord word) {
    submit_local(current_local_frame_, word);
}

void SessionImpl::report_crc(uint32_t frame, uint32_t crc) {
    if (state_ != SessionState::Running && state_ != SessionState::Ended) return;

    auto *msg = client_.create_message<CrcMessage>(MSG_CRC);
    if (msg) {
        msg->frame = frame;
        msg->crc = crc;
        client_.send_control_message(msg);
        client_.send_packets();
    }
}

void SessionImpl::send_result(uint32_t frame, uint8_t winner, uint32_t final_crc) {
    auto *msg = client_.create_message<ResultMessage>(MSG_RESULT);
    if (msg) {
        msg->frame = frame;
        msg->winner = winner;
        msg->final_crc = final_crc;
        client_.send_control_message(msg);
        client_.send_packets();
    }
}

void SessionImpl::send_bye() {
    auto *msg = client_.create_message<ByeMessage>(MSG_BYE);
    if (msg) {
        client_.send_control_message(msg);
        client_.send_packets();
    }
}

bool SessionImpl::send_chat(std::string_view text) {
    if (!validate_chat_text(text)) return false;
    if (state_ != SessionState::Lobby && state_ != SessionState::Registering && state_ != SessionState::Running) {
        return false;
    }
    if (session_id_ != 0 && client_.is_connected()) {
        auto *msg = client_.create_message<ChatTextMessage>(MSG_CHAT);
        if (msg) {
            std::memcpy(msg->text, text.data(), text.size());
            msg->text[text.size()] = '\0';
            client_.send_control_message(msg);
            client_.send_packets();
        }
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

bool SessionImpl::handle_running_control(yojimbo::Message *msg) {
    if (msg->GetType() == MSG_DESYNC) {
        auto *ds = static_cast<DesyncMessage *>(msg);
        desync_info_ = {true, ds->frame, ds->crc_p1, ds->crc_p2};
        state_ = SessionState::Ended;
        end_reason_ = EndReason::Desync;
        client_.release_message(msg);
        close();
        return true;
    }
    if (msg->GetType() == MSG_PEER_LEFT) {
        state_ = SessionState::Ended;
        end_reason_ = EndReason::PeerLeft;
        client_.release_message(msg);
        close();
        return true;
    }
    if (msg->GetType() == MSG_CHAT_FWD) {
        auto *cmsg = static_cast<ChatFwdMessage *>(msg);
        received_chat_.push_back({cmsg->from_player, std::string(cmsg->text)});
        client_.release_message(msg);
        return false;
    }
    if (msg->GetType() == MSG_ERROR) {
        auto *err = static_cast<ErrorMessage *>(msg);
        last_error_ = "Server error " + std::to_string(err->code) + ": " + err->message;
        state_ = SessionState::Ended;
        end_reason_ = EndReason::Error;
        client_.release_message(msg);
        close();
        return true;
    }
    client_.release_message(msg);
    return false;
}

void SessionImpl::poll() {
    client_.poll();

    if (state_ == SessionState::Idle) return;

    if (client_.connection_failed() || client_.is_disconnected()) {
        if (state_ == SessionState::Running) {
            while (auto *msg = client_.receive_control_message()) {
                if (handle_running_control(msg)) return;
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
        if (client_.is_connected()) {
            if (!hello_sent_) {
                auto *hello = client_.create_message<HelloMessage>(MSG_HELLO);
                if (hello) {
                    std::strncpy(hello->game_id, config_.game_id.c_str(), sizeof(hello->game_id) - 1);
                    for (size_t i = 0; i < ROM_CRCS_COUNT && i < config_.rom_crcs.size(); ++i) {
                        hello->rom_crcs[i] = config_.rom_crcs[i];
                    }
                    std::memcpy(hello->build_hash, config_.build_hash.data(), BUILD_HASH_SIZE);
                    std::strncpy(hello->player_name, config_.player_name.c_str(), sizeof(hello->player_name) - 1);
                    client_.send_control_message(hello);
                    client_.send_packets();
                    hello_sent_ = true;
                }
            }

            while (auto *msg = client_.receive_control_message()) {
                if (msg->GetType() == MSG_WELCOME) {
                    if (config_.is_host) {
                        auto *cr = client_.create_message<CreateRoomMessage>(MSG_CREATE_ROOM);
                        if (cr) {
                            cr->input_delay = config_.input_delay;
                            cr->flags = config_.capture_sync ? 1 : config_.flags;
                            client_.send_control_message(cr);
                            client_.send_packets();
                            state_ = SessionState::Lobby;
                        }
                    } else {
                        auto *jr = client_.create_message<JoinRoomMessage>(MSG_JOIN_ROOM);
                        if (jr) {
                            std::strncpy(jr->room_code, config_.room_code.c_str(), sizeof(jr->room_code) - 1);
                            client_.send_control_message(jr);
                            client_.send_packets();
                            state_ = SessionState::Lobby;
                        }
                    }
                } else if (msg->GetType() == MSG_ERROR) {
                    auto *err = static_cast<ErrorMessage *>(msg);
                    last_error_ = "Server error " + std::to_string(err->code) + ": " + err->message;
                    state_ = SessionState::Ended;
                    end_reason_ = EndReason::Error;
                    close();
                    client_.release_message(msg);
                    return;
                }
                client_.release_message(msg);
            }
        }
        return;
    }

    // 2. Lobby & Registering states
    if (state_ == SessionState::Lobby || state_ == SessionState::Registering) {
        while (auto *msg = client_.receive_control_message()) {
            if (msg->GetType() == MSG_ROOM_CREATED) {
                auto *rc = static_cast<RoomCreatedMessage *>(msg);
                room_code_ = rc->room_code;
            } else if (msg->GetType() == MSG_MATCH_START) {
                auto *ms = static_cast<MatchStartMessage *>(msg);
                session_id_ = ms->session_id;
                seed_ = ms->seed;
                player_index_ = ms->player_index;
                input_delay_ = ms->input_delay;
                peer_name_ = ms->peer_name;

                if (player_index_ == 0) {
                    if (config_.capture_sync) {
                        auto blob = config_.capture_sync();
                        if (!blob.empty() && blob.size() <= MAX_SYNC_BLOB_SIZE) {
                            auto *sb = client_.create_message<SyncBlobMessage>(MSG_SYNC_BLOB);
                            if (sb) {
                                client_.attach_block_to_message(sb, blob.data(), blob.size());
                                client_.send_control_message(sb);
                                client_.send_packets();
                                sync_blob_sent_ = true;
                            }
                        }
                    }
                    state_ = SessionState::Registering;
                } else {
                    state_ = SessionState::Registering;
                    sync_blob_applied_ = !config_.apply_sync;
                }

                for (const auto &txt : pending_chat_) {
                    auto *cmsg = client_.create_message<ChatTextMessage>(MSG_CHAT);
                    if (cmsg) {
                        std::memcpy(cmsg->text, txt.data(), txt.size());
                        cmsg->text[txt.size()] = '\0';
                        client_.send_control_message(cmsg);
                    }
                }
                pending_chat_.clear();
            } else if (msg->GetType() == MSG_SYNC_BLOB_FWD) {
                auto *sb = static_cast<SyncBlobFwdMessage *>(msg);
                if (sb->GetBlockData() && sb->GetBlockSize() > 0) {
                    std::span<const uint8_t> blob(sb->GetBlockData(), sb->GetBlockSize());
                    if (config_.apply_sync) {
                        config_.apply_sync(blob, seed_);
                    }
                    sync_blob_applied_ = true;
                }
            } else if (msg->GetType() == MSG_GO) {
                state_ = SessionState::Running;
                client_.release_message(msg);
                break;
            } else if (msg->GetType() == MSG_PEER_LEFT) {
                state_ = SessionState::Ended;
                end_reason_ = EndReason::PeerLeft;
                client_.release_message(msg);
                close();
                return;
            } else if (msg->GetType() == MSG_CHAT_FWD) {
                auto *cmsg = static_cast<ChatFwdMessage *>(msg);
                received_chat_.push_back({cmsg->from_player, std::string(cmsg->text)});
            } else if (msg->GetType() == MSG_ERROR) {
                auto *err = static_cast<ErrorMessage *>(msg);
                last_error_ = "Server error " + std::to_string(err->code) + ": " + err->message;
                state_ = SessionState::Ended;
                end_reason_ = EndReason::Error;
                client_.release_message(msg);
                close();
                return;
            }
            client_.release_message(msg);
        }
        if (state_ != SessionState::Running) {
            client_.send_packets();
            return;
        }
    }

    // 4. Running state
    if (state_ == SessionState::Running) {
        while (auto *msg = client_.receive_control_message()) {
            if (handle_running_control(msg)) return;
        }

        while (auto *msg = client_.receive_inputs_message()) {
            if (msg->GetType() == MSG_INPUTS) {
                auto *inps = static_cast<InputsMessage *>(msg);
                for (int i = 0; i < inps->count; ++i) {
                    record_remote_input(inps->first_frame + i, inps->inputs[i]);
                }
            }
            client_.release_message(msg);
        }

        // Retransmit scheduled inputs periodically if stalled or running
        if (has_sent_inputs_) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_inputs_send_time_).count() >= 8) {
                send_inputs_packet(last_sent_frame_);
            }
        }
    }

    client_.send_packets();
}

// Session wrapper delegating to SessionImpl
Session::Session(SessionConfig config)
    : impl_(std::make_unique<SessionImpl>(std::move(config))) {}

Session::~Session() = default;

Session::Session(Session &&) noexcept = default;
Session &Session::operator=(Session &&) noexcept = default;

void Session::start() { impl_->start(); }
void Session::poll() { impl_->poll(); }

SessionState Session::state() const { return impl_->state(); }
EndReason Session::end_reason() const { return impl_->end_reason(); }

uint64_t Session::session_id() const { return impl_->session_id(); }
const std::string &Session::room_code() const { return impl_->room_code(); }
uint64_t Session::seed() const { return impl_->seed(); }
uint8_t Session::player_index() const { return impl_->player_index(); }
uint8_t Session::input_delay() const { return impl_->input_delay(); }
const std::string &Session::peer_name() const { return impl_->peer_name(); }
const std::string &Session::last_error() const { return impl_->last_error(); }
const DesyncInfo &Session::desync_info() const { return impl_->desync_info(); }
const NetplayStats &Session::stats() const { return impl_->stats(); }

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
            crc_val = recorder->on_report(m, completed);
        } else {
            crc_val = (hook && hook->sync_hash) ? hook->sync_hash(m) : m.state_crc();
        }
        session.report_crc(completed, crc_val);
    }
    return true;
}

} // namespace f3rt::netplay
