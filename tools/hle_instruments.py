#!/usr/bin/env python3
"""Land Maker immutable instrument decoder; never executes sound CPU/device code.

Evidence addresses are sound-68000 addresses. Sample metadata is read directly
from PCM ROM, reproducing c0b4a4..c0b610's *data format*, not its device polling.
Generated JSON contains copyrighted ROM data: keep it in ignored build/.

Modern-rate synthesis contract: sample positions are Q9 word positions, pitch
is Q8 semitones, envelope positions/slopes are wrapping Q16 driver units.
Call envelope advance once per scheduled voice update, not once per PCM frame.
The driver's rotating update schedule (c18b16..c18c36) must be supplied by the
runtime; no millisecond/ADSR time constants are inferred here. Interpolate the
resulting gains/filter targets at mixer rate. ROM coefficient and attenuation
lookup tables are exported verbatim rather than replaced by guessed curves.
"""
import argparse
from dataclasses import asdict, dataclass, replace
import json
from pathlib import Path
import struct
import zlib

ROM_BASE = 0xc00000
SPLIT_BASE = 0xc1ae76
SPLIT_INDEX = 0xc1c1e0
RAM_METADATA = 0x6e9c


def signed(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def be16(data, offset=0):
    return struct.unpack_from('>H', data, offset)[0]


def be32(data, offset=0):
    return struct.unpack_from('>I', data, offset)[0]


@dataclass(frozen=True)
class Split:
    address: int
    tuning_q8: int
    accumulator_q9: int
    start_q9: int
    end_q9: int
    cutoff: int
    mode: int
    raw: str

    @classmethod
    def decode(cls, address, data):
        if len(data) != 14:
            raise ValueError('Sample split must contain 14 bytes')
        return cls(address, signed(be16(data), 16), be32(data, 2),
                   be32(data, 6), be32(data, 10), data[5], data[9], data.hex())

    def region(self, bank_selector, reverse=False):
        """c19620..c196e2 direct-record path (ordinary PCM split IDs <0x78).

        The bank selector's low bit inserts bit28 in the Q9 address, i.e.
        bit19 in the word index, NOT bit20. Board bank is selector>>1.
        Metadata cutoff/mode occupy low bytes. The direct path preserves ACC
        low bits; the layered c180a0 path separately clears ACC's low byte.
        START/END hardware discard low5 bits and mask to29 address bits.
        """
        bit = (bank_selector & 1) << 28
        start = self.accumulator_q9 if reverse else self.start_q9
        accumulator = self.end_q9 if reverse else self.accumulator_q9
        start = (start | bit) & 0x1fffffe0
        end = (self.end_q9 | bit) & 0x1fffffe0
        accumulator = (accumulator | bit) & 0x1fffffff
        board_base = ((bank_selector >> 1) & 7) << 20
        return dict(bank_selector=bank_selector, board_bank=bank_selector >> 1,
                    start_q9=start, end_q9=end, accumulator_q9=accumulator,
                    start_word=board_base + start / 512,
                    end_word=board_base + end / 512,
                    accumulator_word=board_base + accumulator / 512)


@dataclass(frozen=True)
class SampleBank:
    selector: int
    tag: int
    packed: bool
    directory_address: int
    count: int
    probe_q9: int
    data: bytes

    def split_offset(self, index):
        if not 0 <= index < self.count:
            raise ValueError(f'Bank {self.selector} has no sample {index}')
        # c19450: index*14, then signed word offset at directory+2.
        return signed(be16(self.data, 2 + index * 14), 16)

    def splits(self, index):
        offset = self.split_offset(index)
        result = []
        while True:
            if offset < 0 or offset + 14 > len(self.data):
                raise ValueError('Sample directory split escapes decoded metadata')
            split = Split.decode(self.directory_address + offset,
                                 self.data[offset:offset + 14])
            result.append(split)
            if split.cutoff == 0x7f:
                return result
            offset += 14


def decode_sample_banks(samples):
    """Construct immutable equivalents of RAM6e1c table and RAM6e9c metadata.

    Header word bit15 selects high-byte packed metadata (two PCM words per
    metadata word). The first flag word is not packed. Directory entries are
    offset-word plus12 name bytes. A zero offset terminates the directory;
    14-byte split records are copied through the last entry's cutoff7f. Packed offsets acquire
    +1 because the expanded flag word occupies two bytes rather than one.
    The identity word at sample word7ffff yields its high byte in packed mode.
    All supplied Land Maker populated banks use this representation.
    """
    if len(samples) != 0x1000000:
        raise ValueError('Expected 16MiB board sample region')
    result = []
    address = RAM_METADATA
    for selector in range(16):
        base = selector * 0x80000
        cursor = 0

        def sample_word():
            nonlocal cursor
            if cursor >= 0x80000:
                raise ValueError('Metadata read exceeds bank half')
            value = be16(samples, (base + cursor) * 2)
            cursor += 1
            return value

        flag = sample_word()
        packed = bool(flag & 0x8000)

        def metadata_word():
            if not packed:
                return sample_word()
            return (sample_word() & 0xff00) | (sample_word() >> 8)

        output = bytearray(struct.pack('>H', flag))
        count = 0
        last = 0
        valid = True
        while True:
            offset = metadata_word()
            output.extend(struct.pack('>H', offset))
            if offset == 0:
                break
            limit = offset * 2 + 4 if packed else offset + 8
            if limit <= cursor * 2 or limit % 14:
                valid = False
                break
            last = offset * 2 if packed else offset
            if packed:
                output[-2:] = struct.pack('>H', offset + 1)
            count += 1
            for _ in range(6):
                output.extend(struct.pack('>H', metadata_word()))
        probe = cursor * 2
        tag = 0
        if valid and last:
            while cursor * 2 < last:
                output.extend(struct.pack('>H', metadata_word()))
            if cursor * 2 != last:
                raise ValueError('Unaligned split metadata start')
            while True:
                record = b''.join(struct.pack('>H', metadata_word()) for _ in range(7))
                output.extend(record)
                if record[5] == 0x7f:
                    break
            tag_word = be16(samples, (base + 0x7ffff) * 2)
            tag = tag_word >> 8 if packed else tag_word
        else:
            count = 0
            probe = 4  # Empty headers retain the first two decoded words.
        result.append(SampleBank(selector, tag, packed, address, count,
                                 (probe << 8) | ((selector & 1) << 28), bytes(output)))
        address += len(output)
    return result


@dataclass
class EnvelopeState:
    """Driver wrapping position, signed slope and sign-biased target.

    c18444/c18c3a and c19722/c19730 use N (not signed-overflow comparison)
    on position-target. Exposing this arithmetic avoids invented ADSR curves.
    """
    position: int
    slope: int
    target: int

    def advance(self):
        self.position = (self.position + self.slope) & 0xffffffff
        return signed(self.position - self.target, 32) >= 0

    @property
    def level(self):
        return signed(self.position >> 16, 16)


class Instruments:
    def __init__(self, sound, samples):
        self.sound = bytes(sound)
        if len(sound) != 0x80000 or zlib.crc32(sound) != 0x5a7e9117:
            raise ValueError('Expected Land Maker sound CRC5a7e9117')
        self.banks = decode_sample_banks(samples)
        metadata = bytearray(b''.join(bank.data for bank in self.banks))
        # c15fb4..c15fde initializes the heap. Its header overlaps bankc8
        # sample0's first split: reconstruct these boot writes from ROM operands.
        heap_base = be16(self.sound,0x15fb6)
        heap_limit = be32(self.sound,0x15fba) + signed(be32(self.sound,0x15fc0),32)
        heap_size = heap_limit - heap_base - be32(self.sound,0x15fcc)
        header = struct.pack('>II',heap_size,be32(self.sound,0x15fd8))
        offset = heap_base - RAM_METADATA
        metadata[offset:offset+len(header)] = header
        self.metadata = bytes(metadata)
        self.banks = [replace(bank,data=self.metadata[
            bank.directory_address-RAM_METADATA:bank.directory_address-RAM_METADATA+len(bank.data)])
            for bank in self.banks]
        self.boot_overlay = dict(address=heap_base,raw=header.hex(),evidence='c15fb4..c15fde')

    @staticmethod
    def linear_gain(encoded_volume):
        """Native gain arithmetic, runtime/third_party/audio/es5505.cpp:160.

        Scalar logarithmic-to-linear conversion; does not execute a device.
        """
        volume = (encoded_volume >> 8) & 255
        gain_q11 = (((volume & 15) | 16) << 11) >> (16 - (volume >> 4))
        return gain_q11 / 2048

    @classmethod
    def load(cls, directory):
        directory = Path(directory)
        sound = bytearray(0x80000)
        for lane, name in enumerate(('e61-14.32', 'e61-15.33')):
            data = (directory / name).read_bytes()
            if len(data) not in (0x20000, 0x40000):
                raise ValueError(f'Wrong sound chip length: {name}')
            sound[lane::2] = data.ljust(0x40000, b'\xff')
        samples = bytearray(0x1000000)
        for name, offset, crc in (('e61-04.38', 0x400000, 0xc27aec0c),
                                  ('e61-05.39', 0x800000, 0x83920d9d),
                                  ('e61-06.40', 0xc00000, 0x2e717bfe)):
            data = (directory / name).read_bytes()
            if len(data) != 0x200000 or zlib.crc32(data) != crc:
                raise ValueError(f'Wrong sample chip: {name}')
            samples[offset:offset + len(data) * 2:2] = data
        return cls(sound, samples)

    def read(self, address, length):
        # Bank0 is the exact c11a88 boot copy of c00134..c050e3.
        if 0x15e <= address < 0x510e:
            address += 0xc00134 - 0x15e
        if RAM_METADATA <= address < RAM_METADATA + len(self.metadata):
            offset = address - RAM_METADATA
            data = self.metadata[offset:offset + length]
        else:
            offset = address - ROM_BASE
            if offset < 0:
                raise ValueError(f'Not immutable instrument data: {address:#x}')
            data = self.sound[offset:offset + length]
        if len(data) != length:
            raise ValueError('Instrument read exceeds source')
        return data

    def word(self, address):
        return be16(self.read(address, 2))

    def table(self, address, count, signed_words=False):
        data = self.read(address, count * 2)
        values = [be16(data, i * 2) for i in range(count)]
        return [signed(v, 16) for v in values] if signed_words else values

    def pitch_frequency(self, pitch_q8):
        """c19480..c194d6/c1874e: exact ROM pitch lookup, FC lowbit discarded."""
        pitch = max(-0x4800, min(0x5400, signed(pitch_q8, 16)))
        if pitch < 0:
            shift = 0
            while pitch < 0:
                shift += 1
                pitch += 0xc00
            value = self.word(0xc09e6c + ((pitch >> 1) & 0xfffe)) >> shift
        else:
            shift = -1
            while True:
                shift += 1
                pitch -= 0xc00
                if pitch < 0:
                    break
            offset = signed((pitch >> 1) & 0xfffe,16)
            value = (self.word(0xc0a46c + offset) << shift) & 0xffff
        return value & 0xfffe

    @staticmethod
    def program_address(program):
        """c11a0a: high two bits choose bank; lowbyte selects packed column."""
        if program == 0xffff:
            raise ValueError('ffff is no-program, not an instrument')
        base = (0x15e, 0xc00134, 0xc7b050, 0xc760a0)[(program >> 14) & 3]
        index = program & 0xff
        column = 0
        if index >= 0x50:
            base += 0x3fc0
            index -= 0x50
            if index >= 10:
                index -= 10
                column += 1
        if index >= 0x28:
            index -= 0x28
            column += 1
        return base + index * 0x198 + column

    def program(self, program):
        address = self.program_address(program)
        data = self.read(address, 0x198)
        if all(v == 0xff for v in data):
            raise ValueError(f'Program {program:04x} selects unpopulated ROM')
        # c172c8 reads program+196; channel headers come from sequence data.
        header = bytes(data[0x17c:0x198:2])
        direct = (header[13] >> 5) == 0
        signature = bytes(data[0x154:0x162:2])
        result = dict(program=program, address=address, program_tail=header.hex(),
                      layer_enable=header[13] >> 5,
                      raw=data.hex(), kind='key_regions' if direct else 'layers',
                      sample_bank_signature=signature.hex())
        if direct:
            result['regions'] = [self.direct_record(address + i * 20,
                                                    data[i * 20:i * 20 + 20])
                                 for i in range(17)]
            # c1941a XE mark + bitmask selects external bank per key region.
            result['external_region_mask'] = int.from_bytes(signature[:4], 'big')
            result['external_bank_tag'] = signature[6] if signature[4:6] == b'XE' else None
        else:
            result['layers'] = [self.layer(address + i * 0x76,
                                          data[i * 0x76:i * 0x76 + 0x76])
                                for i in range(3)]
            for number, layer in enumerate(result['layers']):
                layer['enabled'] = bool(result['layer_enable'] & (1 << number))
                if not layer['enabled']:
                    continue  # c172e2 rejects this layer before any sample lookup.
                raw = bytes.fromhex(layer['raw'])
                marker = sum(1 << i for i in range(0,16,2) if raw[i] & 0x80)
                tag = sum(1 << (i//2) for i in range(0,16,2) if raw[20+i] & 0x80)
                banks = [b for b in self.banks if b.tag == tag] if marker == 0x5515 else []
                bank = banks[-1] if banks else None
                if marker == 0x5515 and bank is None:
                    raise ValueError('Layer selects unknown external sample bank tag')
                layer['bank_selector'] = bank.selector if bank else 14
                layer['external_bank_tag'] = tag if bank else None
                if bank:
                    splits = bank.splits(layer['sample_id'])
                else:
                    first = self.select_split(layer['sample_id'],0)
                    splits = []
                    at = first.address
                    while True:
                        split = Split.decode(at,self.read(at,14))
                        splits.append(split)
                        if split.cutoff == 127:
                            break
                        at += 14
                layer['splits'] = [asdict(s) for s in splits]
        return result

    def direct_record(self, address, data):
        b = data[::2]
        return dict(address=address, raw=data.hex(), hold_rate=b[0] & 0x7f,
                    hold_on_release=bool(b[0] & 0x80), release_rate=b[1] & 0x7f,
                    key_tracking=bool(b[1] & 0x80), sample_id=b[2] & 0x7f,
                    reverse=bool(b[2] & 0x80), transpose=signed(b[3], 8),
                    fine_q8=(b[4] & 15) << 4, velocity_gain=b[4] >> 4,
                    key_low=b[5], key_high=b[6], filter_byte=b[7],
                    amplitude=b[8] & 0x7f, amplitude_boost=bool(b[8] & 0x80),
                    velocity_curve=b[9] >> 6, pan_nibble=b[9] & 15,
                    output_pair_selector=(b[9] >> 4) & 3)

    def layer(self, address, data):
        b = data[::2]
        return dict(address=address, raw=data.hex(), envelopes=[
            dict(raw=data[o:o + 20].hex(), levels=[data[o + j] & 0x7f for j in (0,4,8,12)],
                 rates=[data[o + j] & 0x7f for j in (2,6,10,14)],
                 flags=data[o + 16], sensitivity=data[o + 18])
            for o in (0,20,40)], sample_id=b[54] & 0x7f,
            reverse=bool(b[54] & 0x80), velocity_threshold=b[55] & 0xf0,
            deferred=b[58] != 0,
            allocation_mode=(b[34] >> 4) & 3,
            priority=b[48] & 0xc0)

    def envelope_parameters(self, raw, key, velocity, amplitude=False):
        """c18c56..c18cf0: three software envelopes, shared 20-byte format.

        The first two feed pitch/filter modulation; the third feeds volume.
        All return driver units, not seconds. Third-envelope large-rise
        intermediate stage is retained explicitly (c18cf8..c18d3c).
        """
        if isinstance(raw, str):
            raw = bytes.fromhex(raw)
        if len(raw) != 20:
            raise ValueError('Envelope descriptor must be 20 bytes')
        flags, sensitivity = raw[16], raw[18]
        curve = self.read(0xc08c6c + ((flags & 0x30) << 3) + velocity, 1)[0]
        gain = sensitivity >> 4
        gain = gain * 8 + (gain >> 1)
        scale = signed(0x4000 - (128 - curve) * gain,16) >> 6
        levels = [(raw[i] & 127) * scale & 0xffff for i in (0,4,8,12)]
        key_rate_scale = signed(flags & 15,4) * 18
        key_rate_scale = -(signed((key-64)*key_rate_scale,16) >> 6)
        velocity_rate_scale = (sensitivity & 15) * 9
        rates = [(raw[i] &127) for i in (2,6,10,14)]
        rates[0] -= velocity * velocity_rate_scale >> 7
        rates[1] += key_rate_scale
        rates[2] += key_rate_scale
        rates = [max(0,min(99,r)) for r in rates]
        intermediate = None
        if amplitude and signed(levels[1]-levels[0],16) > 0x3000 and rates[0] > 20:
            intermediate = dict(target=levels[1]-0x1400, rate=max(0,rates[0]-10))
        return dict(levels=levels,rates=rates,scale=scale,
                    intermediate=intermediate, repeat=bool(flags&0x80),
                    ignore_key_off=bool(flags&0x40), automatic_release=bool(flags&0x40), flags=flags,
                    evidence='c18c56..c18ef8')

    def envelope_segment(self, current, target_level, rate):
        """c18ebe: exact segment slope, including equal-level delay segment."""
        rate = max(0,min(99,rate))
        multiplier = signed(self.word(0xc089dc + rate*2),16)
        difference = signed(target_level-(current>>16),16)
        if difference:
            slope = signed(difference*multiplier*2,32)
            position = current & 0xffff0000
        else:
            slope = -multiplier
            position = (current&0xffff0000)|0x7fff
        target = ((target_level ^ (0x8000 if slope <= 0 else 0))&0xffff)<<16
        return EnvelopeState(position,slope,target)

    def direct_release(self, voice, current):
        """c19824..c19878. None means patch intentionally holds on key-off."""
        if voice['record']['hold_on_release']:
            return None
        multiplier = signed(self.word(0xc089dc+voice['release_rate']*2),16)
        slope = signed(-0x7f00*multiplier*2,32)
        return EnvelopeState(current&0xffff0000,slope,0x80000000 if slope <= 0 else 0)

    def select_split(self, sample_id, key_q8, bank=None):
        if bank is not None:
            splits = bank.splits(sample_id)
        else:
            if not 0 <= sample_id < 121:
                raise ValueError('ROM split index outside documented 121 entries')
            address = SPLIT_BASE + signed(self.word(SPLIT_INDEX + sample_id * 2),16)
            splits = []
            while True:
                split = Split.decode(address, self.read(address,14))
                splits.append(split)
                if split.cutoff == 127:
                    break
                address += 14
        key = (key_q8 >> 8) & 255
        for split in splits:
            if key <= split.cutoff:
                return split
        raise ValueError('No split covers effective key')

    def direct_voice(self, program, note, velocity, channel_transpose=None,
                     channel_pitch=0, pitch_offset_q8=0):
        """c173e6/c192ee: data-only direct-note initializer.

        channel_pitch is channel descriptor+15. Runtime volume/sequence/pan
        controllers are intentionally separate inputs to direct_volume().
        """
        patch = self.program(program)
        if patch['kind'] != 'key_regions':
            raise ValueError('Program uses layered synthesis, not direct records')
        transpose = 0 if channel_transpose is None else channel_transpose
        # c140d6 adds21 to stream/mailbox keys before the voice factory.
        key = (note + 21 + transpose) & 255
        selected = next(((i,r) for i,r in enumerate(patch['regions'])
                         if signed(r['key_low'],8) <= signed(key,8) <= signed(r['key_high'],8)),None)
        if selected is None:
            return None
        index, record = selected
        bank = None
        if patch['external_bank_tag'] is not None and patch['external_region_mask'] & (1 << index):
            matches = [b for b in self.banks if b.tag == patch['external_bank_tag']]
            if not matches:
                raise ValueError('External bank tag has no immutable sample metadata')
            bank = matches[-1]
        pitch_key = ((key-record['key_low']+60)&255) if record['key_tracking'] else 60
        pitch_key = signed((pitch_key + channel_pitch + record['transpose']) & 255,8)
        pitch_q8 = max(0,min(0x7f00,pitch_key << 8)) | record['fine_q8']
        if record['key_tracking']:
            pitch_q8 = (pitch_q8 + pitch_offset_q8) & 0xffff
        split = self.select_split(record['sample_id'],pitch_q8,bank)
        selector = bank.selector if bank else 14
        curve_index = record['velocity_curve'] * 128 + velocity
        curve = self.read(0xc08c6c + curve_index,1)[0]
        sensitivity = record['velocity_gain'] * 8 + (record['velocity_gain'] >> 1)
        scale = signed(0x4000 - (0x80 - curve)*sensitivity,16) >> 6
        amplitude = (record['amplitude'] * scale) & 0xffff
        rate = self.word(0xc089dc + record['hold_rate']*2)
        filter_index = min(0x7fe, max(0, ((record['filter_byte']*4 +
                       ((record['filter_byte'] &15)*velocity >>9)) &0xffff)*2))
        coefficient = (self.word(0xc08e6c+filter_index)<<4)&0xffff
        if record['sample_id'] == 0x78:
            raise ValueError('Direct sample ID78 intentionally enters infinite ROM branch c1968e')
        reverse = record['reverse'] and record['sample_id'] < 0x54
        source_split = split if record['sample_id'] < 0x54 else replace(
            split, accumulator_q9=split.accumulator_q9 & 0xfffffe00)
        region = source_split.region(selector,reverse)
        frequency = self.pitch_frequency(pitch_q8 + split.tuning_q8)
        mode = 0x43 if reverse else self.read(0xc0a734+split.mode,1)[0]
        return dict(program=program, region_index=index, record=record, split=asdict(split),
                    sample_region=region, pitch_q8=pitch_q8, frequency=frequency,
                    source_step=frequency/1024, # per native frame, not host frame
                    source_clock_hz=15238090//(16*32),
                    modern_step_numerator=frequency*(15238090//(16*32)),
                    modern_step_denominator_per_hz=1024,
                    loop=bool(mode&8), bidirectional=bool(mode&16), reverse=bool(mode&64),
                    control_low=mode, filter_k1=coefficient, filter_k2=coefficient,
                    envelope=asdict(EnvelopeState((amplitude<<16)|0x7fff,
                               -signed(rate,16), (amplitude^0x8000)<<16)),
                    release_rate=record['release_rate'], amplitude_boost=record['amplitude_boost'])

    def direct_volume(self, voice, envelope_level, channel_volume, sequence_volume,
                      pan=0x80, channel_gain=0):
        """c19526 pan lookup/c1978e volume target, in driver log units.

        pan is descriptor+6 (80 means use patch nibble); channel_gain is
        channel+14. Return encoded logarithmic gains, not linear PCM amplitude.
        Native ES5505 volume conversion is a distinct mixer contract.
        """
        record = voice['record']
        p = ((record['pan_nibble'] ^ 8) << 3) if pan == 0x80 else ((signed(pan,8)>>1)+0x40)&255
        left = self.read(0xc08b6c+((-p+127)&255),1)[0]
        right = self.read(0xc08b6c+p,1)[0]
        attenuation = self.read(0xc08bec+channel_volume,1)[0] + self.read(0xc08bec+sequence_volume,1)[0]
        attenuation += ((channel_gain+127)&255) if channel_gain else 0
        level = (signed(envelope_level,16)>>5) + (0xa8 if record['amplitude_boost'] else 0)
        def convert(pan_value):
            value = signed(((attenuation+pan_value)*4 + level -0xfa0)&0xffff,16)
            return 0xff0 if value <= 0 else min(0xfff0, value*0x30+0xff0)
        return dict(left=convert(left),right=convert(right))

    def export(self, programs):
        return dict(format='landmakr-instruments-1', sound_crc='5a7e9117',
                    boot_overlay=self.boot_overlay,
                    banks=[dict(selector=b.selector,tag=b.tag,packed=b.packed,
                                directory_address=b.directory_address,count=b.count,
                                probe_q9=b.probe_q9,metadata=b.data.hex()) for b in self.banks],
                    programs=[self.program(p) for p in programs],
                    tables=dict(rate_q15=self.table(0xc089dc,100,True),
                                lfo_phase=self.table(0xc08aa4,100),
                                pan_log=list(self.read(0xc08b6c,128)),
                                attenuation_log=list(self.read(0xc08bec,128)),
                                velocity_curves=list(self.read(0xc08c6c,512)),
                                filter=self.table(0xc08e6c,2048),
                                pitch=self.table(0xc09e6c,768),
                                voice_update_budget=list(self.read(0xc1a562,12)),
                                alternate_voice_update_budget=list(self.read(0xc1a56e,12)),
                                gain_filter_commit_budget=list(self.read(0xc1a57a,12)),
                                loop_control=list(self.read(0xc0a734,8))),
                    unsupported=['Layered synthesis runtime modulation is exported as raw operands, not ADSR defaults',
                                 'Driver rotating voice-update cadence requires runtime scheduling',
                                 'ROM split IDs6f..77 use special release-boundary behavior in layered path',
                                 'Direct sample ID78 enters ROM infinite branch c1968e',
                                 'DSP effects are not decoded by this instrument parser'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom-dir',required=True,type=Path)
    parser.add_argument('--program',action='append',type=lambda s:int(s,16),default=[])
    parser.add_argument('--note',type=int)
    parser.add_argument('--velocity',type=int,default=104)
    parser.add_argument('--output',type=Path)
    args = parser.parse_args()
    data = Instruments.load(args.rom_dir)
    programs = args.program or [0x4002]
    result = data.export(programs) if args.note is None else [data.direct_voice(p,args.note,args.velocity) for p in programs]
    text = json.dumps(result,indent=2)+'\n'
    if args.output:
        args.output.write_text(text)
    else:
        print(text,end='')


if __name__ == '__main__':
    main()
