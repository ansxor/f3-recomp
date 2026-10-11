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
    : config_(std::move(config)),
      adapter_(transport_),
      state_machine_(config_.max_rooms),
      log_writer_(config_.logdir) {
    fill_random_bytes(private_key_.data(), private_key_.size());

}

RelayServer::~RelayServer() {
    stop();
}

bool RelayServer::start() {
    if (running_) {
        return true;
    }

    if (!init_netplay()) {
        return false;
    }

    if (!transport_.open("0.0.0.0", config_.port)) {
        return false;
    }

    std::string pub_addr;
    if (config_.port == 0 || config_.public_address == "127.0.0.1:0" || config_.public_address == "0.0.0.0:0") {
        pub_addr = "127.0.0.1:" + std::to_string(transport_.bound_port());
    } else {
        pub_addr = config_.public_address;
    }

    if (config_.enable_token_server) {
        TokenServer::Config token_cfg;
        token_cfg.port = transport_.bound_port();
        token_cfg.public_address = pub_addr;
        token_server_ = std::make_unique<TokenServer>(token_cfg, private_key_);
        if (!token_server_->start()) {
            transport_.close();
            return false;
        }
    }

    yojimbo::Address server_addr(pub_addr.c_str());
    yojimbo_server_ = std::make_unique<yojimbo::Server>(
        yojimbo::GetDefaultAllocator(),
        private_key_.data(),
        server_addr,
        netplay_config_,
        adapter_,
        get_time_now_seconds()
    );

    if (!yojimbo_server_->Start(MAX_CLIENTS)) {
        if (config_.enable_token_server) {
            token_server_->stop();
        }
        transport_.close();
        yojimbo_server_.reset();
        return false;
    }

    running_ = true;
    return true;
}

void RelayServer::stop() {
    if (!running_) {
        return;
    }

    if (token_server_) {
        token_server_->stop();
    }

    if (yojimbo_server_) {
        yojimbo_server_->Stop();
        yojimbo_server_.reset();
    }

    transport_.close();
    running_ = false;
}

void RelayServer::step(double now, int poll_timeout_ms) {
    if (!running_ || !yojimbo_server_) {
        return;
    }

    // 1. Poll socket
    transport_.poll(poll_timeout_ms);

    // 2. Advance time & receive packets
    yojimbo_server_->AdvanceTime(now);
    yojimbo_server_->ReceivePackets();

    // 3. Process connection / disconnection state changes
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        bool connected = yojimbo_server_->IsClientConnected(static_cast<int>(i));
        if (connected && !client_connected_prev_[i]) {
            client_connected_prev_[i] = true;
            std::vector<Action> actions;
            state_machine_.on_client_connected(static_cast<ClientSlotId>(i), now, actions);
            execute_actions(actions, now);
        } else if (!connected && client_connected_prev_[i]) {
            client_connected_prev_[i] = false;
            std::vector<Action> actions;
            state_machine_.on_client_disconnected(static_cast<ClientSlotId>(i), "yojimbo_disconnect", now, actions);
            execute_actions(actions, now);
        }
    }

    // 4. Process messages
    process_received_messages(now);

    // 5. State machine timer tick
    std::vector<Action> tick_actions;
    state_machine_.tick(now, tick_actions);
    execute_actions(tick_actions, now);

    // 6. Send queued packets
    yojimbo_server_->SendPackets();

    // 7. Flush delayed disconnects after packets are sent
    for (auto it = pending_disconnects_.begin(); it != pending_disconnects_.end(); ) {
        if (now >= it->deadline) {
            if (yojimbo_server_->IsClientConnected(it->slot)) {
                yojimbo_server_->DisconnectClient(it->slot);
            }
            it = pending_disconnects_.erase(it);
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
        if (!yojimbo_server_->IsClientConnected(static_cast<int>(i))) {
            continue;
        }

        for (int ch = 0; ch < NUM_NETPLAY_CHANNELS; ++ch) {
            while (yojimbo::Message *msg = yojimbo_server_->ReceiveMessage(static_cast<int>(i), ch)) {
                std::vector<Action> actions;
                ClientSlotId slot = static_cast<ClientSlotId>(i);

                switch (msg->GetType()) {
                case MSG_HELLO: {
                    auto *h = static_cast<HelloMessage *>(msg);
                    state_machine_.on_hello(slot, h->game_id, h->rom_crcs, h->build_hash, h->player_name, now, actions);
                    break;
                }
                case MSG_CREATE_ROOM: {
                    auto *cr = static_cast<CreateRoomMessage *>(msg);
                    state_machine_.on_create_room(slot, cr->input_delay, cr->flags, now, actions);
                    break;
                }
                case MSG_JOIN_ROOM: {
                    auto *jr = static_cast<JoinRoomMessage *>(msg);
                    state_machine_.on_join_room(slot, jr->room_code, now, actions);
                    break;
                }
                case MSG_SYNC_BLOB: {
                    auto *sb = static_cast<SyncBlobMessage *>(msg);
                    state_machine_.on_sync_blob(slot, std::span<const uint8_t>(sb->GetBlockData(), sb->GetBlockSize()), now, actions);
                    break;
                }
                case MSG_INPUTS: {
                    auto *inp = static_cast<InputsMessage *>(msg);
                    state_machine_.on_inputs(slot, inp->first_frame, inp->count,
                                             std::span<const uint16_t>(inp->inputs, inp->count), now, actions);
                    break;
                }
                case MSG_CRC: {
                    auto *crc = static_cast<CrcMessage *>(msg);
                    state_machine_.on_crc(slot, crc->frame, crc->crc, now, actions);
                    break;
                }
                case MSG_RESULT: {
                    auto *res = static_cast<ResultMessage *>(msg);
                    state_machine_.on_result(slot, res->frame, res->winner, res->final_crc, now, actions);
                    break;
                }
                case MSG_CHAT: {
                    auto *chat = static_cast<ChatTextMessage *>(msg);
                    state_machine_.on_chat(slot, chat->text, now, actions);
                    break;
                }
                case MSG_BYE: {
                    state_machine_.on_bye(slot, now, actions);
                    break;
                }
                default:
                    break;
                }

                yojimbo_server_->ReleaseMessage(static_cast<int>(i), msg);
                execute_actions(actions, now);
            }
        }
    }
}

void RelayServer::execute_actions(const std::vector<Action> &actions, double now) {
    for (const auto &action : actions) {
        std::visit([this, now](const auto &act) {
            using T = std::decay_t<decltype(act)>;
            if constexpr (std::is_same_v<T, ActionSendWelcome>) {
                auto *msg = static_cast<WelcomeMessage *>(yojimbo_server_->CreateMessage(act.slot, MSG_WELCOME));
                if (msg) {
                    yojimbo_server_->SendMessage(act.slot, CHANNEL_CONTROL, msg);
                }
            } else if constexpr (std::is_same_v<T, ActionSendRoomCreated>) {
                auto *msg = static_cast<RoomCreatedMessage *>(yojimbo_server_->CreateMessage(act.slot, MSG_ROOM_CREATED));
                if (msg) {
                    std::strncpy(msg->room_code, act.room_code.c_str(), sizeof(msg->room_code));
                    yojimbo_server_->SendMessage(act.slot, CHANNEL_CONTROL, msg);
                }
            } else if constexpr (std::is_same_v<T, ActionSendMatchStart>) {
                auto *msg = static_cast<MatchStartMessage *>(yojimbo_server_->CreateMessage(act.slot, MSG_MATCH_START));
                if (msg) {
                    msg->session_id = act.session_id;
                    msg->seed = act.seed;
                    msg->player_index = act.player_index;
                    msg->input_delay = act.input_delay;
                    std::strncpy(msg->peer_name, act.peer_name.c_str(), sizeof(msg->peer_name));
                    yojimbo_server_->SendMessage(act.slot, CHANNEL_CONTROL, msg);
                }
            } else if constexpr (std::is_same_v<T, ActionSendGo>) {
                auto *msg = static_cast<GoMessage *>(yojimbo_server_->CreateMessage(act.slot, MSG_GO));
                if (msg) {
                    yojimbo_server_->SendMessage(act.slot, CHANNEL_CONTROL, msg);
                }
            } else if constexpr (std::is_same_v<T, ActionSendSyncBlobFwd>) {
                auto *msg = static_cast<SyncBlobFwdMessage *>(yojimbo_server_->CreateMessage(act.slot, MSG_SYNC_BLOB_FWD));
                if (msg) {
                    uint8_t *block = yojimbo_server_->AllocateBlock(act.slot, static_cast<int>(act.data.size()));
                    if (block) {
                        std::memcpy(block, act.data.data(), act.data.size());
                        yojimbo_server_->AttachBlockToMessage(act.slot, msg, block, static_cast<int>(act.data.size()));
                        yojimbo_server_->SendMessage(act.slot, CHANNEL_CONTROL, msg);
                    } else {
                        yojimbo_server_->ReleaseMessage(act.slot, msg);
                    }
                }
            } else if constexpr (std::is_same_v<T, ActionSendDesync>) {
                auto *msg = static_cast<DesyncMessage *>(yojimbo_server_->CreateMessage(act.slot, MSG_DESYNC));
                if (msg) {
                    msg->frame = act.frame;
                    msg->crc_p1 = act.crc_p1;
                    msg->crc_p2 = act.crc_p2;
                    yojimbo_server_->SendMessage(act.slot, CHANNEL_CONTROL, msg);
                }
            } else if constexpr (std::is_same_v<T, ActionSendResultAck>) {
                auto *msg = static_cast<ResultAckMessage *>(yojimbo_server_->CreateMessage(act.slot, MSG_RESULT_ACK));
                if (msg) {
                    msg->agreed = act.agreed;
                    msg->winner = act.winner;
                    yojimbo_server_->SendMessage(act.slot, CHANNEL_CONTROL, msg);
                }
            } else if constexpr (std::is_same_v<T, ActionSendChatFwd>) {
                auto *msg = static_cast<ChatFwdMessage *>(yojimbo_server_->CreateMessage(act.slot, MSG_CHAT_FWD));
                if (msg) {
                    msg->from_player = act.from_player;
                    std::strncpy(msg->text, act.text.c_str(), sizeof(msg->text));
                    yojimbo_server_->SendMessage(act.slot, CHANNEL_CONTROL, msg);
                }
            } else if constexpr (std::is_same_v<T, ActionSendPeerLeft>) {
                auto *msg = static_cast<PeerLeftMessage *>(yojimbo_server_->CreateMessage(act.slot, MSG_PEER_LEFT));
                if (msg) {
                    yojimbo_server_->SendMessage(act.slot, CHANNEL_CONTROL, msg);
                }
            } else if constexpr (std::is_same_v<T, ActionSendError>) {
                auto *msg = static_cast<ErrorMessage *>(yojimbo_server_->CreateMessage(act.slot, MSG_ERROR));
                if (msg) {
                    msg->code = static_cast<uint16_t>(act.code);
                    std::strncpy(msg->message, act.message.c_str(), sizeof(msg->message));
                    yojimbo_server_->SendMessage(act.slot, CHANNEL_CONTROL, msg);
                }
            } else if constexpr (std::is_same_v<T, ActionSendInputs>) {
                auto *msg = static_cast<InputsMessage *>(yojimbo_server_->CreateMessage(act.slot, MSG_INPUTS));
                if (msg) {
                    msg->first_frame = act.first_frame;
                    msg->count = act.count;
                    for (size_t k = 0; k < act.inputs.size() && k < MAX_INPUTS_COUNT; ++k) {
                        msg->inputs[k] = act.inputs[k];
                    }
                    yojimbo_server_->SendMessage(act.slot, CHANNEL_INPUTS, msg);
                }
            } else if constexpr (std::is_same_v<T, ActionDisconnect>) {
                pending_disconnects_.push_back({act.slot, now + 0.1});
            } else if constexpr (std::is_same_v<T, ActionWriteLog>) {
                log_writer_.write_log(act.snapshot);
            }
        }, action);
    }
}

} // namespace f3rt::netplay
