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
    : m_config(std::move(config)), m_input_delay(m_config.input_delay) {}

SessionImpl::~SessionImpl() { close(); }

void SessionImpl::start() {
    if (m_state != SessionState::Idle) return;

    if (!m_client.connect(m_config.server_host, m_config.server_port)) {
        m_last_error = m_client.last_error();
        m_state = SessionState::Ended;
        m_end_reason = EndReason::Disconnected;
        return;
    }

    m_state = SessionState::Connecting;
    m_hello_sent = false;
}

void SessionImpl::close() { m_client.disconnect(); }

void SessionImpl::send_inputs_packet(uint32_t current_frame) {
    if (!m_client.is_connected()) return;

    uint32_t max_sched = current_frame + m_input_delay;
    uint32_t count = std::min(32u, unsigned(m_input_delay + 16));
    uint32_t first_frame = 0;
    if (max_sched + 1 <= count) {
        first_frame = 0;
        count = max_sched + 1;
    } else {
        first_frame = (max_sched + 1) - count;
    }

    auto *inps = m_client.create_message<InputsMessage>(MsgInputs);
    if (!inps) return;

    inps->first_frame = first_frame;
    inps->count = static_cast<uint8_t>(count);
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t fr = first_frame + k;
        if (fr >= m_input_delay && fr < m_local_inputs.size()) {
            inps->inputs[k] = m_local_inputs[fr];
        } else {
            inps->inputs[k] = 0;
        }
    }

    m_client.send_inputs_message(inps);
    m_client.send_packets();
    m_last_inputs_send_time = std::chrono::steady_clock::now();
}

void SessionImpl::record_remote_input(uint32_t frame, uint16_t word) {
    if (m_remote_inputs.size() <= frame) {
        m_remote_inputs.resize(frame + 1, 0);
        m_remote_known.resize(frame + 1, false);
    }
    m_remote_inputs[frame] = word;
    m_remote_known[frame] = true;
}

bool SessionImpl::ready_to_step(uint32_t frame) const {
    if (m_state != SessionState::Running) {
        return false;
    }

    bool local_ok = false;
    if (frame < m_input_delay) {
        local_ok = true;
    } else {
        local_ok = (frame < m_local_submitted.size() && m_local_submitted[frame]);
    }

    bool remote_ok = false;
    if (frame < m_input_delay) {
        remote_ok = true;
    } else {
        remote_ok = (frame < m_remote_known.size() && m_remote_known[frame]);
    }

    if (local_ok && remote_ok) {
        return true;
    }

    m_stats.stall_count++;
    return false;
}

std::array<LocalInputWord, 2> SessionImpl::inputs_for(uint32_t frame) const {
    LocalInputWord local_w = 0;
    if (frame >= m_input_delay && frame < m_local_inputs.size()) {
        local_w = m_local_inputs[frame];
    }
    LocalInputWord remote_w = 0;
    if (frame >= m_input_delay && frame < m_remote_inputs.size()) {
        remote_w = m_remote_inputs[frame];
    }

    std::array<LocalInputWord, 2> out{};
    if (m_player_index == 0) {
        out[0] = local_w;
        out[1] = remote_w;
    } else {
        out[0] = remote_w;
        out[1] = local_w;
    }
    return out;
}

std::array<LocalInputWord, LOCAL_PLAYER_COUNT> SessionImpl::all_inputs_for(uint32_t frame) const {
    auto two = inputs_for(frame);
    std::array<LocalInputWord, LOCAL_PLAYER_COUNT> out{};
    out[0] = two[0];
    out[1] = two[1];
    return out;
}

void SessionImpl::submit_local(uint32_t frame, LocalInputWord word) {
    uint32_t target_frame = frame + m_input_delay;
    if (m_local_inputs.size() <= target_frame) {
        m_local_inputs.resize(target_frame + 1, 0);
        m_local_submitted.resize(target_frame + 1, false);
    }
    m_local_inputs[target_frame] = word;
    m_local_submitted[target_frame] = true;

    m_current_local_frame = std::max(m_current_local_frame, frame + 1);
    m_last_sent_frame = frame;
    m_has_sent_inputs = true;

    send_inputs_packet(frame);
}

void SessionImpl::submit_local(LocalInputWord word) { submit_local(m_current_local_frame, word); }

void SessionImpl::report_crc(uint32_t frame, uint32_t crc) {
    if (m_state != SessionState::Running && m_state != SessionState::Ended) return;

    auto *msg = m_client.create_message<CrcMessage>(MsgCrc);
    if (msg) {
        msg->frame = frame;
        msg->crc = crc;
        m_client.send_control_message(msg);
        m_client.send_packets();
    }
}

void SessionImpl::send_result(uint32_t frame, uint8_t winner, uint32_t final_crc) {
    auto *msg = m_client.create_message<ResultMessage>(MsgResult);
    if (msg) {
        msg->frame = frame;
        msg->winner = winner;
        msg->final_crc = final_crc;
        m_client.send_control_message(msg);
        m_client.send_packets();
    }
}

void SessionImpl::send_bye() {
    auto *msg = m_client.create_message<ByeMessage>(MsgBye);
    if (msg) {
        m_client.send_control_message(msg);
        m_client.send_packets();
    }
}

bool SessionImpl::send_chat(std::string_view text) {
    if (!validate_chat_text(text)) return false;
    if (m_state != SessionState::Lobby && m_state != SessionState::Registering &&
        m_state != SessionState::Running) {
        return false;
    }
    if (m_session_id != 0 && m_client.is_connected()) {
        auto *msg = m_client.create_message<ChatTextMessage>(MsgChat);
        if (msg) {
            std::memcpy(msg->text, text.data(), text.size());
            msg->text[text.size()] = '\0';
            m_client.send_control_message(msg);
            m_client.send_packets();
        }
    } else {
        m_pending_chat.push_back(std::string(text));
    }
    m_received_chat.push_back({m_player_index, std::string(text)});
    return true;
}

std::vector<ChatMessage> SessionImpl::drain_chat() {
    std::vector<ChatMessage> res;
    res.swap(m_received_chat);
    return res;
}

bool SessionImpl::handle_running_control(yojimbo::Message *msg) {
    if (msg->GetType() == MsgDesync) {
        auto *ds = static_cast<DesyncMessage *>(msg);
        m_desync_info = {true, ds->frame, ds->crc_p1, ds->crc_p2};
        m_state = SessionState::Ended;
        m_end_reason = EndReason::Desync;
        m_client.release_message(msg);
        close();
        return true;
    }
    if (msg->GetType() == MsgPeerLeft) {
        m_state = SessionState::Ended;
        m_end_reason = EndReason::PeerLeft;
        m_client.release_message(msg);
        close();
        return true;
    }
    if (msg->GetType() == MsgChatFwd) {
        auto *cmsg = static_cast<ChatFwdMessage *>(msg);
        m_received_chat.push_back({cmsg->from_player, std::string(cmsg->text)});
        m_client.release_message(msg);
        return false;
    }
    if (msg->GetType() == MsgError) {
        auto *err = static_cast<ErrorMessage *>(msg);
        m_last_error = "Server error " + std::to_string(err->code) + ": " + err->message;
        m_state = SessionState::Ended;
        m_end_reason = EndReason::Error;
        m_client.release_message(msg);
        close();
        return true;
    }
    m_client.release_message(msg);
    return false;
}

void SessionImpl::poll() {
    m_client.poll();

    if (m_state == SessionState::Idle) return;

    if (m_client.connection_failed() || m_client.is_disconnected()) {
        if (m_state == SessionState::Running) {
            while (auto *msg = m_client.receive_control_message()) {
                if (handle_running_control(msg)) return;
            }
        }
        if (m_state != SessionState::Ended) {
            m_state = SessionState::Ended;
            m_end_reason = EndReason::Disconnected;
            if (!m_client.last_error().empty()) {
                m_last_error = m_client.last_error();
            }
        }
        return;
    }

    // 1. Connecting state
    if (m_state == SessionState::Connecting) {
        if (m_client.is_connected()) {
            if (!m_hello_sent) {
                auto *hello = m_client.create_message<HelloMessage>(MsgHello);
                if (hello) {
                    std::strncpy(hello->game_id, m_config.game_id.c_str(),
                                 sizeof(hello->game_id) - 1);
                    for (size_t i = 0; i < ROM_CRCS_COUNT && i < m_config.rom_crcs.size(); ++i) {
                        hello->rom_crcs[i] = m_config.rom_crcs[i];
                    }
                    std::memcpy(hello->build_hash, m_config.build_hash.data(), BUILD_HASH_SIZE);
                    std::strncpy(hello->player_name, m_config.player_name.c_str(),
                                 sizeof(hello->player_name) - 1);
                    m_client.send_control_message(hello);
                    m_client.send_packets();
                    m_hello_sent = true;
                }
            }

            while (auto *msg = m_client.receive_control_message()) {
                if (msg->GetType() == MsgWelcome) {
                    if (m_config.is_host) {
                        auto *cr = m_client.create_message<CreateRoomMessage>(MsgCreateRoom);
                        if (cr) {
                            cr->input_delay = m_config.input_delay;
                            cr->flags = m_config.capture_sync ? 1 : m_config.flags;
                            m_client.send_control_message(cr);
                            m_client.send_packets();
                            m_state = SessionState::Lobby;
                        }
                    } else {
                        auto *jr = m_client.create_message<JoinRoomMessage>(MsgJoinRoom);
                        if (jr) {
                            std::strncpy(jr->room_code, m_config.room_code.c_str(),
                                         sizeof(jr->room_code) - 1);
                            m_client.send_control_message(jr);
                            m_client.send_packets();
                            m_state = SessionState::Lobby;
                        }
                    }
                } else if (msg->GetType() == MsgError) {
                    auto *err = static_cast<ErrorMessage *>(msg);
                    m_last_error =
                        "Server error " + std::to_string(err->code) + ": " + err->message;
                    m_state = SessionState::Ended;
                    m_end_reason = EndReason::Error;
                    close();
                    m_client.release_message(msg);
                    return;
                }
                m_client.release_message(msg);
            }
        }
        return;
    }

    // 2. Lobby & Registering states
    if (m_state == SessionState::Lobby || m_state == SessionState::Registering) {
        while (auto *msg = m_client.receive_control_message()) {
            if (msg->GetType() == MsgRoomCreated) {
                auto *rc = static_cast<RoomCreatedMessage *>(msg);
                m_room_code = rc->room_code;
            } else if (msg->GetType() == MsgMatchStart) {
                auto *ms = static_cast<MatchStartMessage *>(msg);
                m_session_id = ms->session_id;
                m_seed = ms->seed;
                m_player_index = ms->player_index;
                m_input_delay = ms->input_delay;
                m_peer_name = ms->peer_name;

                if (m_player_index == 0) {
                    if (m_config.capture_sync) {
                        auto blob = m_config.capture_sync();
                        if (!blob.empty() && blob.size() <= MAX_SYNC_BLOB_SIZE) {
                            auto *sb = m_client.create_message<SyncBlobMessage>(MsgSyncBlob);
                            if (sb) {
                                m_client.attach_block_to_message(sb, blob.data(), blob.size());
                                m_client.send_control_message(sb);
                                m_client.send_packets();
                                m_sync_blob_sent = true;
                            }
                        }
                    }
                    m_state = SessionState::Registering;
                } else {
                    m_state = SessionState::Registering;
                    m_sync_blob_applied = !m_config.apply_sync;
                }

                for (const auto &txt : m_pending_chat) {
                    auto *cmsg = m_client.create_message<ChatTextMessage>(MsgChat);
                    if (cmsg) {
                        std::memcpy(cmsg->text, txt.data(), txt.size());
                        cmsg->text[txt.size()] = '\0';
                        m_client.send_control_message(cmsg);
                    }
                }
                m_pending_chat.clear();
            } else if (msg->GetType() == MsgSyncBlobFwd) {
                auto *sb = static_cast<SyncBlobFwdMessage *>(msg);
                if (sb->GetBlockData() && sb->GetBlockSize() > 0) {
                    std::span<const uint8_t> blob(sb->GetBlockData(), sb->GetBlockSize());
                    if (m_config.apply_sync) {
                        m_config.apply_sync(blob, m_seed);
                    }
                    m_sync_blob_applied = true;
                }
            } else if (msg->GetType() == MsgGo) {
                m_state = SessionState::Running;
                m_client.release_message(msg);
                break;
            } else if (msg->GetType() == MsgPeerLeft) {
                m_state = SessionState::Ended;
                m_end_reason = EndReason::PeerLeft;
                m_client.release_message(msg);
                close();
                return;
            } else if (msg->GetType() == MsgChatFwd) {
                auto *cmsg = static_cast<ChatFwdMessage *>(msg);
                m_received_chat.push_back({cmsg->from_player, std::string(cmsg->text)});
            } else if (msg->GetType() == MsgError) {
                auto *err = static_cast<ErrorMessage *>(msg);
                m_last_error = "Server error " + std::to_string(err->code) + ": " + err->message;
                m_state = SessionState::Ended;
                m_end_reason = EndReason::Error;
                m_client.release_message(msg);
                close();
                return;
            }
            m_client.release_message(msg);
        }
        if (m_state != SessionState::Running) {
            m_client.send_packets();
            return;
        }
    }

    // 4. Running state
    if (m_state == SessionState::Running) {
        while (auto *msg = m_client.receive_control_message()) {
            if (handle_running_control(msg)) return;
        }

        while (auto *msg = m_client.receive_inputs_message()) {
            if (msg->GetType() == MsgInputs) {
                auto *inps = static_cast<InputsMessage *>(msg);
                for (int i = 0; i < inps->count; ++i) {
                    record_remote_input(inps->first_frame + i, inps->inputs[i]);
                }
            }
            m_client.release_message(msg);
        }

        // Retransmit scheduled inputs periodically if stalled or running
        if (m_has_sent_inputs) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_inputs_send_time)
                    .count() >= 8) {
                send_inputs_packet(m_last_sent_frame);
            }
        }
    }

    m_client.send_packets();
}

// Session wrapper delegating to SessionImpl
Session::Session(SessionConfig config) : m_impl(std::make_unique<SessionImpl>(std::move(config))) {}

Session::~Session() = default;

Session::Session(Session &&) noexcept = default;
Session &Session::operator=(Session &&) noexcept = default;

void Session::start() { m_impl->start(); }
void Session::poll() { m_impl->poll(); }

SessionState Session::state() const { return m_impl->state(); }
EndReason Session::end_reason() const { return m_impl->end_reason(); }

uint64_t Session::session_id() const { return m_impl->session_id(); }
const std::string &Session::room_code() const { return m_impl->room_code(); }
uint64_t Session::seed() const { return m_impl->seed(); }
uint8_t Session::player_index() const { return m_impl->player_index(); }
uint8_t Session::input_delay() const { return m_impl->input_delay(); }
const std::string &Session::peer_name() const { return m_impl->peer_name(); }
const std::string &Session::last_error() const { return m_impl->last_error(); }
const DesyncInfo &Session::desync_info() const { return m_impl->desync_info(); }
const NetplayStats &Session::stats() const { return m_impl->stats(); }

bool Session::ready_to_step(uint32_t frame) const { return m_impl->ready_to_step(frame); }
std::array<LocalInputWord, 2> Session::inputs_for(uint32_t frame) const {
    return m_impl->inputs_for(frame);
}
std::array<LocalInputWord, LOCAL_PLAYER_COUNT> Session::all_inputs_for(uint32_t frame) const {
    return m_impl->all_inputs_for(frame);
}

void Session::submit_local(uint32_t frame, LocalInputWord word) {
    m_impl->submit_local(frame, word);
}
void Session::submit_local(LocalInputWord word) { m_impl->submit_local(word); }

void Session::report_crc(uint32_t frame, uint32_t crc) { m_impl->report_crc(frame, crc); }
void Session::send_result(uint32_t frame, uint8_t winner, uint32_t final_crc) {
    m_impl->send_result(frame, winner, final_crc);
}
void Session::send_bye() { m_impl->send_bye(); }
void Session::close() { m_impl->close(); }
bool Session::send_chat(std::string_view text) { return m_impl->send_chat(text); }
std::vector<ChatMessage> Session::drain_chat() { return m_impl->drain_chat(); }

bool run_lockstep_frame(Machine &m, const std::array<LocalInputWord, LOCAL_PLAYER_COUNT> &words,
                        uint64_t seed, uint32_t match_frame, const GameNetplay *hook) {
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
