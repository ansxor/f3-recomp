#include "audio/hle/command.hpp"
#include <stdexcept>

namespace f3rt::hle {

CommandPacket CommandPacket::parse(std::span<const uint8_t> bytes) {
    if (bytes.size() < 2 || bytes.size() > MAX_BYTES || size_t(bytes[0]) != bytes.size()) {
        throw std::runtime_error("invalid HLE length-prefixed packet");
    }

    const uint8_t opcode = bytes[1];
    const uint8_t *payload = bytes.data() + 2;
    switch (opcode) {
    case EffectPreset::OPCODE:
        if (bytes.size() == EffectPreset::LENGTH) return CommandPacket{opcode, EffectPreset::read(payload)};
        break;
    case EffectParameter::OPCODE:
        if (bytes.size() == EffectParameter::LENGTH)
            return CommandPacket{opcode, EffectParameter::read(payload)};
        break;
    case SequenceStart::OPCODE:
        if (bytes.size() == SequenceStart::LENGTH)
            return CommandPacket{opcode, SequenceStart::read(payload)};
        break;
    case SequenceStartRepeat::OPCODE:
        if (bytes.size() == SequenceStartRepeat::LENGTH)
            return CommandPacket{opcode, SequenceStartRepeat::read(payload)};
        break;
    case SequenceStop::OPCODE:
        if (bytes.size() == SequenceStop::LENGTH)
            return CommandPacket{opcode, SequenceStop::read(payload)};
        break;
    case SequencePause::OPCODE:
        if (bytes.size() == SequencePause::LENGTH)
            return CommandPacket{opcode, SequencePause::read(payload)};
        break;
    case SequenceResume::OPCODE:
        if (bytes.size() == SequenceResume::LENGTH)
            return CommandPacket{opcode, SequenceResume::read(payload)};
        break;
    case SequenceSeek::OPCODE:
        if (bytes.size() == SequenceSeek::LENGTH)
            return CommandPacket{opcode, SequenceSeek::read(payload)};
        break;
    case SequenceVolume::OPCODE:
        if (bytes.size() == SequenceVolume::LENGTH)
            return CommandPacket{opcode, SequenceVolume::read(payload)};
        break;
    case SequencePan::OPCODE:
        if (bytes.size() == SequencePan::LENGTH)
            return CommandPacket{opcode, SequencePan::read(payload)};
        break;
    case SequenceRate::OPCODE:
        if (bytes.size() == SequenceRate::LENGTH)
            return CommandPacket{opcode, SequenceRate::read(payload)};
        break;
    case SequenceFade::OPCODE:
        if (bytes.size() == SequenceFade::LENGTH)
            return CommandPacket{opcode, SequenceFade::read(payload)};
        break;
    case TrackVolume::OPCODE:
        if (bytes.size() == TrackVolume::LENGTH)
            return CommandPacket{opcode, TrackVolume::read(payload)};
        break;
    case TrackPan::OPCODE:
        if (bytes.size() == TrackPan::LENGTH) return CommandPacket{opcode, TrackPan::read(payload)};
        break;
    case TrackController::OPCODE:
        if (bytes.size() == TrackController::LENGTH)
            return CommandPacket{opcode, TrackController::read(payload)};
        break;
    case TrackProgram::OPCODE:
        if (bytes.size() == TrackProgram::LENGTH) return CommandPacket{opcode, TrackProgram::read(payload)};
        break;
    case DirectNoteOn::OPCODE:
        if (bytes.size() == DirectNoteOn::LENGTH) return CommandPacket{opcode, DirectNoteOn::read(payload)};
        break;
    case DirectNoteOff::OPCODE:
        if (bytes.size() == DirectNoteOff::LENGTH) return CommandPacket{opcode, DirectNoteOff::read(payload)};
        break;
    case Nop::OPCODE:
        if (bytes.size() == Nop::LENGTH) return CommandPacket{opcode, Nop::read(payload)};
        break;
    default:
        break;
    }
    // The firmware dispatcher consumes an unknown opcode or a wrong length without acting on it.
    return CommandPacket{0, Ignored{opcode}};
}

} // namespace f3rt::hle
