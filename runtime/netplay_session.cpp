#include "f3rt/netplay_session.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include <stdexcept>
#include <utility>
#include <vector>

namespace f3rt::netplay {
struct Session::Impl {
    Machine &machine;
    std::unique_ptr<Transport> transport;
    std::unique_ptr<Rollback> rollback;
    std::vector<uint8_t> previous_local_state;
    Phase phase = Phase::Lobby;
    Result result = Result::None;
    std::string message;
    unsigned window, assigned_slot = 0, input_delay = 2;
    uint32_t preparation_frames = 0, frame_limit = 0;
    bool host, discontinuity = false;
    double last_rtt = 0;
    int last_advantage = 0;

    Impl(Machine &m, const TransportOptions &options, const Identity &identity, unsigned history)
        : machine(m), window(history), input_delay(options.delay), host(options.host) {
        if (m.allow_main_fallback || m.fallback_instructions || !m.blocks || m.sound_trace)
            throw std::runtime_error("Netplay requires strict-native main execution without sound tracing");
        if (history < 16 || history > Rollback::max_window)
            throw std::runtime_error("Rollback window must be 16..32");
        transport = std::make_unique<Transport>(options, identity);
    }
    void return_local(Result why, const std::string &text) {
        if (phase == Phase::Local) return;
        if (rollback) rollback->restore_confirmed();
        else if (!previous_local_state.empty()) machine.load_state(previous_local_state);
        std::vector<uint8_t>().swap(previous_local_state);
        transport.reset();
        phase = Phase::Local;
        result = why;
        message = text;
        discontinuity = true;
    }
    void flush_checksums() {
        Checksum checksum;
        while (rollback->receive_checksum_to_send(checksum)) transport->checksum(checksum);
    }
    void finish_if_ready() {
        if (!rollback || phase == Phase::Finishing || phase == Phase::Local) return;
        const bool natural = rollback->match_finished();
        if (natural || (frame_limit && rollback->frame() == frame_limit &&
                        rollback->confirmed_frame() == frame_limit)) {
            transport->finish(rollback->confirmed_frame(), machine.sync_state_crc());
            result = natural ? Result::MatchEnded : Result::FrameLimit;
            phase = Phase::Finishing;
            discontinuity = true;
        }
    }
    void pump() {
        if (phase == Phase::Local) return;
        transport->pump(rollback ? rollback->frame() : 0, rollback ? rollback->confirmed_frame() : 0);
        last_rtt = transport->rtt_ms();
        last_advantage = transport->frame_advantage();
        if (phase == Phase::Lobby && transport->paired()) {
            assigned_slot = transport->slot();
            input_delay = transport->delay();
            phase = host ? Phase::Preparing : Phase::WaitingSnapshot;
        }
        if (phase == Phase::Preparing && versus_handoff_ready(machine) && !machine.audio->available_frames()) {
            // This is a between-frame native boundary, after draining solo PCM.
            // Only ordinary game inputs reached it; no game RAM is patched.
            apply_inputs(machine, {});
            std::vector<uint8_t> state(machine.sync_state_size());
            machine.save_sync_state(state);
            transport->offer_snapshot(state);
            phase = Phase::Sending;
        }
        if (phase == Phase::WaitingSnapshot && transport->snapshot_available()) {
            previous_local_state.resize(machine.state_size());
            machine.save_state(previous_local_state);
            machine.load_sync_state(transport->snapshot());
            if (!versus_handoff_ready(machine) || machine.audio->available_frames())
                throw std::runtime_error("Host snapshot is not a drained versus-entry boundary");
            transport->accept_snapshot(machine.sync_state_crc());
            phase = Phase::Accepting;
            discontinuity = true;
        }
        if (!rollback && transport->ready()) {
            if (phase != Phase::Sending && phase != Phase::Accepting)
                throw std::runtime_error("Start barrier arrived before snapshot acceptance");
            rollback = std::make_unique<Rollback>(machine, assigned_slot, input_delay, window);
            std::vector<uint8_t>().swap(previous_local_state);
            phase = Phase::Playing;
            discontinuity = true;
        }
        if (rollback) {
            Input input;
            while (transport->receive(input)) rollback->receive(input);
            Checksum checksum;
            while (transport->receive_checksum(checksum)) rollback->receive_checksum(checksum);
            rollback->synchronize();
            flush_checksums();
            finish_if_ready();
            if (transport->finished()) {
                const auto why = result;
                return_local(why, why == Result::MatchEnded ?
                    "Versus ended; local play resumed. Host/Join again for a fresh match." :
                    "Capture complete; local play resumed");
            }
        }
    }
};

Session::Session(Machine &m, const TransportOptions &options, const Identity &identity, unsigned window)
    : impl_(std::make_unique<Impl>(m, options, identity, window)) {}
Session::~Session() = default;
void Session::pump() {
    try { impl_->pump(); }
    catch (const std::exception &error) { impl_->return_local(Result::Error, error.what()); }
}
bool Session::advance(const std::array<InputWord, 2> &local) {
    auto &p = *impl_;
    if (p.phase == Phase::Local || p.phase == Phase::Sending || p.phase == Phase::Accepting ||
        p.phase == Phase::Finishing) return false;
    if (p.rollback) {
        if (p.frame_limit && p.rollback->frame() >= p.frame_limit) return false;
        try {
            if (p.rollback->needs_local_input()) p.transport->submit(p.rollback->local_input(local[0]));
            const bool advanced = p.rollback->advance();
            p.flush_checksums();
            p.finish_if_ready();
            return advanced;
        } catch (const std::exception &error) {
            p.return_local(Result::Error,error.what());
            return false;
        }
    }
    auto words = local;
    if (p.phase == Phase::Preparing) {
        // Stagger physical coin/start edges. Low active bits include AI/demo,
        // so they cannot gate the human challenger's Start input.
        const unsigned cycle = p.preparation_frames++ % 200;
        words = {};
        for (unsigned player = 0; player < 2; ++player) {
            if (cycle >= player*40 && cycle < player*40+20) words[player] |= 0x100;
            if (cycle >= 80+player*45 && cycle < 85+player*45) words[player] |= 0x80;
            // Finish an interrupted tutorial/selection instead of waiting on
            // an action prompt forever. The next pump snapshots ready entry.
            if (p.preparation_frames % 60 >= 30 && p.preparation_frames % 60 < 33)
                words[player] |= 0x10;
        }
        if (p.preparation_frames > 3600) {
            p.return_local(Result::Error,
                "Could not enter 2P selection through normal coin/start inputs. Exit service or finish the current match, then Host again.");
            return false;
        }
    }
    apply_inputs(p.machine, words);
    if (!p.machine.run_frame(true) || p.machine.fallback_instructions)
        throw std::runtime_error("Strict-native local netplay preparation halted");
    return true;
}
size_t Session::render_audio(int16_t *stereo, size_t max_frames) {
    return impl_->rollback ? impl_->rollback->render_audio(stereo, max_frames) :
        impl_->machine.audio->render(stereo, max_frames);
}
void Session::disconnect(const std::string &reason) { impl_->return_local(Result::Disconnected, reason); }
void Session::set_frame_limit(uint32_t frames) {
    if (frames >= UINT32_MAX - Rollback::history_size)
        throw std::runtime_error("Match frame limit exceeds protocol range");
    impl_->frame_limit = frames;
}
bool Session::connected() const { return impl_->phase != Phase::Local; }
bool Session::synchronized() const { return bool(impl_->rollback); }
Session::Phase Session::phase() const { return impl_->phase; }
Session::Result Session::result() const { return impl_->result; }
const Rollback *Session::rollback() const { return impl_->rollback.get(); }
unsigned Session::slot() const { return impl_->assigned_slot; }
unsigned Session::delay() const { return impl_->input_delay; }
double Session::rtt_ms() const { return impl_->last_rtt; }
int Session::frame_advantage() const { return impl_->last_advantage; }
double Session::transfer_progress() const { return impl_->transport ? impl_->transport->transfer_progress() : 0; }
const std::string &Session::error() const { return impl_->message; }
bool Session::take_discontinuity() { return std::exchange(impl_->discontinuity, false); }
std::string Session::status() const {
    switch (impl_->phase) {
    case Phase::Lobby: return impl_->transport->status();
    case Phase::Preparing: return "Preparing local 2P versus with coin/start inputs";
    case Phase::WaitingSnapshot: return "Playing locally; waiting for host versus entry";
    case Phase::Sending: return "Sending host snapshot";
    case Phase::Accepting: return "Snapshot accepted; waiting for start barrier";
    case Phase::Playing: return "Versus ready, player " + std::to_string(slot() + 1);
    case Phase::Finishing: return "Confirming match end";
    case Phase::Local: return impl_->message;
    }
    throw std::logic_error("Unknown netplay session phase");
}
} // namespace f3rt::netplay
