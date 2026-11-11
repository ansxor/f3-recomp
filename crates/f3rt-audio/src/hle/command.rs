//! `audio/hle/command.hpp` + `command.cpp`: the mailbox packet opcodes and their decoded payloads.
//!
//! `parse` panics on a malformed length prefix (the C++ throws); an opcode or payload length the
//! driver rejects becomes `Ignored`.
//!
//! The `read` decoders are `unsafe` because they dereference the payload pointer the C++ passes them;
//! `CommandPacket::parse` is their only caller and keeps the pointer inside the packet.
#![allow(clippy::missing_safety_doc)]

/// The sequence byte of a sequence-addressed command: bits 0..6 select the sequence slot, bit 7 is
/// the driver's selection flag (arrangement dispatch; some routines keep extra state in it).
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct SequenceRef {
    pub id: u8,
    pub selection: bool,
}

impl SequenceRef {
    pub fn read(byte: u8) -> SequenceRef {
        SequenceRef { id: byte & 0x7f, selection: (byte & 0x80) != 0 }
    }
}

macro_rules! sequence_command {
    ($name:ident, $opcode:expr, $length:expr) => {
        /// One struct per mailbox opcode; `length` is the whole packet.
        #[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
        pub struct $name {
            pub sequence: SequenceRef,
        }

        impl $name {
            pub const OPCODE: u8 = $opcode;
            pub const LENGTH: usize = $length;

            pub unsafe fn read(p: *const u8) -> $name {
                $name { sequence: SequenceRef::read(unsafe { *p }) }
            }
        }
    };
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct EffectPreset {
    pub preset: u8,
}

impl EffectPreset {
    pub const OPCODE: u8 = 0x20;
    pub const LENGTH: usize = 3;

    pub unsafe fn read(p: *const u8) -> EffectPreset {
        EffectPreset { preset: unsafe { *p } }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct EffectParameter {
    pub index: u8,
    pub value: u8,
}

impl EffectParameter {
    pub const OPCODE: u8 = 0x21;
    pub const LENGTH: usize = 4;

    pub unsafe fn read(p: *const u8) -> EffectParameter {
        EffectParameter { index: unsafe { *p }, value: unsafe { *p.add(1) } }
    }
}

sequence_command!(SequenceStart, 0x80, 3);
sequence_command!(SequenceStartRepeat, 0x81, 3);
sequence_command!(SequenceStop, 0x82, 3);
sequence_command!(SequencePause, 0x83, 3);
sequence_command!(SequenceResume, 0x84, 3);

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct SequenceSeek {
    pub sequence: SequenceRef,
    pub position: u16,
}

impl SequenceSeek {
    pub const OPCODE: u8 = 0x85;
    pub const LENGTH: usize = 5;

    pub unsafe fn read(p: *const u8) -> SequenceSeek {
        SequenceSeek {
            sequence: SequenceRef::read(unsafe { *p }),
            position: u16::from_be_bytes([unsafe { *p.add(1) }, unsafe { *p.add(2) }]),
        }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct SequenceVolume {
    pub sequence: SequenceRef,
    pub volume: u8,
}

impl SequenceVolume {
    pub const OPCODE: u8 = 0x86;
    pub const LENGTH: usize = 4;

    pub unsafe fn read(p: *const u8) -> SequenceVolume {
        SequenceVolume { sequence: SequenceRef::read(unsafe { *p }), volume: unsafe { *p.add(1) } }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct SequencePan {
    pub sequence: SequenceRef,
    pub value: u8,
}

impl SequencePan {
    pub const OPCODE: u8 = 0x87;
    pub const LENGTH: usize = 4;

    pub unsafe fn read(p: *const u8) -> SequencePan {
        SequencePan { sequence: SequenceRef::read(unsafe { *p }), value: unsafe { *p.add(1) } }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct SequenceRate {
    pub sequence: SequenceRef,
    pub rate: u8,
}

impl SequenceRate {
    pub const OPCODE: u8 = 0x88;
    pub const LENGTH: usize = 4;

    pub unsafe fn read(p: *const u8) -> SequenceRate {
        SequenceRate { sequence: SequenceRef::read(unsafe { *p }), rate: unsafe { *p.add(1) } }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct SequenceFade {
    pub sequence: SequenceRef,
    pub start: u8,
    pub target: u8,
    /// Index into the ROM rate table.
    pub rate_index: u8,
    /// The driver accepts and ignores this byte.
    pub unused: u8,
}

impl SequenceFade {
    pub const OPCODE: u8 = 0x89;
    pub const LENGTH: usize = 7;

    pub unsafe fn read(p: *const u8) -> SequenceFade {
        SequenceFade {
            sequence: SequenceRef::read(unsafe { *p }),
            start: unsafe { *p.add(1) },
            target: unsafe { *p.add(2) },
            rate_index: unsafe { *p.add(3) },
            unused: unsafe { *p.add(4) },
        }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct TrackVolume {
    pub sequence: SequenceRef,
    /// 1-based, as on the wire.
    pub track: u8,
    pub value: u8,
}

impl TrackVolume {
    pub const OPCODE: u8 = 0x8a;
    pub const LENGTH: usize = 5;

    pub unsafe fn read(p: *const u8) -> TrackVolume {
        TrackVolume {
            sequence: SequenceRef::read(unsafe { *p }),
            track: unsafe { *p.add(1) },
            value: unsafe { *p.add(2) },
        }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct TrackPan {
    pub sequence: SequenceRef,
    pub track: u8,
    pub value: u8,
}

impl TrackPan {
    pub const OPCODE: u8 = 0x8b;
    pub const LENGTH: usize = 5;

    pub unsafe fn read(p: *const u8) -> TrackPan {
        TrackPan {
            sequence: SequenceRef::read(unsafe { *p }),
            track: unsafe { *p.add(1) },
            value: unsafe { *p.add(2) },
        }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct TrackController {
    pub sequence: SequenceRef,
    pub track: u8,
    pub index: u8,
    pub value: u8,
}

impl TrackController {
    pub const OPCODE: u8 = 0x8c;
    pub const LENGTH: usize = 6;

    pub unsafe fn read(p: *const u8) -> TrackController {
        TrackController {
            sequence: SequenceRef::read(unsafe { *p }),
            track: unsafe { *p.add(1) },
            index: unsafe { *p.add(2) },
            value: unsafe { *p.add(3) },
        }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct TrackProgram {
    pub sequence: SequenceRef,
    pub track: u8,
    pub program: u16,
}

impl TrackProgram {
    pub const OPCODE: u8 = 0x8d;
    pub const LENGTH: usize = 6;

    pub unsafe fn read(p: *const u8) -> TrackProgram {
        TrackProgram {
            sequence: SequenceRef::read(unsafe { *p }),
            track: unsafe { *p.add(1) },
            program: u16::from_be_bytes([unsafe { *p.add(2) }, unsafe { *p.add(3) }]),
        }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct DirectNoteOn {
    pub sequence: SequenceRef,
    pub track: u8,
    pub key: u8,
    pub velocity: u8,
}

impl DirectNoteOn {
    pub const OPCODE: u8 = 0x8e;
    pub const LENGTH: usize = 6;

    pub unsafe fn read(p: *const u8) -> DirectNoteOn {
        DirectNoteOn {
            sequence: SequenceRef::read(unsafe { *p }),
            track: unsafe { *p.add(1) },
            key: unsafe { *p.add(2) },
            velocity: unsafe { *p.add(3) },
        }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct DirectNoteOff {
    pub sequence: SequenceRef,
    pub track: u8,
    pub key: u8,
}

impl DirectNoteOff {
    pub const OPCODE: u8 = 0x8f;
    pub const LENGTH: usize = 5;

    pub unsafe fn read(p: *const u8) -> DirectNoteOff {
        DirectNoteOff {
            sequence: SequenceRef::read(unsafe { *p }),
            track: unsafe { *p.add(1) },
            key: unsafe { *p.add(2) },
        }
    }
}

/// The length-6 command the driver consumes without changing driver or note state.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Nop {
    pub sequence: SequenceRef,
    pub track: u8,
    pub key: u8,
    pub value: u8,
}

impl Nop {
    pub const OPCODE: u8 = 0x90;
    pub const LENGTH: usize = 6;

    pub unsafe fn read(p: *const u8) -> Nop {
        Nop {
            sequence: SequenceRef::read(unsafe { *p }),
            track: unsafe { *p.add(1) },
            key: unsafe { *p.add(2) },
            value: unsafe { *p.add(3) },
        }
    }
}

/// A packet the driver consumes without acting on: an unknown opcode, or a known opcode whose
/// length does not match.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Ignored {
    pub opcode: u8,
}

/// `std::variant<Ignored, EffectPreset, ...>`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Command {
    Ignored(Ignored),
    EffectPreset(EffectPreset),
    EffectParameter(EffectParameter),
    SequenceStart(SequenceStart),
    SequenceStartRepeat(SequenceStartRepeat),
    SequenceStop(SequenceStop),
    SequencePause(SequencePause),
    SequenceResume(SequenceResume),
    SequenceSeek(SequenceSeek),
    SequenceVolume(SequenceVolume),
    SequencePan(SequencePan),
    SequenceRate(SequenceRate),
    SequenceFade(SequenceFade),
    TrackVolume(TrackVolume),
    TrackPan(TrackPan),
    TrackController(TrackController),
    TrackProgram(TrackProgram),
    DirectNoteOn(DirectNoteOn),
    DirectNoteOff(DirectNoteOff),
    Nop(Nop),
}

impl Default for Command {
    fn default() -> Self {
        Command::Ignored(Ignored::default())
    }
}

/// One validated mailbox packet: `[length, opcode, args...]`.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct CommandPacket {
    opcode: u8,
    command: Command,
}

impl CommandPacket {
    /// Largest packet the mailbox carries (the engine drops longer ones without recording them).
    pub const MAX_BYTES: usize = 8;

    pub fn new(opcode: u8, command: Command) -> CommandPacket {
        CommandPacket { opcode, command }
    }

    /// Panics unless 2 <= bytes.len() <= MAX_BYTES and bytes[0] == bytes.len(). An opcode or payload
    /// length the driver rejects becomes Ignored.
    pub fn parse(bytes: &[u8]) -> CommandPacket {
        if bytes.len() < 2
            || bytes.len() > Self::MAX_BYTES
            || usize::from(bytes[0]) != bytes.len()
        {
            panic!("invalid HLE length-prefixed packet");
        }

        let opcode = bytes[1];
        let payload = unsafe { bytes.as_ptr().add(2) };
        // SAFETY: the guard on every arm keeps the packet at that opcode's LENGTH, so the payload
        // bytes it reads stay inside `bytes` (at most MAX_BYTES - 2 of them).
        unsafe {
            match opcode {
            EffectPreset::OPCODE if bytes.len() == EffectPreset::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::EffectPreset(EffectPreset::read(payload)),
                };
            }
            EffectParameter::OPCODE if bytes.len() == EffectParameter::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::EffectParameter(EffectParameter::read(payload)),
                };
            }
            SequenceStart::OPCODE if bytes.len() == SequenceStart::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::SequenceStart(SequenceStart::read(payload)),
                };
            }
            SequenceStartRepeat::OPCODE if bytes.len() == SequenceStartRepeat::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::SequenceStartRepeat(SequenceStartRepeat::read(payload)),
                };
            }
            SequenceStop::OPCODE if bytes.len() == SequenceStop::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::SequenceStop(SequenceStop::read(payload)),
                };
            }
            SequencePause::OPCODE if bytes.len() == SequencePause::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::SequencePause(SequencePause::read(payload)),
                };
            }
            SequenceResume::OPCODE if bytes.len() == SequenceResume::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::SequenceResume(SequenceResume::read(payload)),
                };
            }
            SequenceSeek::OPCODE if bytes.len() == SequenceSeek::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::SequenceSeek(SequenceSeek::read(payload)),
                };
            }
            SequenceVolume::OPCODE if bytes.len() == SequenceVolume::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::SequenceVolume(SequenceVolume::read(payload)),
                };
            }
            SequencePan::OPCODE if bytes.len() == SequencePan::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::SequencePan(SequencePan::read(payload)),
                };
            }
            SequenceRate::OPCODE if bytes.len() == SequenceRate::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::SequenceRate(SequenceRate::read(payload)),
                };
            }
            SequenceFade::OPCODE if bytes.len() == SequenceFade::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::SequenceFade(SequenceFade::read(payload)),
                };
            }
            TrackVolume::OPCODE if bytes.len() == TrackVolume::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::TrackVolume(TrackVolume::read(payload)),
                };
            }
            TrackPan::OPCODE if bytes.len() == TrackPan::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::TrackPan(TrackPan::read(payload)),
                };
            }
            TrackController::OPCODE if bytes.len() == TrackController::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::TrackController(TrackController::read(payload)),
                };
            }
            TrackProgram::OPCODE if bytes.len() == TrackProgram::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::TrackProgram(TrackProgram::read(payload)),
                };
            }
            DirectNoteOn::OPCODE if bytes.len() == DirectNoteOn::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::DirectNoteOn(DirectNoteOn::read(payload)),
                };
            }
            DirectNoteOff::OPCODE if bytes.len() == DirectNoteOff::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::DirectNoteOff(DirectNoteOff::read(payload)),
                };
            }
            Nop::OPCODE if bytes.len() == Nop::LENGTH => {
                return CommandPacket {
                    opcode,
                    command: Command::Nop(Nop::read(payload)),
                };
            }
                _ => {}
            }
        }
        // The firmware dispatcher consumes an unknown opcode or a wrong length without acting on it.
        CommandPacket { opcode: 0, command: Command::Ignored(Ignored { opcode }) }
    }

    /// The wire opcode byte for a decoded command, 0 for Ignored.
    pub fn opcode(&self) -> u8 {
        self.opcode
    }

    pub fn command(&self) -> &Command {
        &self.command
    }
}
