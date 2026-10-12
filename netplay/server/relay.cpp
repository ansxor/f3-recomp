#include "relay.hpp"
#include <chrono>
#include <cstring>
#include <random>

namespace f3rt::netplay {

namespace {

void fill_random_bytes(uint8_t *data, size_t size) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned int> dist(0, 255);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>(dist(gen));
    }
}

double get_time_now_seconds() {
    using namespace std::chrono;
    auto now = steady_clock::now().time_since_epoch();
    return duration<double>(now).count();
}

} // namespace

RelayServer::RelayServer(RelayConfig config)
    : m_config(std::move(config)), m_adapter(m_transport), m_state_machine(m_config.max_rooms),
      m_log_writer(m_config.logdir) {
    fill_random_bytes(m_private_key.data(), m_private_key.size());
}

RelayServer::~RelayServer() { stop(); }

bool RelayServer::start() {
    if (m_running) {
        return true;
    }

    if (!init_netplay()) {
        return false;
    }

    if (!m_transport.open("0.0.0.0", m_config.port)) {
        return false;
    }

    std::string pub_addr;
    if (m_config.port == 0 || m_config.public_address == "127.0.0.1:0" ||
        m_config.public_address == "0.0.0.0:0") {
        pub_addr = "127.0.0.1:" + std::to_string(m_transport.bound_port());
    } else {
        pub_addr = m_config.public_address;
    }

    if (m_config.enable_token_server) {
        TokenServer::Config token_cfg;
        token_cfg.port = m_transport.bound_port();
        token_cfg.public_address = pub_addr;
        m_token_server = std::make_unique<TokenServer>(token_cfg, m_private_key);
        if (!m_token_server->start()) {
            m_transport.close();
            return false;
        }
    }

    yojimbo::Address server_addr(pub_addr.c_str());
    m_yojimbo_server = std::make_unique<yojimbo::Server>(
        yojimbo::GetDefaultAllocator(), m_private_key.data(), server_addr, m_netplay_config,
        m_adapter, get_time_now_seconds());

    if (!m_yojimbo_server->Start(MAX_CLIENTS)) {
        if (m_config.enable_token_server) {
            m_token_server->stop();
        }
        m_transport.close();
        m_yojimbo_server.reset();
        return false;
    }

    m_running = true;
    return true;
}

void RelayServer::stop() {
    if (!m_running) {
        return;
    }

    if (m_token_server) {
        m_token_server->stop();
    }

    if (m_yojimbo_server) {
        m_yojimbo_server->Stop();
        m_yojimbo_server.reset();
    }

    m_transport.close();
    m_running = false;
}

void RelayServer::step(double now, int poll_timeout_ms) {
    if (!m_running || !m_yojimbo_server) {
        return;
    }

    // 1. Poll socket
    m_transport.poll(poll_timeout_ms);

    // 2. Advance time & receive packets
    m_yojimbo_server->AdvanceTime(now);
    m_yojimbo_server->ReceivePackets();

    // 3. Process connection / disconnection state changes
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        bool connected = m_yojimbo_server->IsClientConnected(static_cast<int>(i));
        if (connected && !m_client_connected_prev[i]) {
            m_client_connected_prev[i] = true;
            std::vector<Action> actions;
            m_state_machine.on_client_connected(static_cast<ClientSlotId>(i), now, actions);
            execute_actions(actions, now);
        } else if (!connected && m_client_connected_prev[i]) {
            m_client_connected_prev[i] = false;
            std::vector<Action> actions;
            m_state_machine.on_client_disconnected(static_cast<ClientSlotId>(i),
                                                   "yojimbo_disconnect", now, actions);
            execute_actions(actions, now);
        }
    }

    // 4. Process messages
    process_received_messages(now);

    // 5. State machine timer tick
    std::vector<Action> tick_actions;
    m_state_machine.tick(now, tick_actions);
    execute_actions(tick_actions, now);

    // 6. Send queued packets
    m_yojimbo_server->SendPackets();

    // 7. Flush delayed disconnects after packets are sent
    for (auto it = m_pending_disconnects.begin(); it != m_pending_disconnects.end();) {
        if (now >= it->deadline) {
            if (m_yojimbo_server->IsClientConnected(it->slot)) {
                m_yojimbo_server->DisconnectClient(it->slot);
            }
            it = m_pending_disconnects.erase(it);
        } else {
            ++it;
        }
    }
}

void RelayServer::run(std::atomic<bool> &stop_requested) {
    while (!stop_requested) {
        step(get_time_now_seconds(), 1);
    }
}

void RelayServer::process_received_messages(double now) {
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (!m_yojimbo_server->IsClientConnected(static_cast<int>(i))) {
            continue;
        }

        for (int ch = 0; ch < NumNetplayChannels; ++ch) {
            while (yojimbo::Message *msg =
                       m_yojimbo_server->ReceiveMessage(static_cast<int>(i), ch)) {
                std::vector<Action> actions;
                ClientSlotId slot = static_cast<ClientSlotId>(i);

                switch (msg->GetType()) {
                case MsgHello: {
                    auto *h = static_cast<HelloMessage *>(msg);
                    m_state_machine.on_hello(slot, h->game_id, h->rom_crcs, h->build_hash,
                                             h->player_name, now, actions);
                    break;
                }
                case MsgCreateRoom: {
                    auto *cr = static_cast<CreateRoomMessage *>(msg);
                    m_state_machine.on_create_room(slot, cr->input_delay, cr->flags, now, actions);
                    break;
                }
                case MsgJoinRoom: {
                    auto *jr = static_cast<JoinRoomMessage *>(msg);
                    m_state_machine.on_join_room(slot, jr->room_code, now, actions);
                    break;
                }
                case MsgSyncBlob: {
                    auto *sb = static_cast<SyncBlobMessage *>(msg);
                    m_state_machine.on_sync_blob(
                        slot, std::span<const uint8_t>(sb->GetBlockData(), sb->GetBlockSize()), now,
                        actions);
                    break;
                }
                case MsgInputs: {
                    auto *inp = static_cast<InputsMessage *>(msg);
                    m_state_machine.on_inputs(slot, inp->first_frame, inp->count,
                                              std::span<const uint16_t>(inp->inputs, inp->count),
                                              now, actions);
                    break;
                }
                case MsgCrc: {
                    auto *crc = static_cast<CrcMessage *>(msg);
                    m_state_machine.on_crc(slot, crc->frame, crc->crc, now, actions);
                    break;
                }
                case MsgResult: {
                    auto *res = static_cast<ResultMessage *>(msg);
                    m_state_machine.on_result(slot, res->frame, res->winner, res->final_crc, now,
                                              actions);
                    break;
                }
                case MsgChat: {
                    auto *chat = static_cast<ChatTextMessage *>(msg);
                    m_state_machine.on_chat(slot, chat->text, now, actions);
                    break;
                }
                case MsgBye: {
                    m_state_machine.on_bye(slot, now, actions);
                    break;
                }
                default:
                    break;
                }

                m_yojimbo_server->ReleaseMessage(static_cast<int>(i), msg);
                execute_actions(actions, now);
            }
        }
    }
}

void RelayServer::execute_actions(const std::vector<Action> &actions, double now) {
    for (const auto &action : actions) {
        std::visit(
            [this, now](const auto &act) {
                using T = std::decay_t<decltype(act)>;
                if constexpr (std::is_same_v<T, ActionSendWelcome>) {
                    auto *msg = static_cast<WelcomeMessage *>(
                        m_yojimbo_server->CreateMessage(act.slot, MsgWelcome));
                    if (msg) {
                        m_yojimbo_server->SendMessage(act.slot, ChannelControl, msg);
                    }
                } else if constexpr (std::is_same_v<T, ActionSendRoomCreated>) {
                    auto *msg = static_cast<RoomCreatedMessage *>(
                        m_yojimbo_server->CreateMessage(act.slot, MsgRoomCreated));
                    if (msg) {
                        std::strncpy(msg->room_code, act.room_code.c_str(), sizeof(msg->room_code));
                        m_yojimbo_server->SendMessage(act.slot, ChannelControl, msg);
                    }
                } else if constexpr (std::is_same_v<T, ActionSendMatchStart>) {
                    auto *msg = static_cast<MatchStartMessage *>(
                        m_yojimbo_server->CreateMessage(act.slot, MsgMatchStart));
                    if (msg) {
                        msg->session_id = act.session_id;
                        msg->seed = act.seed;
                        msg->player_index = act.player_index;
                        msg->input_delay = act.input_delay;
                        std::strncpy(msg->peer_name, act.peer_name.c_str(), sizeof(msg->peer_name));
                        m_yojimbo_server->SendMessage(act.slot, ChannelControl, msg);
                    }
                } else if constexpr (std::is_same_v<T, ActionSendGo>) {
                    auto *msg =
                        static_cast<GoMessage *>(m_yojimbo_server->CreateMessage(act.slot, MsgGo));
                    if (msg) {
                        m_yojimbo_server->SendMessage(act.slot, ChannelControl, msg);
                    }
                } else if constexpr (std::is_same_v<T, ActionSendSyncBlobFwd>) {
                    auto *msg = static_cast<SyncBlobFwdMessage *>(
                        m_yojimbo_server->CreateMessage(act.slot, MsgSyncBlobFwd));
                    if (msg) {
                        uint8_t *block = m_yojimbo_server->AllocateBlock(
                            act.slot, static_cast<int>(act.data.size()));
                        if (block) {
                            std::memcpy(block, act.data.data(), act.data.size());
                            m_yojimbo_server->AttachBlockToMessage(
                                act.slot, msg, block, static_cast<int>(act.data.size()));
                            m_yojimbo_server->SendMessage(act.slot, ChannelControl, msg);
                        } else {
                            m_yojimbo_server->ReleaseMessage(act.slot, msg);
                        }
                    }
                } else if constexpr (std::is_same_v<T, ActionSendDesync>) {
                    auto *msg = static_cast<DesyncMessage *>(
                        m_yojimbo_server->CreateMessage(act.slot, MsgDesync));
                    if (msg) {
                        msg->frame = act.frame;
                        msg->crc_p1 = act.crc_p1;
                        msg->crc_p2 = act.crc_p2;
                        m_yojimbo_server->SendMessage(act.slot, ChannelControl, msg);
                    }
                } else if constexpr (std::is_same_v<T, ActionSendResultAck>) {
                    auto *msg = static_cast<ResultAckMessage *>(
                        m_yojimbo_server->CreateMessage(act.slot, MsgResultAck));
                    if (msg) {
                        msg->agreed = act.agreed;
                        msg->winner = act.winner;
                        m_yojimbo_server->SendMessage(act.slot, ChannelControl, msg);
                    }
                } else if constexpr (std::is_same_v<T, ActionSendChatFwd>) {
                    auto *msg = static_cast<ChatFwdMessage *>(
                        m_yojimbo_server->CreateMessage(act.slot, MsgChatFwd));
                    if (msg) {
                        msg->from_player = act.from_player;
                        std::strncpy(msg->text, act.text.c_str(), sizeof(msg->text));
                        m_yojimbo_server->SendMessage(act.slot, ChannelControl, msg);
                    }
                } else if constexpr (std::is_same_v<T, ActionSendPeerLeft>) {
                    auto *msg = static_cast<PeerLeftMessage *>(
                        m_yojimbo_server->CreateMessage(act.slot, MsgPeerLeft));
                    if (msg) {
                        m_yojimbo_server->SendMessage(act.slot, ChannelControl, msg);
                    }
                } else if constexpr (std::is_same_v<T, ActionSendError>) {
                    auto *msg = static_cast<ErrorMessage *>(
                        m_yojimbo_server->CreateMessage(act.slot, MsgError));
                    if (msg) {
                        msg->code = static_cast<uint16_t>(act.code);
                        std::strncpy(msg->message, act.message.c_str(), sizeof(msg->message));
                        m_yojimbo_server->SendMessage(act.slot, ChannelControl, msg);
                    }
                } else if constexpr (std::is_same_v<T, ActionSendInputs>) {
                    auto *msg = static_cast<InputsMessage *>(
                        m_yojimbo_server->CreateMessage(act.slot, MsgInputs));
                    if (msg) {
                        msg->first_frame = act.first_frame;
                        msg->count = act.count;
                        for (size_t k = 0; k < act.inputs.size() && k < MAX_INPUTS_COUNT; ++k) {
                            msg->inputs[k] = act.inputs[k];
                        }
                        m_yojimbo_server->SendMessage(act.slot, ChannelInputs, msg);
                    }
                } else if constexpr (std::is_same_v<T, ActionDisconnect>) {
                    m_pending_disconnects.push_back({act.slot, now + 0.1});
                } else if constexpr (std::is_same_v<T, ActionWriteLog>) {
                    m_log_writer.write_log(act.snapshot);
                }
            },
            action);
    }
}

} // namespace f3rt::netplay
