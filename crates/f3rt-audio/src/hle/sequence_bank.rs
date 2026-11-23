use crate::hle::driver_layout::DriverLayout;
use crate::hle::events::{NO_PROGRAM, SONG_SLOTS, TRACK_COUNT};
use crate::hle::rom_view::{RomView, SOUND_ROM_IMAGE_SIZE};
use crate::hle::sound_rom;
use crate::hle::track_parameters::at;
use crate::hle::track_service::TrackConfig;
use crate::memory_map::SOUND_ROM_BASE;

const STREAM_BASE: u32 = sound_rom::SEQ_STREAM_BASE;

pub const CONDUCTOR_STREAM_INDEX: usize = 0;
pub const FIRST_MUSIC_STREAM_INDEX: usize = 1;
pub const ARRANGEMENT_STREAM_INDEX: usize = 10;
pub const TOTAL_SONG_STREAMS: usize = 11;
pub const MAX_ARRANGEMENT_PHASES: usize = (u8::MAX - 1) as usize;

pub const EV_NOTE_SHORT_MAX: u8 = 0x58;
pub const EV_NOTE_LONG_MAX: u8 = 0xb0;
const EV_NOTE_KEY_OFFSET: u8 = 0x58;
pub const EV_CONTROLLER_MIN: u8 = 0xb0;
pub const EV_CONTROLLER_MAX: u8 = 0xd9;
pub const EV_PROGRAM_SELECTOR: u8 = 0xd9;
pub const EV_SPECIAL_TRACK_DA: u8 = 0xda;
pub const EV_SPECIAL_TRACK_DB: u8 = 0xdb;
const EV_RELATIVE_NOTE: u8 = 0xdc;
pub const EV_DELAY: u8 = 0xe6;
pub const EV_ARRANGEMENT: u8 = 0xe7;
pub const EV_NOTIFICATION: u8 = 0xe8;
pub const EV_END: u8 = 0xe9;

/// An arrangement entry whose last word is this value marks where the playlist loops back to.
pub const ARRANGEMENT_LOOP_MARKER: u16 = 100;

/// Header flags (+1) bit 1: a standalone song restarts at its last bar.
pub const SEQ_FLAG_LOOP: u8 = 0x02;

const fn decode_short_note_velocity(operand: u16) -> u8 {
    let velocity = ((operand >> 8) & 0x7c) as u8;
    if velocity != 0 { velocity | (velocity >> 5) } else { 1 }
}

const fn decode_long_note_velocity(operand: u16) -> u8 {
    let velocity = (operand & 0x7f) as u8;
    if velocity != 0 { velocity } else { 1 }
}

const fn decode_arrangement_transpose(word3: u16) -> i16 {
    (word3.wrapping_mul(2) as i16) >> 11
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct StreamEvent {
    pub delay: u32,
    pub operand: u16,
    pub duration: u16,
    pub opcode: u8,
    pub key: u8,
    pub velocity: u8,
    pub arrangement: [u16; 4],
}

struct StreamReader<'a> {
    rom: &'a RomView,
    position: u32,
    end: u32,
}

impl StreamReader<'_> {
    fn token(&mut self) -> Result<Option<u16>, &'static str> {
        if self.position.wrapping_add(2) > self.end {
            return Ok(None);
        }
        let word = self.rom.try_read_u16(self.position)?;
        self.position += 2;
        Ok(Some(word))
    }

    fn operand(&mut self) -> Result<u16, &'static str> {
        self.token()?.ok_or("truncated HLE event operand")
    }
}

impl StreamEvent {
    /// `token` has bit 15 set; its low byte is the opcode and bits 8..14 the delay.
    fn decode(token: u16, stream: &mut StreamReader) -> Result<StreamEvent, &'static str> {
        let mut e = StreamEvent {
            opcode: token as u8,
            delay: u32::from((token >> 8) & 0x7f),
            ..StreamEvent::default()
        };

        if e.opcode < EV_NOTE_SHORT_MAX {
            e.key = e.opcode;
            e.operand = stream.operand()?;
            e.duration = e.operand & 0x3ff;
            if e.duration == 0 {
                e.duration = stream.operand()?;
            }
            e.velocity = decode_short_note_velocity(e.operand);
        } else if e.opcode < EV_NOTE_LONG_MAX {
            e.key = e.opcode - EV_NOTE_KEY_OFFSET;
            e.operand = stream.operand()?;
            e.duration = (e.operand & 0x3ff).max(1);
            e.velocity = decode_long_note_velocity(e.operand);
        } else if e.opcode < EV_RELATIVE_NOTE {
            e.operand = stream.operand()?;
        } else if e.opcode == EV_RELATIVE_NOTE {
            return Err("relative-note references are absent from supported ROM bank");
        } else if e.opcode == EV_DELAY {
            e.operand = stream.operand()?;
            e.delay = u32::from(e.operand | ((u32::from(token) << 1) as u16 & 0x8000));
        } else if e.opcode == EV_ARRANGEMENT {
            for word in e.arrangement.iter_mut() {
                *word = stream.operand()?;
            }
            e.delay = 0;
        } else if e.opcode == EV_NOTIFICATION || e.opcode == EV_END {
            e.delay = 0;
        } else {
            return Err("unsupported HLE sequence opcode");
        }
        Ok(e)
    }

    pub fn is_arrangement(&self) -> bool {
        self.opcode == EV_ARRANGEMENT
    }

    pub fn is_loop_marker(&self) -> bool {
        self.arrangement[3] == ARRANGEMENT_LOOP_MARKER
    }

    /// The semitone shift an arrangement entry applies to the tracks its mask selects.
    pub fn arrangement_transpose(&self) -> i16 {
        decode_arrangement_transpose(self.arrangement[3])
    }
}

#[derive(Default)]
pub struct TrackData {
    pub events: Vec<StreamEvent>,
}

impl TrackData {
    fn parse(rom: &RomView, start: u32, end: u32) -> Result<TrackData, &'static str> {
        let mut stream = StreamReader { rom, position: start, end };
        let mut events = Vec::new();

        while let Some(token) = stream.token()? {
            // Words without bit 15 are padding between events.
            if token & 0x8000 == 0 {
                continue;
            }
            let event = StreamEvent::decode(token, &mut stream)?;
            events.push(event);
            if event.opcode == EV_END {
                return Ok(TrackData { events });
            }
        }
        Err("HLE track missing end event")
    }
}

pub struct SongData {
    pub present: bool,
    pub flags: u8,
    pub rate: u8,
    pub numerator: u8,
    pub exponent: u8,
    pub bars: u16,
    pub ticks_per_bar: u16,
    pub loop_start: u32,
    pub loop_end: u32,
    pub initial: [TrackConfig; TRACK_COUNT],
    pub tracks: [TrackData; TOTAL_SONG_STREAMS],
    /// The sequence id this one aliases: commands to it also reach the linked sequence.
    pub linked: u8,
}

impl Default for SongData {
    fn default() -> SongData {
        SongData {
            present: false,
            flags: 0,
            rate: 0,
            numerator: 0,
            exponent: 0,
            bars: 0,
            ticks_per_bar: 0,
            loop_start: 0,
            loop_end: 0,
            initial: [TrackConfig::default(); TRACK_COUNT],
            tracks: core::array::from_fn(|_| TrackData::default()),
            linked: 0,
        }
    }
}

impl SongData {
    pub fn conductor(&self) -> &[StreamEvent] {
        &self.tracks[CONDUCTOR_STREAM_INDEX].events
    }

    pub fn music(&self, track: usize) -> &[StreamEvent] {
        &self.tracks[FIRST_MUSIC_STREAM_INDEX + track].events
    }

    pub fn arrangement(&self) -> &[StreamEvent] {
        &self.tracks[ARRANGEMENT_STREAM_INDEX].events
    }

    /// A song whose arrangement stream opens with a playlist entry selects child sequences.
    pub fn is_arranged(&self) -> bool {
        self.arrangement().first().is_some_and(StreamEvent::is_arrangement)
    }

    /// The child sequence playing at the 1-based playlist `phase`; 0 when none.
    pub fn selected_child(&self, phase: u8) -> u8 {
        let entry = usize::from(phase).checked_sub(1).and_then(|i| self.arrangement().get(i));
        match entry {
            Some(e) if e.is_arrangement() && !e.is_loop_marker() => e.arrangement[0] as u8,
            _ => 0,
        }
    }

    fn parse(
        rom: &RomView,
        layout: &DriverLayout,
        h: u32,
        start: u32,
        end: u32,
    ) -> Result<SongData, &'static str> {
        let mut song = SongData {
            present: true,
            flags: rom.try_read_byte(h.wrapping_add(1))?,
            rate: rom.try_read_byte(h.wrapping_add(sound_rom::SEQ_HEADER_RATE_OFFSET))?,
            bars: rom.try_read_u16(h.wrapping_add(sound_rom::SEQ_HEADER_BARS_OFFSET))?,
            ..SongData::default()
        };

        let exponent = rom.try_read_byte(h.wrapping_add(sound_rom::SEQ_HEADER_TIME_SIG_EXP_OFFSET))?;
        if exponent > 8 {
            return Err("unsupported HLE time signature");
        }
        song.exponent = exponent;
        song.numerator =
            rom.try_read_byte(h.wrapping_add(sound_rom::SEQ_HEADER_TIME_SIG_FACTOR_OFFSET))?;
        song.ticks_per_bar = ((384u32 >> exponent).wrapping_mul(u32::from(song.numerator))) as u16;
        song.loop_start =
            rom.try_read_u32(h.wrapping_add(sound_rom::SEQ_HEADER_LOOP_START_OFFSET))?;
        song.loop_end = rom.try_read_u32(h.wrapping_add(sound_rom::SEQ_HEADER_LOOP_END_OFFSET))?;
        song.linked = rom.try_read_byte(h.wrapping_add(sound_rom::SEQ_HEADER_LINKED_OFFSET))?;

        for (track, config) in song.initial.iter_mut().enumerate() {
            *config = parse_initial_track(rom, layout, h, track)?;
        }

        // The driver only reads music stream offsets for channels the header assigns, so the HLE
        // plays the conductor, those music channels and the arrangement.
        let mut offsets = [0u32; TOTAL_SONG_STREAMS];
        let music_streams = song.initial.iter().enumerate().filter_map(|(track, config)| {
            (config.program != NO_PROGRAM).then_some(FIRST_MUSIC_STREAM_INDEX + track)
        });
        for stream in [CONDUCTOR_STREAM_INDEX, ARRANGEMENT_STREAM_INDEX]
            .into_iter()
            .chain(music_streams)
        {
            offsets[stream] = rom.try_read_u32(start + 4 + stream as u32 * 4)?;
        }

        for (slot, &offset) in song.tracks.iter_mut().zip(&offsets) {
            if offset == 0 {
                continue;
            }
            if offset < 48 || offset & 1 != 0 || start.wrapping_add(offset).wrapping_add(4) > end {
                return Err("invalid HLE track offset");
            }
            let bound = offsets
                .iter()
                .filter(|&&other| other > offset)
                .map(|&other| start.wrapping_add(other))
                .fold(end, u32::min);
            *slot = TrackData::parse(rom, start.wrapping_add(offset).wrapping_add(4), bound)?;
        }
        song.check_arrangement()?;
        Ok(song)
    }

    fn check_arrangement(&self) -> Result<(), &'static str> {
        if self.arrangement().len() > MAX_ARRANGEMENT_PHASES {
            return Err("HLE arrangement exceeds the 254-phase limit");
        }
        Ok(())
    }
}

fn parse_initial_track(
    rom: &RomView,
    layout: &DriverLayout,
    header_addr: u32,
    track_index: usize,
) -> Result<TrackConfig, &'static str> {
    let mut config = TrackConfig::default();

    // The driver restores even-offset controller defaults from a ROM table on every track init.
    for index in (0..=0x14u32).step_by(2) {
        config.parameters.set_byte(
            at::controllers + index as usize,
            rom.try_read_byte(layout.controller_defaults_table + index)?,
        );
    }

    let record = header_addr
        .wrapping_add(sound_rom::SEQ_HEADER_TRACKS_OFFSET)
        .wrapping_add((track_index as u32).wrapping_mul(sound_rom::SEQ_TRACK_RECORD_SIZE as u32));

    for n in 0..sound_rom::SEQ_TRACK_RECORD_SIZE as u32 {
        config
            .parameters
            .set_byte(at::program_record + n as usize, rom.try_read_byte(record.wrapping_add(n))?);
    }

    config.program = rom.try_read_u16(record)?;
    config.parameters.set_program_image(config.program);
    config.parameters.copy_record_to_live();

    Ok(config)
}

/// Every sequence slot of one ROM bank; absent slots have `present == false`.
pub struct SequenceBank {
    songs: Box<[SongData; SONG_SLOTS]>,
}

impl SequenceBank {
    pub fn try_parse(rom: &RomView, layout: &DriverLayout) -> Result<SequenceBank, &'static str> {
        let stream_limit = SOUND_ROM_BASE.wrapping_add(SOUND_ROM_IMAGE_SIZE);
        let header_rel = rom.try_read_u32(STREAM_BASE)?;
        if header_rel < 8 + SONG_SLOTS as u32 * 4
            || header_rel & 1 != 0
            || STREAM_BASE.wrapping_add(header_rel) >= stream_limit
        {
            return Err("unsupported HLE sequence bank layout");
        }

        let header = STREAM_BASE.wrapping_add(header_rel);
        let header_size = rom.try_read_u32(header)?;
        if header_size < 8 + SONG_SLOTS as u32 * 2 + sound_rom::SEQ_HEADER_SIZE
            || header.wrapping_add(header_size) > stream_limit
        {
            return Err("unsupported HLE sequence bank layout");
        }

        let songs = (0..SONG_SLOTS as u32)
            .map(|id| Self::parse_song(rom, layout, header, id))
            .collect::<Result<Vec<SongData>, _>>()?;
        Ok(SequenceBank::from_songs(songs))
    }

    /// `songs` holds one entry per slot. Boxed from a Vec: 100 songs would otherwise be built on the
    /// stack first.
    pub fn from_songs(songs: Vec<SongData>) -> SequenceBank {
        let songs = songs.into_boxed_slice().try_into().unwrap_or_else(|_| {
            panic!("a sequence bank holds {SONG_SLOTS} songs")
        });
        SequenceBank { songs }
    }

    fn parse_song(
        rom: &RomView,
        layout: &DriverLayout,
        header: u32,
        id: u32,
    ) -> Result<SongData, &'static str> {
        let relative = rom.try_read_u32(STREAM_BASE.wrapping_add(8).wrapping_add(id * 4))?;
        if relative == 0 {
            return Ok(SongData::default());
        }

        let start = STREAM_BASE.wrapping_add(relative);
        let end = start.wrapping_add(rom.try_read_u32(start)?);
        if relative < 408 || relative & 1 != 0 || end > header || end < start.wrapping_add(48) {
            return Err("invalid HLE sequence extent");
        }

        let header_offset = rom.try_read_u16(header.wrapping_add(8).wrapping_add(id * 2))?;
        if u32::from(header_offset) < 208
            || u32::from(header_offset).wrapping_add(sound_rom::SEQ_HEADER_SIZE)
                > rom.try_read_u32(header)?
        {
            return Err("invalid HLE sequence header");
        }

        let record = header.wrapping_add(u32::from(header_offset));
        SongData::parse(rom, layout, record, start, end)
    }

    /// `id` must be below `SONG_SLOTS`.
    pub fn song(&self, id: u8) -> &SongData {
        &self.songs[usize::from(id)]
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use crate::hle::driver_layout::STANDARD;

    fn song_with_arrangement(events: usize) -> SongData {
        let mut song = SongData::default();
        let mut stream = vec![StreamEvent { opcode: EV_ARRANGEMENT, ..StreamEvent::default() }; events - 1];
        stream.push(StreamEvent { opcode: EV_END, ..StreamEvent::default() });
        song.tracks[ARRANGEMENT_STREAM_INDEX] = TrackData { events: stream };
        song
    }

    #[test]
    fn a_playlist_leaves_the_last_u8_phase_free() {
        let longest = usize::from(u8::MAX) - 1;
        assert_eq!(song_with_arrangement(longest).check_arrangement(), Ok(()));
        assert!(song_with_arrangement(longest + 1).check_arrangement().is_err());
    }

    #[test]
    fn a_zeroed_rom_has_no_sequence_bank() {
        let rom: Arc<[u8]> = vec![0; SOUND_ROM_IMAGE_SIZE as usize].into();
        let reason = SequenceBank::try_parse(&RomView::with_image(rom, 1), &STANDARD).err();
        assert_eq!(reason, Some("unsupported HLE sequence bank layout"));
    }

    #[test]
    fn a_stream_without_an_end_event_is_an_error_not_a_panic() {
        let rom: Arc<[u8]> = vec![0; SOUND_ROM_IMAGE_SIZE as usize].into();
        let view = RomView::with_image(rom, 1);
        let start = SOUND_ROM_BASE;
        assert_eq!(TrackData::parse(&view, start, start + 2).err(), Some("HLE track missing end event"));
    }

    #[test]
    fn a_read_past_the_image_is_an_error() {
        let rom: Arc<[u8]> = vec![0; SOUND_ROM_IMAGE_SIZE as usize].into();
        let view = RomView::with_image(rom, 1);
        let last = SOUND_ROM_BASE + SOUND_ROM_IMAGE_SIZE - 1;
        assert_eq!(view.try_read_u16(last), Err("HLE ROM read outside image"));
        assert_eq!(view.try_read_u32(last - 2), Err("HLE ROM read outside image"));
        assert_eq!(view.try_read_byte(last), Ok(0));
    }
}
