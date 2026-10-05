#!/usr/bin/env python3
"""Decode Land Maker's ROM sequence banks without executing its sound program.

All integers in the ROM are big endian. The c16024..c1603a reset path installs
stream bank c20000 and header bank stream+u32(stream). c158ae/c158cc index their
+8 directories (100 longs / 100 words). A sequence starts with its inclusive
byte size and eleven track-relative longs; each nonzero track offset selects a
declared-length long followed by word events. Header records are 0xa8 bytes:
+1 flags, +2..11 name/mute bits, +12 position, +14 beat count/beat exponent,
+16/+1a loop bounds, +1e rate, +1f special-track selector, +20 eight 14-byte
channel records, +90 eight auxiliary bytes, +98 effect data, +a6 linked ID.
Playback skips the track's length long (c15a64/c13f40), and stops on e9, not
that length. Arrangement tracks declare 0x42 despite occupying only 0x24 bytes;
their physical bound comes from the sequence/next track, never that field.

c128a8/c12968 copy each channel record to channel+2a..37. Record word +0 is
the program, then bytes +2/+3/+4/+5 are restored to channel+20/+26/+1a/+1c
by c12858; +6 goes to +33. We retain address-named controllers rather than
inventing MIDI meanings. Physical channels are allocated highest-free first
(56..1); their previous-key byte survives sequence destruction for portamento.

c1475a skips words with bit15 clear. Event token low byte selects the class;
bits14..8 are the delay AFTER applying the event (c140ac), not before it.
Tracks initially have countdown1 (c13f46), but c13e7c immediately branches to
c13f66 for the startup scan, so the first event is tick0.
Notes 00..57: operand bits9..0 duration (zero => next word), velocity from
bits14..10 expanded as (v<<2)|(v>>3), except zero expands to1.
JSON key is the wire/class key; kernel_key is key+21 before runtime transposition.
58..af are key continuation events, b0..d8 channel-byte controls (index*2),
d9 changes channel+33 and dispatches the program/bank selector at c0ed7e;
da/db are special-track channel+20/+26 changes, dc is a
signed word-relative note reference from the reference operand's address.
e6 takes a full delay word OR token bit14 shifted to bit15; e7 takes four
arrangement words; e8 notifies; e9 ends/restarts according to runtime mode.
The e7 callback c145a8 stores selected sequence, an unidentified d0a8 word,
a transposition-track mask (c141a6), and repeat/transposition packed word.
Repeat100 sets d4c1: c14d36 bypasses repeat decrement/arrangement advancement.
Only the conductor context acts on e7. All supplied d0a8 and mask words are0.
For the supplied headers, ordinary 80/81 does not play track10. With the
sequence-ID high bit set, c12bea/c12d0e selects a child sequence, skipping
repeat100 markers and returning to the phase following the last marker at E9.
Mailbox starts first stop/reset the selection; internal section transitions
advance it. Events belong to the selected child, not the arrangement ID.
No e8/dc/58..af/da/db occur in the supplied banks. dd..e5 and ea..ff are
not understood: decoding fails explicitly instead of silently ignoring them.
D9 selector00..63 selects program (bank_byte<<14)|selector, 64..7a is ignored,
7b sets the channel bit in d740, and 7c..7f stores selector-7c as bank byte
at5e1c+channel (c0ed90..edda). The bank is mutable runtime channel state.

This module emits immutable event operands and flow, not a CPU, voice renderer,
or trace replay. A runtime can consume generated events directly and need not
implement this binary parsing again. Tick fields describe one linear traversal;
loop/restart policy remains explicit, never unrolled into guessed repetitions.
"""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import zlib

ROM_BASE = 0xC00000
STREAM_BASE = 0xC20000
ROM_CRC = 0x5A7E9117
SLOT_COUNT = 100
HEADER_SIZE = 0xA8


class DecodeError(ValueError):
    """Invalid ROM data or an event whose grammar is not established."""


class Rom:
    def __init__(self, data: bytes):
        self.data = data

    def bytes(self, address: int, size: int) -> bytes:
        off = address - ROM_BASE
        if off < 0 or size < 0 or off + size > len(self.data):
            raise DecodeError(f"ROM read outside supplied image: {address:08x}+{size:x}")
        return self.data[off:off + size]

    def u16(self, address: int) -> int:
        return struct.unpack('>H', self.bytes(address, 2))[0]

    def u32(self, address: int) -> int:
        return struct.unpack('>I', self.bytes(address, 4))[0]


def load_rom(directory: str | Path) -> bytes:
    """Interleave the actual sound chips exactly as runtime/rom.cpp does."""
    directory = Path(directory)
    chips = [(directory / name).read_bytes() for name in ('e61-14.32', 'e61-15.33')]
    if any(len(chip) != 0x20000 for chip in chips):
        raise DecodeError('Land Maker sound chips must each contain 0x20000 bytes')
    hi, lo = (chip.ljust(0x40000, b'\xff') for chip in chips)
    data = bytearray(0x80000)
    data[0::2], data[1::2] = hi, lo
    return bytes(data)


def _note(rom: Rom, address: int, key: int, limit: int) -> tuple[dict, int]:
    if address + 2 > limit:
        raise DecodeError(f'truncated note operand at {address:08x}')
    packed = rom.u16(address)
    duration = packed & 0x3FF
    end = address + 2
    if not duration:
        if end + 2 > limit:
            raise DecodeError(f'truncated extended duration at {end:08x}')
        duration = rom.u16(end)
        end += 2
    v = (packed >> 8) & 0x7C
    velocity = (v | (v >> 5)) if v else 1
    return {'key': key, 'kernel_key': key + 0x15, 'velocity': velocity,
            'duration': duration, 'packed_note': packed}, end


def decode_track(data: bytes | Rom, address: int, limit: int | None = None) -> dict:
    """Return a track whose events follow the length long at ``address``.

    Every event retains its token, operands and address; delay_after is in
    sequence ticks, not milliseconds. This routine does not execute callbacks.
    """
    rom = data if isinstance(data, Rom) else Rom(data)
    size = rom.u32(address)
    end = limit if limit is not None else ROM_BASE + len(rom.data)
    if size < 6 or size & 1 or address + 6 > end:
        raise DecodeError(f'invalid track extent at {address:08x}: size={size:x}')
    rom.bytes(address, end - address)
    p, tick = address + 4, 0
    events, skipped = [], []
    while p < end:
        start = p
        token = rom.u16(p)
        p += 2
        if not token & 0x8000:
            skipped.append({'address': start, 'word': token})
            continue
        opcode = token & 0xFF
        event = {'address': start, 'token': token, 'opcode': opcode,
                 'tick': tick, 'delay_after': (token >> 8) & 0x7F}
        if opcode < 0x58:
            note, p = _note(rom, p, opcode, end)
            event.update(kind='note', **note)
        elif opcode < 0xDC:
            if p + 2 > end:
                raise DecodeError(f'truncated control operand at {p:08x}')
            operand = rom.u16(p)
            p += 2
            event.update(operand=operand, value=operand & 0x7F)
            if opcode < 0xB0:
                event.update(kind='key_continuation', key=opcode - 0x58,
                             kernel_key=opcode - 0x58 + 0x15)
            elif opcode < 0xD9:
                index = opcode - 0xB0
                event.update(kind='controller', index=index,
                             channel_offset=0x0A + index * 2)
            elif opcode == 0xD9:
                event.update(kind='program_selector', channel_offset=0x33)
            elif opcode == 0xDA:
                event.update(kind='special_volume', channel_offset=0x20,
                             alternate_channels=bool(operand & 1))
            else:
                event.update(kind='special_balance', channel_offset=0x26,
                             stored_value=((operand & 0x7F) * 2 + 0x80) & 0xFF,
                             alternate_channels=bool(operand & 1))
        elif opcode == 0xDC:
            if p + 2 > end:
                raise DecodeError(f'truncated note reference at {p:08x}')
            relative = rom.u16(p)
            relative = relative if relative < 0x8000 else relative - 0x10000
            target = p + relative
            target_token = rom.u16(target)
            key = target_token & 0xFF
            if key >= 0x58:
                raise DecodeError(f'non-note reference target {target:08x}')
            note, _ = _note(rom, target + 2, key, ROM_BASE + len(rom.data))
            event.update(kind='note_reference', target=target, relative=relative,
                         target_token=target_token, **note)
            p += 2
        elif opcode == 0xE6:
            if p + 2 > end:
                raise DecodeError(f'truncated full delay at {p:08x}')
            operand = rom.u16(p)
            p += 2
            event.update(kind='delay', operand=operand,
                         delay_after=operand | ((token << 1) & 0x8000))
        elif opcode == 0xE7:
            if p + 8 > end:
                raise DecodeError(f'truncated arrangement at {p:08x}')
            words = [rom.u16(p + n * 2) for n in range(4)]
            p += 8
            transpose = words[3] * 2 & 0xFFFF
            transpose = transpose if transpose < 0x8000 else transpose - 0x10000
            event.update(kind='arrangement', payload_words=words,
                         selected_sequence=words[0], ram_d0a8=words[1],
                         transpose_track_mask=words[2],
                         repeat=words[3] & 0x7F,
                         repeat_forever=(words[3] & 0x7F) == 100,
                         transpose=transpose >> 11, delay_after=0)
        elif opcode == 0xE8:
            event.update(kind='notification', delay_after=0)
        elif opcode == 0xE9:
            event.update(kind='end', delay_after=0)
        else:
            raise DecodeError(f'unsupported event {opcode:02x} at {start:08x}')
        event['words'] = [rom.u16(a) for a in range(start, p, 2)]
        events.append(event)
        tick += event['delay_after']
        if opcode == 0xE9:
            break
    if not events or events[-1]['kind'] != 'end':
        raise DecodeError(f'track {address:08x} lacks an end event')
    return {'address': address, 'size': p - address, 'declared_length': size, 'events': events,
            'linear_end_tick': tick, 'skipped_words': skipped}


def decode_header(data: bytes | Rom, address: int) -> dict:
    rom = data if isinstance(data, Rom) else Rom(data)
    raw = rom.bytes(address, HEADER_SIZE)
    channels = []
    for index in range(8):
        off = 0x20 + index * 14
        record = raw[off:off + 14]
        channels.append({'logical_track': index + 1,
                         'program': int.from_bytes(record[:2], 'big'),
                         'saved_channel_bytes': {f'{0x2A+n:02x}': value
                                                 for n, value in enumerate(record)},
                         'initial_controllers': {'20': record[2], '26': record[3],
                                                 '1a': record[4], '1c': record[5],
                                                 '33': record[6]},
                         'header_90_byte': raw[0x90 + index], 'raw': record.hex()})
    return {'address': address, 'flags': raw[1], 'name_bytes': raw[2:0x12].hex(),
            'initial_position': rom.u16(address + 0x12),
            'beat_count': raw[0x14], 'beat_exponent': raw[0x15],
            'ticks_per_beat': 0x180 >> raw[0x15],
            'loop_start': rom.u32(address + 0x16),
            'loop_end': rom.u32(address + 0x1A),
            'rate': raw[0x1E], 'special_track': raw[0x1F],
            'linked_sequence': raw[0xA6], 'channels': channels,
            'effects_raw': raw[0x98:0xA6].hex(), 'raw': raw.hex()}


def decode_sequences(data: bytes, *, validate_crc: bool = True) -> dict:
    """Decode all nonzero ROM-directory entries into a runtime-ready data model."""
    crc = zlib.crc32(data)
    if validate_crc and crc != ROM_CRC:
        raise DecodeError(f'unsupported sound ROM CRC {crc:08x}, expected {ROM_CRC:08x}')
    rom = Rom(data)
    stream_size = rom.u32(STREAM_BASE)
    header_base = STREAM_BASE + stream_size
    header_size = rom.u32(header_base)
    rom.bytes(STREAM_BASE, stream_size)
    rom.bytes(header_base, header_size)
    if stream_size < 408 or header_size < 208:
        raise DecodeError('bank shorter than its 100-entry directory')
    sequences = []
    for sequence_id in range(SLOT_COUNT):
        offset = rom.u32(STREAM_BASE + 8 + sequence_id * 4)
        header_offset = rom.u16(header_base + 8 + sequence_id * 2)
        if not offset:
            continue
        if offset < 408 or offset & 1 or not header_offset or header_offset & 1:
            raise DecodeError(f'invalid directory entry for sequence {sequence_id}')
        start = STREAM_BASE + offset
        size = rom.u32(start)
        end = start + size
        if size < 48 or size & 1 or end > header_base:
            raise DecodeError(f'invalid sequence {sequence_id} extent')
        if header_offset + HEADER_SIZE > header_size:
            raise DecodeError(f'header outside bank for sequence {sequence_id}')
        tracks = []
        offsets = [rom.u32(start + 4 + index * 4) for index in range(11)]
        for index, relative in enumerate(offsets):
            if not relative:
                continue
            if relative < 48 or relative & 1 or relative + 4 > size:
                raise DecodeError(f'invalid sequence {sequence_id} track {index} offset')
            next_offset = min((other for other in offsets if other > relative), default=size)
            track = decode_track(rom, start + relative, start + next_offset)
            track['logical_track'] = index
            tracks.append(track)
        sequences.append({'id': sequence_id, 'address': start, 'size': size,
                          'header': decode_header(rom, header_base + header_offset),
                          'tracks': tracks})
    histogram = Counter(event['opcode'] for seq in sequences for track in seq['tracks']
                        for event in track['events'])
    return {'format': 'landmakr-hle-sequence-1', 'rom_crc32': f'{crc:08x}',
            'stream_base': STREAM_BASE, 'header_base': header_base,
            'clock': {'irq_hz': 1000, 'rate_accumulator_threshold': 625,
                      'ticks_per_quarter': 96, 'initial_track_countdown': 1,
                      'initial_event_tick': 0},
            'event_counts': {f'{opcode:02x}': count for opcode, count in sorted(histogram.items())},
            'sequences': sequences}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--rom-dir', type=Path)
    source.add_argument('--sound-rom', type=Path, help='already interleaved/padded sound ROM')
    parser.add_argument('--output', type=Path, required=True, help='JSON destination under build/')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent / 'build'
    destination = args.output.resolve()
    if not destination.is_relative_to(root.resolve()):
        parser.error('--output must be under this worktree\'s build/ directory')
    try:
        data = load_rom(args.rom_dir) if args.rom_dir else args.sound_rom.read_bytes()
        decoded = decode_sequences(data)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(decoded, separators=(',', ':')) + '\n')
    except (DecodeError, OSError) as exc:
        parser.exit(1, f'{exc}\n')
    print(f"Decoded {len(decoded['sequences'])} sequences to {destination}")


if __name__ == '__main__':
    main()
