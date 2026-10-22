#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace f3rt::hle {

// The sequence byte of a sequence-addressed command: bits 0..6 select the sequence slot, bit 7 is the
// driver's selection flag (arrangement dispatch; some routines keep extra state in it).
struct SequenceRef {
    uint8_t id = 0;
    bool selection = false;

    static constexpr SequenceRef read(uint8_t byte) noexcept {
        return {uint8_t(byte & 0x7f), (byte & 0x80) != 0};
    }
};

// One struct per mailbox opcode. `length` is the whole packet, including its length byte and the opcode;
// read() decodes the payload bytes after the opcode. The driver consumes a known opcode with any other
// length without acting on it, so the layout of every accepted packet is fixed here.
struct EffectPreset {
    static constexpr uint8_t OPCODE = 0x20;
    static constexpr size_t LENGTH = 3;
    uint8_t preset = 0;

    static constexpr EffectPreset read(const uint8_t *p) noexcept { return {p[0]}; }
};

struct EffectParameter {
    static constexpr uint8_t OPCODE = 0x21;
    static constexpr size_t LENGTH = 4;
    uint8_t index = 0, value = 0;

    static constexpr EffectParameter read(const uint8_t *p) noexcept { return {p[0], p[1]}; }
};

struct SequenceStart {
    static constexpr uint8_t OPCODE = 0x80;
    static constexpr size_t LENGTH = 3;
    SequenceRef sequence{};

    static constexpr SequenceStart read(const uint8_t *p) noexcept { return {SequenceRef::read(p[0])}; }
};

struct SequenceStartRepeat {
    static constexpr uint8_t OPCODE = 0x81;
    static constexpr size_t LENGTH = 3;
    SequenceRef sequence{};

    static constexpr SequenceStartRepeat read(const uint8_t *p) noexcept {
        return {SequenceRef::read(p[0])};
    }
};

struct SequenceStop {
    static constexpr uint8_t OPCODE = 0x82;
    static constexpr size_t LENGTH = 3;
    SequenceRef sequence{};

    static constexpr SequenceStop read(const uint8_t *p) noexcept { return {SequenceRef::read(p[0])}; }
};

struct SequencePause {
    static constexpr uint8_t OPCODE = 0x83;
    static constexpr size_t LENGTH = 3;
    SequenceRef sequence{};

    static constexpr SequencePause read(const uint8_t *p) noexcept { return {SequenceRef::read(p[0])}; }
};

struct SequenceResume {
    static constexpr uint8_t OPCODE = 0x84;
    static constexpr size_t LENGTH = 3;
    SequenceRef sequence{};

    static constexpr SequenceResume read(const uint8_t *p) noexcept { return {SequenceRef::read(p[0])}; }
};

struct SequenceSeek {
    static constexpr uint8_t OPCODE = 0x85;
    static constexpr size_t LENGTH = 5;
    SequenceRef sequence{};
    uint16_t position = 0;

    static constexpr SequenceSeek read(const uint8_t *p) noexcept {
        return {SequenceRef::read(p[0]), uint16_t(uint16_t(p[1]) << 8 | p[2])};
    }
};

struct SequenceVolume {
    static constexpr uint8_t OPCODE = 0x86;
    static constexpr size_t LENGTH = 4;
    SequenceRef sequence{};
    uint8_t volume = 0;

    static constexpr SequenceVolume read(const uint8_t *p) noexcept {
        return {SequenceRef::read(p[0]), p[1]};
    }
};

struct SequencePan {
    static constexpr uint8_t OPCODE = 0x87;
    static constexpr size_t LENGTH = 4;
    SequenceRef sequence{};
    uint8_t value = 0;

    static constexpr SequencePan read(const uint8_t *p) noexcept {
        return {SequenceRef::read(p[0]), p[1]};
    }
};

struct SequenceRate {
    static constexpr uint8_t OPCODE = 0x88;
    static constexpr size_t LENGTH = 4;
    SequenceRef sequence{};
    uint8_t rate = 0;

    static constexpr SequenceRate read(const uint8_t *p) noexcept {
        return {SequenceRef::read(p[0]), p[1]};
    }
};

struct SequenceFade {
    static constexpr uint8_t OPCODE = 0x89;
    static constexpr size_t LENGTH = 7;
    SequenceRef sequence{};
    uint8_t start = 0, target = 0;
    uint8_t rate_index = 0; // index into the ROM rate table
    uint8_t unused = 0;     // the driver accepts and ignores this byte

    static constexpr SequenceFade read(const uint8_t *p) noexcept {
        return {SequenceRef::read(p[0]), p[1], p[2], p[3], p[4]};
    }
};

struct TrackVolume {
    static constexpr uint8_t OPCODE = 0x8a;
    static constexpr size_t LENGTH = 5;
    SequenceRef sequence{};
    uint8_t track = 0; // 1-based, as on the wire
    uint8_t value = 0;

    static constexpr TrackVolume read(const uint8_t *p) noexcept {
        return {SequenceRef::read(p[0]), p[1], p[2]};
    }
};

struct TrackPan {
    static constexpr uint8_t OPCODE = 0x8b;
    static constexpr size_t LENGTH = 5;
    SequenceRef sequence{};
    uint8_t track = 0;
    uint8_t value = 0;

    static constexpr TrackPan read(const uint8_t *p) noexcept {
        return {SequenceRef::read(p[0]), p[1], p[2]};
    }
};

struct TrackController {
    static constexpr uint8_t OPCODE = 0x8c;
    static constexpr size_t LENGTH = 6;
    SequenceRef sequence{};
    uint8_t track = 0;
    uint8_t index = 0, value = 0;

    static constexpr TrackController read(const uint8_t *p) noexcept {
        return {SequenceRef::read(p[0]), p[1], p[2], p[3]};
    }
};

struct TrackProgram {
    static constexpr uint8_t OPCODE = 0x8d;
    static constexpr size_t LENGTH = 6;
    SequenceRef sequence{};
    uint8_t track = 0;
    uint16_t program = 0;

    static constexpr TrackProgram read(const uint8_t *p) noexcept {
        return {SequenceRef::read(p[0]), p[1], uint16_t(uint16_t(p[2]) << 8 | p[3])};
    }
};

struct DirectNoteOn {
    static constexpr uint8_t OPCODE = 0x8e;
    static constexpr size_t LENGTH = 6;
    SequenceRef sequence{};
    uint8_t track = 0;
    uint8_t key = 0, velocity = 0;

    static constexpr DirectNoteOn read(const uint8_t *p) noexcept {
        return {SequenceRef::read(p[0]), p[1], p[2], p[3]};
    }
};

struct DirectNoteOff {
    static constexpr uint8_t OPCODE = 0x8f;
    static constexpr size_t LENGTH = 5;
    SequenceRef sequence{};
    uint8_t track = 0;
    uint8_t key = 0;

    static constexpr DirectNoteOff read(const uint8_t *p) noexcept {
        return {SequenceRef::read(p[0]), p[1], p[2]};
    }
};

// The length-6 command the driver consumes without changing driver or note state.
struct Nop {
    static constexpr uint8_t OPCODE = 0x90;
    static constexpr size_t LENGTH = 6;
    SequenceRef sequence{};
    uint8_t track = 0;
    uint8_t key = 0, value = 0;

    static constexpr Nop read(const uint8_t *p) noexcept {
        return {SequenceRef::read(p[0]), p[1], p[2], p[3]};
    }
};

// A packet the driver consumes without acting on: an unknown opcode, or a known opcode whose length does
// not match. The default alternative is inert, so a default-constructed packet is a no-op.
struct Ignored {
    uint8_t opcode = 0;
};

using Command = std::variant<Ignored, EffectPreset, EffectParameter, SequenceStart, SequenceStartRepeat,
                             SequenceStop, SequencePause, SequenceResume, SequenceSeek, SequenceVolume,
                             SequencePan, SequenceRate, SequenceFade, TrackVolume, TrackPan,
                             TrackController, TrackProgram, DirectNoteOn, DirectNoteOff, Nop>;

// One validated mailbox packet: [length, opcode, args...]. The variant alternative is the opcode's
// decoded payload, so callers read named fields instead of packet bytes.
class CommandPacket {
public:
    // Largest packet the mailbox carries (the engine drops longer ones without recording them).
    static constexpr size_t MAX_BYTES = 8;

    // An inert packet (Ignored): the default-initialized state of the AudioEngine queue slot.
    CommandPacket() noexcept = default;

    // Throws std::runtime_error unless 2 <= bytes.size() <= MAX_BYTES and bytes[0] == bytes.size().
    // An opcode or payload length the driver rejects becomes Ignored.
    static CommandPacket parse(std::span<const uint8_t> bytes);

    // The wire opcode byte for a decoded command, 0 for Ignored (unknown opcode or rejected length).
    // Dispatch switches on this rather than taking an indirect call through a variant type table.
    uint8_t opcode() const noexcept { return m_opcode; }
    const Command &command() const noexcept { return m_command; }

private:
    explicit CommandPacket(uint8_t opcode, Command command) noexcept
        : m_opcode(opcode), m_command(command) {}

    uint8_t m_opcode = 0;
    Command m_command;
};

} // namespace f3rt::hle
