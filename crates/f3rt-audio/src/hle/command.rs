//! `audio/hle/command.hpp` + `command.cpp`: the mailbox packet opcodes and their decoded payloads.
//!
//! `parse` panics on a malformed length prefix (the C++ throws); an opcode or payload length the
//! driver rejects becomes `Ignored`.

/// A payload field with a fixed wire size. `read` gets exactly `SIZE` bytes.
trait Wire: Sized {
    const SIZE: usize;
    fn read(bytes: &[u8]) -> Self;
}

impl Wire for u8 {
    const SIZE: usize = 1;
    fn read(bytes: &[u8]) -> u8 {
        bytes[0]
    }
}

/// Big-endian on the wire.
impl Wire for u16 {
    const SIZE: usize = 2;
    fn read(bytes: &[u8]) -> u16 {
        u16::from_be_bytes([bytes[0], bytes[1]])
    }
}

fn take<T: Wire>(cursor: &mut &[u8]) -> T {
    let (head, rest) = cursor.split_at(T::SIZE);
    *cursor = rest;
    T::read(head)
}

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

impl Wire for SequenceRef {
    const SIZE: usize = 1;
    fn read(bytes: &[u8]) -> SequenceRef {
        SequenceRef::read(bytes[0])
    }
}

/// A packet the driver consumes without acting on: an unknown opcode, or a known opcode whose
/// length does not match.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Ignored {
    pub opcode: u8,
}

/// Declares every mailbox command once: `Name = opcode { field: WireType, ... }`, fields in wire
/// order. Generates the payload struct, its `OPCODE` and `LENGTH`, the `Command` variant and the
/// decode arm.
macro_rules! commands {
    ($(
        $(#[$meta:meta])*
        $name:ident = $opcode:literal { $( $(#[$fmeta:meta])* $field:ident: $ty:ty ),* $(,)? }
    )*) => {
        $(
            $(#[$meta])*
            #[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
            pub struct $name {
                $( $(#[$fmeta])* pub $field: $ty ),*
            }

            impl $name {
                pub const OPCODE: u8 = $opcode;
                /// The whole packet: length byte, opcode byte, fields.
                pub const LENGTH: usize = 2 $(+ <$ty as Wire>::SIZE)*;

                /// `payload` is exactly `LENGTH - 2` bytes.
                fn read(mut payload: &[u8]) -> $name {
                    $name { $( $field: take(&mut payload) ),* }
                }
            }
        )*

        /// `std::variant<Ignored, EffectPreset, ...>`.
        #[derive(Clone, Copy, PartialEq, Eq, Debug)]
        pub enum Command {
            Ignored(Ignored),
            $( $name($name) ),*
        }

        impl Command {
            /// The firmware dispatcher consumes an unknown opcode or a wrong length without acting
            /// on it.
            fn decode(opcode: u8, payload: &[u8]) -> Command {
                match opcode {
                    $(
                        $name::OPCODE if payload.len() + 2 == $name::LENGTH => {
                            Command::$name($name::read(payload))
                        }
                    )*
                    _ => Command::Ignored(Ignored { opcode }),
                }
            }
        }
    };
}

commands! {
    EffectPreset = 0x20 { preset: u8 }
    EffectParameter = 0x21 { index: u8, value: u8 }

    SequenceStart = 0x80 { sequence: SequenceRef }
    SequenceStartRepeat = 0x81 { sequence: SequenceRef }
    SequenceStop = 0x82 { sequence: SequenceRef }
    SequencePause = 0x83 { sequence: SequenceRef }
    SequenceResume = 0x84 { sequence: SequenceRef }
    SequenceSeek = 0x85 { sequence: SequenceRef, position: u16 }
    SequenceVolume = 0x86 { sequence: SequenceRef, volume: u8 }
    SequencePan = 0x87 { sequence: SequenceRef, value: u8 }
    SequenceRate = 0x88 { sequence: SequenceRef, rate: u8 }
    SequenceFade = 0x89 {
        sequence: SequenceRef,
        start: u8,
        target: u8,
        /// Index into the ROM rate table.
        rate_index: u8,
        /// The driver accepts and ignores this byte.
        unused: u8,
    }

    TrackVolume = 0x8a {
        sequence: SequenceRef,
        /// 1-based, as on the wire.
        track: u8,
        value: u8,
    }
    TrackPan = 0x8b { sequence: SequenceRef, track: u8, value: u8 }
    TrackController = 0x8c { sequence: SequenceRef, track: u8, index: u8, value: u8 }
    TrackProgram = 0x8d { sequence: SequenceRef, track: u8, program: u16 }

    DirectNoteOn = 0x8e { sequence: SequenceRef, track: u8, key: u8, velocity: u8 }
    DirectNoteOff = 0x8f { sequence: SequenceRef, track: u8, key: u8 }

    /// The length-6 command the driver consumes without changing driver or note state.
    Nop = 0x90 { sequence: SequenceRef, track: u8, key: u8, value: u8 }
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

        let command = Command::decode(bytes[1], &bytes[2..]);
        let opcode = if matches!(command, Command::Ignored(_)) { 0 } else { bytes[1] };
        CommandPacket { opcode, command }
    }

    /// The wire opcode byte for a decoded command, 0 for Ignored.
    pub fn opcode(&self) -> u8 {
        self.opcode
    }

    pub fn command(&self) -> &Command {
        &self.command
    }
}
