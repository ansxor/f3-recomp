"""Taito F3 sprite list decoding from a graphics RAM dump (sprite RAM = graphics.bin 0x0000-0xffff).

Mirrors runtime/renderer/decode.cpp decode_sprite_list (the hardware state machine) without the
presentation splices: 16-byte entries, two banks of 0x400 entries (0x0000 / 0x8000), a command
entry (word 3 bit 15) that can switch banks, flip the screen and set extra planes, jumps (word 6
bit 15 -> entry w6 & 0x3ff) and the list end (a jump to itself).

Entry words: w0 tile (bit 16 in w5 bit 0); w1 zoom (low byte x, high byte y; 0 = full size);
w2/w3 x/y (12-bit signed) with scroll mode in w2 bits 12-15; w3 bit 15 command; w4 high byte
sprite control (bit 0 flip x, 1 flip y, 2 colour lock, 3 multi/continue block, 4-5 y block
control, 6-7 x block control), w4 low byte colour (bits 6-7 = priority); w6 jump.
"""
from __future__ import annotations

from dataclasses import dataclass, field


def _signed12(value: int) -> int:
    value &= 0xfff
    return value - 0x1000 if value & 0x800 else value


@dataclass
class Axis:
    block_scale: int = 1 << 8
    pos: int = 0
    block_pos: int = 0
    global_: int = 0
    subglobal: int = 0

    def update(self, scroll: int, posw: int, multi: bool, block_ctrl: int, zoom: int) -> None:
        new = _signed12(posw)
        if scroll & 1:
            self.subglobal = new
        if scroll & 2:
            self.global_ = new
        if not scroll & 8:
            new += self.global_
            if not scroll & 4:
                new += self.subglobal
        if block_ctrl == 0 and not multi:
            self.block_pos = new << 8
            self.block_scale = 0x100 - zoom
        if block_ctrl in (0, 2):
            self.pos = self.block_pos
        elif block_ctrl == 3:
            self.pos += self.block_scale * 16


@dataclass
class Entry:
    slot: int
    bank: int
    words: tuple
    tile: int
    color: int
    x: float
    y: float
    scale_x: int
    scale_y: int
    control: int
    drawn: bool
    notes: list = field(default_factory=list)

    @property
    def address(self) -> int:
        return 0x600000 + self.bank * 0x8000 + self.slot * 16


def decode(spriteram: bytes, bank: int = 0, limit: int = 0x400) -> tuple[list[Entry], dict]:
    """Walk the list like the hardware. Returns (visited entries in walk order, summary)."""
    x, y = Axis(), Axis()
    color = 0
    multi = False
    entries: list[Entry] = []
    summary = {"start_bank": bank, "end": None, "jumps": 0, "commands": 0, "drawn": 0, "visited": 0,
               "banks_read": [], "switches": [], "last_used": None}
    offs = 0
    visited = 0
    while offs < 0x400 and visited < limit:
        visited += 1
        base = bank * 0x8000 + offs * 16
        words = tuple(int.from_bytes(spriteram[base + 2 * i:base + 2 * i + 2], "big") for i in range(8))
        w0, w1, w2, w3, w4, w5, w6, _ = words
        notes = []
        this_bank = bank
        if bank not in summary["banks_read"]:
            summary["banks_read"].append(bank)
        if any(words[:7]):
            summary["last_used"] = (bank, offs)
        if w3 & 0x8000:
            summary["commands"] += 1
            if (w5 & 1) != bank:
                summary["switches"].append((bank, offs, w5 & 1))
            bank = w5 & 1
            notes.append(f"command: bank {bank}, flipscreen {int(bool(w5 & 0x2000))}, "
                         f"extra planes {(w5 >> 8) & 3}, trails {int(bool(w5 & 2))}")
        slot = offs
        end = False
        if w6 & 0x8000:
            target = w6 & 0x3ff
            if target == offs:
                notes.append("list end (jump to itself)")
                end = True
            else:
                summary["jumps"] += 1
                notes.append(f"jump to {target:#05x}")
                offs = target - 1
        control = w4 >> 8
        scroll = (w2 >> 12) & 0xf
        if not end:
            if not control & 4:
                color = w4 & 0xff
            x.update(scroll, w2, multi, (control >> 6) & 3, w1 & 0xff)
            y.update(scroll, w3, multi, (control >> 4) & 3, w1 >> 8)
            multi = bool(control & 8)
        tile = w0 | (w5 & 1) << 16
        drawn = bool(tile) and not end
        summary["drawn"] += drawn
        entries.append(Entry(slot, this_bank, words, tile, color, x.pos / 256, y.pos / 256,
                             x.block_scale, y.block_scale, control, drawn, notes))
        if end:
            summary["end"] = slot
            break
        offs += 1
    summary["visited"] = visited
    return entries, summary


def control_text(control: int) -> str:
    flags = [name for bit, name in ((1, "fx"), (2, "fy"), (4, "lock"), (8, "multi")) if control & bit]
    blocks = f"bx{(control >> 6) & 3}by{(control >> 4) & 3}"
    return ",".join(flags + [blocks])
