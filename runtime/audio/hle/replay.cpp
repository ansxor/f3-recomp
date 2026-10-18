#include "audio/hle/replay.hpp"
#include <stdexcept>
#include <string>

namespace f3rt::hle {
namespace {
enum Op : uint8_t { OP_RESET = 1, OP_GAIN = 2, OP_BANK = 3, OP_PACKET = 4 };
constexpr size_t HEADER_BYTES = 8;
constexpr size_t MAX_ITEMS = 0xffff;
[[noreturn]] void malformed(const char *what) {
    throw std::invalid_argument(std::string("malformed F3RP replay script: ") + what);
}
void check_packet(std::span<const uint8_t> packet) {
    if (packet.size() < 2 || packet.size() > REPLAY_MAX_PACKET || packet[0] != packet.size())
        throw std::invalid_argument("replay packet must be 2.." + std::to_string(REPLAY_MAX_PACKET) +
                                    " bytes long and start with its own length");
}
} // namespace

std::vector<uint8_t> encode_replay(const ReplayScript &script) {
    const size_t items = 3 + (script.page ? 1 : 0) + script.packets.size();
    if (items > MAX_ITEMS) throw std::invalid_argument("too many replay items");
    for (const auto &packet : script.packets) check_packet(packet);
    std::vector<uint8_t> out{'F', '3', 'R', 'P', REPLAY_VERSION, 0, uint8_t(items), uint8_t(items >> 8)};
    out.push_back(OP_RESET);
    for (uint8_t channel = 0; channel < 2; ++channel) {
        out.push_back(OP_GAIN);
        out.push_back(channel);
        out.push_back(script.gain_raw[channel]);
    }
    if (script.page) {
        out.push_back(OP_BANK);
        out.push_back(*script.page);
    }
    for (const auto &packet : script.packets) {
        out.push_back(OP_PACKET);
        out.push_back(uint8_t(packet.size()));
        out.insert(out.end(), packet.begin(), packet.end());
    }
    return out;
}

void replay(Core &core, std::span<const uint8_t> script) {
    if (core.tick() != 0 || core.frames() != 0)
        throw std::invalid_argument("replay requires a freshly constructed Core");
    if (script.size() < HEADER_BYTES || script[0] != 'F' || script[1] != '3' || script[2] != 'R' ||
        script[3] != 'P')
        malformed("bad magic");
    if (script[4] != REPLAY_VERSION) malformed("unsupported version");
    if (script[5] != 0) malformed("reserved byte is not zero");
    const size_t count = size_t(script[6]) | size_t(script[7]) << 8;
    // First pass: validate every item so a bad script applies nothing.
    size_t at = HEADER_BYTES;
    for (size_t item = 0; item < count; ++item) {
        if (at >= script.size()) malformed("truncated item list");
        switch (script[at++]) {
        case OP_RESET: break;
        case OP_GAIN:
            if (at + 2 > script.size()) malformed("truncated GAIN");
            if (script[at] > 1) malformed("GAIN channel above 1");
            at += 2;
            break;
        case OP_BANK:
            if (at + 1 > script.size()) malformed("truncated BANK");
            at += 1;
            break;
        case OP_PACKET: {
            if (at >= script.size()) malformed("truncated PACKET");
            const size_t n = script[at++];
            if (at + n > script.size()) malformed("truncated PACKET payload");
            if (n < 2 || n > REPLAY_MAX_PACKET || script[at] != n) malformed("invalid PACKET length");
            at += n;
            break;
        }
        default: malformed("unknown item");
        }
    }
    if (at != script.size()) malformed("trailing bytes");
    // Second pass: apply.
    uint64_t serial = 0;
    at = HEADER_BYTES;
    for (size_t item = 0; item < count; ++item) {
        switch (script[at++]) {
        case OP_RESET: core.reset(0); break;
        case OP_GAIN:
            core.set_gain(script[at], script[at + 1], 0);
            at += 2;
            break;
        case OP_BANK: core.set_bank(script[at++], 0); break;
        default: { // OP_PACKET
            const size_t n = script[at++];
            core.command(script.subspan(at, n), ++serial, 0);
            at += n;
            break;
        }
        }
    }
}

} // namespace f3rt::hle
