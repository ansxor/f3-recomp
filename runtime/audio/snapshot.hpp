#pragma once
#include "audio/shared_mailbox.hpp"
#include "f3rt/bytes.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace f3rt {

struct AudioSnapshot {
    static constexpr size_t BYTES = 32;
    static constexpr size_t CLOCK_AT = 0, COMMANDS_AT = 8, CONSUMER_AT = 16, HELD_AT = 18,
                            RESERVED_AT = 19;

    uint64_t clock = 0, commands = 0;
    uint16_t consumer = 0;
    bool held = false;

    [[nodiscard]] static AudioSnapshot parse(std::span<const uint8_t> src) {
        if (src.size() != BYTES) throw std::invalid_argument("Audio state size mismatch");
        const uint16_t index = read_be16(src, CONSUMER_AT);
        if (!SharedMailbox::valid_index(index) || src[HELD_AT] > 1 ||
            std::any_of(src.begin() + RESERVED_AT, src.end(),
                        [](uint8_t byte) { return byte != 0; }))
            throw std::invalid_argument("Invalid audio mailbox state");
        return {read_be64(src, CLOCK_AT), read_be64(src, COMMANDS_AT), index, src[HELD_AT] != 0};
    }

    void write(std::span<uint8_t> dst) const {
        if (dst.size() != BYTES) throw std::invalid_argument("Audio state size mismatch");
        std::fill(dst.begin(), dst.end(), 0);
        write_be<uint64_t>(dst, CLOCK_AT, clock);
        write_be<uint64_t>(dst, COMMANDS_AT, commands);
        write_be<uint16_t>(dst, CONSUMER_AT, consumer);
        dst[HELD_AT] = held;
    }
};

}
