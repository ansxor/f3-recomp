#pragma once
#include "audio/hle/command.hpp"
#include "f3rt/bytes.hpp"
#include "f3rt/memory_map.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace f3rt {

class SharedMailbox {
public:
    static constexpr uint16_t INDEX_MASK = 0x7fe;

    static constexpr bool valid_index(uint16_t index) noexcept {
        return (index & ~unsigned(INDEX_MASK)) == 0;
    }

    explicit SharedMailbox(uint8_t *shared) : m_shared(shared) {
        if (!shared) throw std::invalid_argument("HLE requires shared RAM");
    }

    template <class OnPacket> void drain(OnPacket &&on_packet) {
        const uint16_t producer = uint16_t(read_be16(m_shared + MAILBOX_PRODUCER_H) & INDEX_MASK);
        while (m_consumer != producer) {
            const unsigned index = m_consumer >> 1;
            const uint8_t length = m_shared[index];
            const unsigned available = ((producer - m_consumer) & 0x7ff) >> 1;
            if (length < 2 || length > available) break;
            if (length <= hle::CommandPacket::MAX_BYTES) {
                std::array<uint8_t, hle::CommandPacket::MAX_BYTES> bytes{};
                for (unsigned n = 0; n < length; ++n)
                    bytes[n] = m_shared[(index + n) & MAILBOX_RING_MASK];
                on_packet(hle::CommandPacket::parse(std::span(bytes).first(length)));
            }
            m_consumer = uint16_t((m_consumer + length * 2) & 0x7ff);
        }
        write_be<uint16_t>(m_shared + MAILBOX_CONSUMER_H, m_consumer);
    }

    uint8_t gain(unsigned channel) const noexcept {
        return m_shared[MAILBOX_GAIN_CH0 + (channel & 1)];
    }
    uint16_t consumer() const noexcept { return m_consumer; }
    void reset() noexcept { m_consumer = 0; }
    void restore(uint16_t consumer) noexcept { m_consumer = consumer; }

private:
    uint8_t *m_shared;
    uint16_t m_consumer = 0;
};

}
