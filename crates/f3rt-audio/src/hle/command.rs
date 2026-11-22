//! The mailbox packet opcodes and their decoded payloads.
//!
//! `parse` panics on a malformed length prefix; an opcode or payload length the driver rejects
//! becomes `Ignored`.

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

/// The accessors only the sequence-addressed (and, for `track`, track-addressed) variants answer;
/// every other variant yields `None`. Listing the variants here keeps the match arms in one place.
macro_rules! addressed {
    (sequence: [$($seq:ident),* $(,)?], track: [$($trk:ident),* $(,)?]) => {
        impl Command {
            /// The sequence byte the command addresses.
            pub fn sequence_ref(&self) -> Option<SequenceRef> {
                match self {
                    $(Command::$seq(c) => Some(c.sequence),)*
                    _ => None,
                }
            }

            /// The 1-based track the command addresses, as on the wire.
            pub fn track(&self) -> Option<u8> {
                match self {
                    $(Command::$trk(c) => Some(c.track),)*
                    _ => None,
                }
            }

            /// The same command with the driver's selection flag cleared, so it addresses the
            /// sequence directly instead of re-entering arrangement dispatch.
            pub fn without_selection(mut self) -> Command {
                match &mut self {
                    $(Command::$seq(c) => c.sequence.selection = false,)*
                    _ => {}
                }
                self
            }
        }
    };
}

addressed! {
    sequence: [
        SequenceStart, SequenceStartRepeat, SequenceStop, SequencePause, SequenceResume,
        SequenceSeek, SequenceVolume, SequencePan, SequenceRate, SequenceFade, TrackVolume,
        TrackPan, TrackController, TrackProgram, DirectNoteOn, DirectNoteOff, Nop,
    ],
    track: [TrackVolume, TrackPan, TrackController, TrackProgram, DirectNoteOn, DirectNoteOff, Nop]
}

impl Command {
    pub fn is_start(&self) -> bool {
        matches!(self, Command::SequenceStart(_) | Command::SequenceStartRepeat(_))
    }
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

    /// Panics unless 2 <= bytes.len() <= MAX_BYTES and bytes[0] == bytes.len().
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

#[cfg(test)]
mod tests {
    use super::*;

    fn decode(opcode: u8, args: &[u8]) -> Command {
        let mut bytes = vec![(args.len() + 2) as u8, opcode];
        bytes.extend_from_slice(args);
        *CommandPacket::parse(&bytes).command()
    }

    fn seq(id: u8) -> SequenceRef {
        SequenceRef { id, selection: false }
    }

    #[test]
    fn decodes_every_opcode() {
        let s = 0x85;
        let r = SequenceRef { id: 5, selection: true };
        assert_eq!(decode(0x20, &[7]), Command::EffectPreset(EffectPreset { preset: 7 }));
        assert_eq!(
            decode(0x21, &[3, 9]),
            Command::EffectParameter(EffectParameter { index: 3, value: 9 })
        );
        assert_eq!(decode(0x80, &[s]), Command::SequenceStart(SequenceStart { sequence: r }));
        assert_eq!(
            decode(0x81, &[s]),
            Command::SequenceStartRepeat(SequenceStartRepeat { sequence: r })
        );
        assert_eq!(decode(0x82, &[s]), Command::SequenceStop(SequenceStop { sequence: r }));
        assert_eq!(decode(0x83, &[s]), Command::SequencePause(SequencePause { sequence: r }));
        assert_eq!(decode(0x84, &[s]), Command::SequenceResume(SequenceResume { sequence: r }));
        assert_eq!(
            decode(0x85, &[s, 0x12, 0x34]),
            Command::SequenceSeek(SequenceSeek { sequence: r, position: 0x1234 })
        );
        assert_eq!(
            decode(0x86, &[s, 9]),
            Command::SequenceVolume(SequenceVolume { sequence: r, volume: 9 })
        );
        assert_eq!(
            decode(0x87, &[s, 9]),
            Command::SequencePan(SequencePan { sequence: r, value: 9 })
        );
        assert_eq!(
            decode(0x88, &[s, 9]),
            Command::SequenceRate(SequenceRate { sequence: r, rate: 9 })
        );
        assert_eq!(
            decode(0x89, &[s, 1, 2, 3, 4]),
            Command::SequenceFade(SequenceFade {
                sequence: r,
                start: 1,
                target: 2,
                rate_index: 3,
                unused: 4
            })
        );
        assert_eq!(
            decode(0x8a, &[s, 2, 9]),
            Command::TrackVolume(TrackVolume { sequence: r, track: 2, value: 9 })
        );
        assert_eq!(
            decode(0x8b, &[s, 2, 9]),
            Command::TrackPan(TrackPan { sequence: r, track: 2, value: 9 })
        );
        assert_eq!(
            decode(0x8c, &[s, 2, 3, 9]),
            Command::TrackController(TrackController { sequence: r, track: 2, index: 3, value: 9 })
        );
        assert_eq!(
            decode(0x8d, &[s, 2, 0xab, 0xcd]),
            Command::TrackProgram(TrackProgram { sequence: r, track: 2, program: 0xabcd })
        );
        assert_eq!(
            decode(0x8e, &[s, 2, 60, 100]),
            Command::DirectNoteOn(DirectNoteOn { sequence: r, track: 2, key: 60, velocity: 100 })
        );
        assert_eq!(
            decode(0x8f, &[s, 2, 60]),
            Command::DirectNoteOff(DirectNoteOff { sequence: r, track: 2, key: 60 })
        );
        assert_eq!(
            decode(0x90, &[s, 2, 60, 1]),
            Command::Nop(Nop { sequence: r, track: 2, key: 60, value: 1 })
        );
    }

    #[test]
    fn wrong_length_or_unknown_opcode_is_ignored() {
        assert_eq!(decode(0x80, &[]), Command::Ignored(Ignored { opcode: 0x80 }));
        assert_eq!(decode(0x80, &[1, 2]), Command::Ignored(Ignored { opcode: 0x80 }));
        assert_eq!(decode(0x55, &[1]), Command::Ignored(Ignored { opcode: 0x55 }));
    }

    #[test]
    fn packet_records_opcode_only_for_accepted_commands() {
        assert_eq!(CommandPacket::parse(&[3, 0x20, 1]).opcode(), 0x20);
        assert_eq!(CommandPacket::parse(&[3, 0x55, 1]).opcode(), 0);
    }

    #[test]
    fn sequence_byte_splits_id_and_selection_flag() {
        assert_eq!(SequenceRef::read(0x05), seq(5));
        assert_eq!(SequenceRef::read(0xff), SequenceRef { id: 0x7f, selection: true });
    }

    #[test]
    fn without_selection_clears_only_the_flag() {
        let command = decode(0x8e, &[0x85, 2, 60, 100]);
        let cleared = command.without_selection();
        assert_eq!(
            cleared,
            Command::DirectNoteOn(DirectNoteOn { sequence: seq(5), track: 2, key: 60, velocity: 100 })
        );
        let effect = decode(0x20, &[7]);
        assert_eq!(effect.without_selection(), effect);
    }

    #[test]
    fn sequence_ref_and_track_exist_only_where_addressed() {
        let note = decode(0x8e, &[0x85, 2, 60, 100]);
        assert_eq!(note.sequence_ref(), Some(SequenceRef { id: 5, selection: true }));
        assert_eq!(note.track(), Some(2));

        let stop = decode(0x82, &[0x05]);
        assert_eq!(stop.sequence_ref(), Some(seq(5)));
        assert_eq!(stop.track(), None);

        let effect = decode(0x20, &[7]);
        assert_eq!(effect.sequence_ref(), None);
        assert_eq!(effect.track(), None);
        assert_eq!(Command::default().sequence_ref(), None);
    }

    #[test]
    fn only_the_two_start_opcodes_start() {
        assert!(decode(0x80, &[1]).is_start());
        assert!(decode(0x81, &[1]).is_start());
        assert!(!decode(0x82, &[1]).is_start());
    }
}
