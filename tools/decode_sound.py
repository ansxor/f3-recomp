#!/usr/bin/env python3
"""Decode F3SND1 oracle bus traces without touching the running sound devices."""
import argparse
from collections import Counter, defaultdict, deque
import gzip
import json
from pathlib import Path
import struct

RECORD = struct.Struct("<QQIIIBB2x")
LOW_REGS = ("CR", "FC", "START_HI", "START_LO", "END_HI", "END_LO", "K2", "K1",
            "LVOL", "RVOL", "ACC_HI", "ACC_LO", "unused", "ACT", "IRQV", "PAGE")
HIGH_REGS = ("CR", "O4N1", "O3N1", "O3N2", "O2N1", "O2N2", "O1N1")


def records(path):
    with path.open("rb") as source:
        if source.read(8) != b"F3SND1\0\0":
            raise ValueError("Expected F3SND1 oracle trace")
        ended = False
        previous = 0
        while block := source.read(RECORD.size * 8192):
            if len(block) % RECORD.size:
                raise ValueError("Truncated sound trace record")
            for row in RECORD.iter_unpack(block):
                if ended:
                    raise ValueError("Data after sound trace end")
                if row[0] < previous:
                    raise ValueError("Sound trace clock moved backwards")
                previous = row[0]
                if row[5] == 5:
                    ended = True
                yield row
        if not ended:
            raise ValueError("Incomplete sound trace (no end marker)")


def words(address, value, width):
    if width == 4:
        yield address & ~1, value >> 16, 0xffff
        yield (address + 2) & ~1, value & 0xffff, 0xffff
    elif width == 2:
        yield address & ~1, value, 0xffff
    elif width == 1:
        yield address & ~1, value if address & 1 else value << 8, 0xff if address & 1 else 0xff00
    else:
        raise ValueError(f"Invalid bus width {width}")


class CommandRing:
    """Main $2fde/$3270 producer and sound $c11116 consumer, not a time heuristic."""
    def __init__(self):
        self.shared = bytearray(0x800)
        self.producer = 0
        self.next_id = 1
        self.pending = defaultdict(deque)
        self.ready = defaultdict(deque)

    def main_write(self, address, value):
        if 0xc80100 <= address <= 0xc80103:
            self.producer = 0
            self.pending.clear()
            self.ready.clear()
        if not 0xc00000 <= address < 0xc00800:
            return
        self.shared[address - 0xc00000] = value
        if address != 0xc00481:
            return
        doubled = int.from_bytes(self.shared[0x480:0x482], "big")
        if doubled & 1 or doubled >= 0x800:
            raise ValueError("Invalid committed command-ring pointer")
        end = doubled // 2
        while self.producer != end:
            size = self.shared[self.producer]
            available = (end - self.producer) & 0x3ff
            if size < 2 or size > available:
                raise ValueError("Committed command packet is incomplete")
            packet = bytes(self.shared[(self.producer + i) & 0x3ff] for i in range(size))
            command = dict(command_id=self.next_id, ring_offset=self.producer,
                           opcode=packet[1], payload=list(packet[2:]), packet=packet.hex())
            self.next_id += 1
            self.pending[self.producer].append(command)
            self.producer = (self.producer + size) & 0x3ff
            yield command

    def consume(self, address, value):
        offset = (address - 0x140000) // 2
        if not self.pending[offset]:
            raise ValueError(f"Sound consumes unpublished packet at {offset:#x}")
        command = self.pending[offset].popleft()
        if value != len(bytes.fromhex(command["packet"])):
            raise ValueError("Sound packet length differs from committed packet")
        self.ready[offset].append(command)
        return command

    def dispatch(self, address, value):
        offset = (address - 0x140000) // 2
        if not self.ready[offset]:
            raise ValueError(f"Sound dispatches unconsumed packet at {offset:#x}")
        command = self.ready[offset].popleft()
        if value != len(bytes.fromhex(command["packet"])):
            raise ValueError("Dispatched packet length differs from consumed packet")
        return command


class Decoder:
    def __init__(self):
        self.page = 0
        self.dsp_known = 0x1ff
        self.active = 31
        self.voices = [[0xf003, 0, 0, 0, 0, 0, 0, 0, 0x8000, 0x8000, 0, 0] for _ in range(32)]
        self.banks = [0] * 32
        self.dsp = bytearray(256)
        self.commands = CommandRing()
        self.ram = bytearray(0x10000)
        self.context_address = None
        self.driver = [None] * 32
        self.dispatch_command = None
        self.sequence_commands = {}
        self.note_sources = {}
        self.note_event = 0
        self.direct_command = None
        self.next_note = 1

    def ram_bytes(self, address, size):
        return bytes(self.ram[(address + i) & 0xffff] for i in range(size))

    def ram_int(self, address, size):
        return int.from_bytes(self.ram_bytes(address, size), "big")

    def voice_context(self, voice):
        at = self.context_address
        channel = self.ram_int(at + 0x10, 2)
        sample = self.ram_int(at + 0x8c, 2)
        node = self.ram_int(at + 0x0e, 2)
        sequence = (self.ram_int(channel + 0x0e, 2) - 0x5e5c) // 0x28
        source = dict(voice_state=at, channel_state=channel,
                      patch_descriptor=self.ram_int(at + 0x16, 4), sample_descriptor=sample,
                      key=self.ram[at + 4], tag5=self.ram[at + 5],
                      tag6=self.ram[at + 6], tag7=self.ram[at + 7], sequence=sequence,
                      note_node=node, origin=self.note_sources.get(node))
        self.driver[voice] = source
        return dict(voice=voice, driver=source, voice_ram=self.ram_bytes(at, 0xac).hex(),
                    channel_ram=self.ram_bytes(channel, 0x40).hex(),
                    sample_descriptor_ram=self.ram_bytes(sample, 0x20).hex())

    def snapshot(self, voice):
        r = self.voices[voice]
        bank = self.banks[voice] & 7
        start, end, accum = (r[i] << 16 | r[i+1] for i in (2, 4, 10))
        return dict(voice=voice, bank=bank, bank_word=bank << 20,
                    start_word=(bank << 20) + (start >> 9), end_word=(bank << 20) + (end >> 9),
                    accumulator_observed=accum, sample_word=(bank << 20) + (accum >> 9),
                    frequency=r[1], sample_step=r[1] / 1024, control=r[0],
                    loop=bool(r[0] & 8), bidirectional=bool(r[0] & 16), reverse=bool(r[0] & 64),
                    stopped=bool(r[0] & 3), output_pair=(r[0] >> 8) & 3,
                    left_volume=r[8] >> 8, right_volume=r[9] >> 8, k1=r[7], k2=r[6],
                    driver=self.driver[voice])

    def decode(self, row):
        tick, sample, pc, address, value, kind, width = row
        base = dict(tick=tick, seconds=tick / 16000000, sample=sample, pc=f"0x{pc:06x}")
        if kind == 6:
            self.context_address = address
            return
        if kind == 7:
            for i, byte in enumerate(value.to_bytes(width, "big")):
                self.ram[(address + i) & 0xffff] = byte
            return
        if kind == 10:
            self.note_event = address
            return
        if kind == 9:
            if self.dispatch_command is None or self.dispatch_command["opcode"] != 0x8e:
                raise ValueError("Direct note has no dispatched note-on command")
            self.direct_command = self.dispatch_command
            return
        if kind == 8:
            track, channel = value >> 16, value & 0xffff
            sequence = (self.ram_int(channel + 0x0e, 2) - 0x5e5c) // 0x28
            track_id = self.ram[(track + 3) & 0xffff]
            note = self.ram[(self.note_event + 0x14) & 0xffff]
            velocity = self.ram[(self.note_event + 0x15) & 0xffff]
            command_id = self.sequence_commands.get(sequence)
            trigger = "sequence"
            if self.direct_command:
                if self.direct_command["payload"] != [sequence, track_id, note, velocity]:
                    raise ValueError("Direct note context disagrees with mailbox operands")
                command_id = self.direct_command["command_id"]
                trigger = "mailbox"
                self.direct_command = None
            source = dict(note_id=self.next_note, note_node=address, command_id=command_id,
                          trigger=trigger, sequence=sequence, track=track_id, note=note,
                          velocity=velocity, track_state=track, channel_state=channel)
            self.next_note += 1
            self.note_sources[address] = source
            yield base | dict(event="note_source", **source)
            return
        if kind == 5:
            yield base | dict(event="end")
            return
        if kind == 4:
            self.dsp_known = 0x1ff
            self.dsp[:] = bytes(256)
            yield base | dict(event="board_reset", otis_preserved=True)
            return
        if kind == 1:
            for command in self.commands.main_write(address, value):
                yield base | dict(event="command", **command)
            yield base | dict(event="main_write", address=f"0x{address:06x}", value=value)
            return
        if kind not in (2, 3):
            raise ValueError(f"Unknown trace kind {kind}")
        if kind == 2 and pc == 0xc11116:
            yield base | dict(event="command_consumed", **self.commands.consume(address, value))
        if kind == 2 and pc in (0xc12ecc, 0xc0b414):
            command = self.commands.dispatch(address, value)
            if pc == 0xc12ecc:
                self.dispatch_command = command
                if command["opcode"] in (0x80, 0x81):
                    self.sequence_commands[command["payload"][0] & 127] = command["command_id"]
            yield base | dict(event="command_dispatch", **command)
        write = kind == 3
        for a, data, mask in words(address, value, width):
            event = base | dict(address=f"0x{a:06x}", data=data, mask=mask)
            if 0x200000 <= a < 0x200020:
                reg = (a >> 1) & 15
                voice = self.page & 31
                if reg == 15:
                    if write and mask & 0xff:
                        self.page = data & 127
                    if write and pc in (0xc17e62, 0xc17806) and self.context_address is not None:
                        yield event | dict(event="voice_context", **self.voice_context(self.page & 31))
                        self.context_address = None
                    if write:
                        yield event | dict(event="page", page=self.page)
                elif reg == 13:
                    if write and mask & 0xff:
                        self.active = data & 31
                    if write:
                        yield event | dict(event="active_voices", count=self.active + 1)
                elif self.page < 64 and (reg == 0 or (self.page < 32 and reg < 12)):
                    old = self.voices[voice][reg]
                    allowed = (0xfff, 0xfffe, 0x1fff, 0xffe0, 0x1fff, 0xffe0,
                               0xfff0, 0xfff0, 0xff00, 0xff00, 0x1fff, 0xffff)[reg]
                    effective_mask = mask & allowed
                    self.voices[voice][reg] = (old & ~effective_mask) | (data & effective_mask)
                    if reg == 0:
                        self.voices[voice][reg] |= 0xf000
                    if write:
                        snapshot = self.snapshot(voice)
                        start = reg == 0 and old & 3 and not snapshot["stopped"]
                        yield event | dict(event="voice_start" if start else "voice_write",
                                           register=LOW_REGS[reg], **snapshot)
                elif write:
                    name = HIGH_REGS[reg] if self.page < 64 and reg < len(HIGH_REGS) else LOW_REGS[reg]
                    yield event | dict(event="otis_write", page=self.page, voice=voice, register=name)
            elif 0x300000 <= a < 0x300040 and write:
                voice = (a >> 1) & 31
                self.banks[voice] = (self.banks[voice] & ~mask) | (data & mask)
                yield event | dict(event="bank", **self.snapshot(voice))
            elif 0x260000 <= a < 0x260200 and mask & 0xff:
                reg = (a >> 1) & 255
                self.dsp[reg] = data & 255
                if reg < 9:
                    self.dsp_known |= 1 << reg
                if not write:
                    continue
                if reg == 0x80:
                    # Running DSP GPRs may have changed since the last host write.
                    self.dsp_known = 0
                extra = {}
                if reg in (0xa0, 0xc0, 0xe0):
                    extra = dict(destination=data & 255)
                    if reg in (0xa0, 0xe0):
                        extra["gpr_value"] = int.from_bytes(self.dsp[0:3], "big") if self.dsp_known & 7 == 7 else None
                    if reg in (0xc0, 0xe0):
                        extra["instruction"] = (f"0x{int.from_bytes(self.dsp[3:9], 'big'):012x}"
                                                if self.dsp_known & 0x1f8 == 0x1f8 else None)
                yield event | dict(event="dsp_write", register=reg, **extra)
            elif 0x280000 <= a < 0x280020 and write and mask & 0xff:
                yield event | dict(event="duart_write", register=(a >> 1) & 15)
            elif 0x340000 <= a < 0x340004 and write and mask & 0xff00:
                yield event | dict(event="volume_write", register=((a >> 1) & 1) ^ 1, value=data >> 8)
            elif 0x140000 <= a < 0x141000 and mask & 0xff00:
                yield event | dict(event="mailbox_write" if write else "mailbox_read",
                                   offset=(a-0x140000)//2, value=data >> 8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="JSONL, gzip when ending .gz")
    parser.add_argument("--notes-only", action="store_true", help="Only voice starts, commands and reset/end events")
    parser.add_argument("--commands-only", action="store_true", help="Only submitted/consumed command packets")
    args = parser.parse_args()
    decoder = Decoder()
    counts = Counter()
    opener = gzip.open if args.output.suffix == ".gz" else open
    with opener(args.output, "wt") as output:
        for row in records(args.trace):
            for event in decoder.decode(row):
                counts[event["event"]] += 1
                selected = ("command", "command_consumed")
                if args.commands_only:
                    include = event["event"] in selected
                else:
                    include = not args.notes_only or event["event"] in selected + ("voice_start", "board_reset", "end")
                if include:
                    output.write(json.dumps(event, separators=(",", ":")) + "\n")
    print(json.dumps(dict(counts), sort_keys=True))


if __name__ == "__main__":
    main()
