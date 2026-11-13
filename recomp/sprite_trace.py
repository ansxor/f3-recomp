"""Static trace from sprite X/Y writers to object-record root fields.

The tracer answers two questions for a game image, without per-game
configuration:

* which instruction stores a sprite X and/or Y word, which register holds the
  sprite-RAM cursor at that store, and which object-record field the position
  came from (``base register + field offset``);
* whether every writer of such a field *continues* the field (``pos += vel``)
  or *resets* it (constant, table, another field), so the runtime knows when a
  group is born.

Method
------

Instructions come from :class:`recomp.discovery.Discovery` (the same table the
recompiler emits).  A forward abstract interpretation computes, per instruction:

* a symbolic value per 16-bit register lane 32-bit value, where the interesting
  atom is a *root leaf* ``RootField(base, offset)`` produced by reading a fixed
  displacement from a register that holds an object/queue record;
* two flavours of root leaf: ``direct`` (an additive anchor that survived only
  moves, adds/subs and constant masks) and ``roots`` (all leaves, including the
  ones folded into offsets through multiply/shift/index arithmetic);
* whether a register holds a sprite-RAM cursor and, if so, its offset modulo 16
  inside the 16-byte sprite entry.

A lane's root field is the unique ``direct`` leaf at the store: the seed of the
position accumulator.  Leaves that were transformed (zoom, size, descriptor) are
tracked as offsets, which is exactly the rigidity definition in the handoff.
Several distinct direct leaves, or a mixture of root and non-root paths, is
ambiguous and stays unresolved: nothing is guessed.

Writer discovery is automatic.  Cursor provenance comes from a whole-image
constant-store summary: a register loaded from a RAM address (absolute or
``d(a5)``) whose constant stores land in sprite RAM is a sprite cursor, and a
register set by ``lea``/``move.l #`` with a sprite-RAM constant is one too.
Indirect dispatch (``jmp ([$8,a3])``) resolves through the code pointers stored
at that record displacement, and ``jmp (a4)`` through the constant return
address.  ``[[video.emit_units]]`` is never consulted.

Continuity classifies every decoded store to a table root field, including
writers in code that static reachability cannot enter (object code reached by
a pointer slot), because a missed spawn reset would break identity.  A PC that
never executes costs one unused table entry; a reset that executes is a birth.
The rule for a continuation is: a read-modify-write of the field, a value that
traces back to the field itself, or a pure function of *other fields of the same
record* (a per-frame projection such as `pos = anchor - zoom_offset` computed
on the object it belongs to).  A constant, an unproven source, a ROM table or a
value from a different record replaces the position, so it is a reset/birth.

``--sprite-writers`` is an optional extra seed: an explicit PC may carry a lane
hint for runtime discovery logs whose store is mid-entry, where the cursor
offset is not statically known.

Everything is deterministic: worklists and output are PC sorted.
"""

from __future__ import annotations

import json
from collections.abc import Iterable, Mapping
from dataclasses import dataclass, replace
from pathlib import Path

from capstone.m68k import (
    M68K_AM_ABSOLUTE_DATA_LONG,
    M68K_AM_ABSOLUTE_DATA_SHORT,
    M68K_AM_AREGI_INDEX_8_BIT_DISP,
    M68K_AM_AREGI_INDEX_BASE_DISP,
    M68K_AM_MEMI_POST_INDEX,
    M68K_AM_MEMI_PRE_INDEX,
    M68K_AM_PCI_DISP,
    M68K_AM_REGI_ADDR,
    M68K_AM_REGI_ADDR_DISP,
    M68K_AM_REGI_ADDR_POST_INC,
    M68K_AM_REGI_ADDR_PRE_DEC,
    M68K_OP_IMM,
    M68K_OP_MEM,
    M68K_OP_REG,
    M68K_OP_REG_PAIR,
)

from .discovery import CALL_MNEMONICS, COND_BRANCH_MNEMONICS, TERMINAL_MNEMONICS, UNCOND_BRANCH_MNEMONICS

# Register encoding matches f3rt::UnitRegister: 0..7 = d0..d7, 8..14 = a0..a6.
REG_COUNT = 16
SP = 15
A5 = 13

# F3 sprite RAM: command list plus two 8 KiB banks.
SPRITE_RAM_START = 0x600000
SPRITE_RAM_END = 0x610000
RAM_START = 0x400000
RAM_END = 0x600000
# a5 is the game work-RAM page pointer; its value is recovered from reset.
WORK_BASE_MIN = 0x400000
WORK_BASE_MAX = 0x420000

# Root delta to 24.8 scanout units: these roots hold pixel coordinates.
DEFAULT_SHIFT = 8

# 16-byte sprite entry lanes: +0 tile, +2 zoom, +4 X, +6 Y, +8 attr.
LANE_X = 1
LANE_Y = 2
LANE_XY = 3
ENTRY_WORDS = {0: "tile", 2: "zoom", 4: "X", 6: "Y", 8: "attr"}

# Mnemonics whose last operand may be written in memory.
_STORE_BASE = frozenset(
    {
        "move",
        "clr",
        "not",
        "neg",
        "negx",
        "tas",
        "st",
        "sf",
        "scc",
        "add",
        "adda",
        "addi",
        "addq",
        "sub",
        "suba",
        "subi",
        "subq",
        "and",
        "andi",
        "or",
        "ori",
        "eor",
        "eori",
        "lsl",
        "lsr",
        "asl",
        "asr",
        "rol",
        "ror",
        "roxl",
        "roxr",
        "bset",
        "bclr",
        "bchg",
        "addx",
        "subx",
        "abcd",
        "sbcd",
        "movep",
    }
)
# Read-modify-write forms: the stored value is derived from the old memory value.
_RMW_BASE = frozenset(
    {
        "add",
        "adda",
        "addi",
        "addq",
        "sub",
        "suba",
        "subi",
        "subq",
        "and",
        "andi",
        "or",
        "ori",
        "eor",
        "eori",
        "lsl",
        "lsr",
        "asl",
        "asr",
        "rol",
        "ror",
        "roxl",
        "roxr",
        "bset",
        "bclr",
        "bchg",
        "not",
        "neg",
        "negx",
        "addx",
        "subx",
    }
)
_ARITH = frozenset({"add", "adda", "addi", "addq", "sub", "suba", "subi", "subq"})
_LOGIC_IMM = frozenset({"and", "andi", "or", "ori", "eor", "eori"})
_LOGIC_REG = frozenset({"and", "or", "eor"})
_SHIFT = frozenset({"lsl", "lsr", "asl", "asr", "rol", "ror", "roxl", "roxr"})
_MUL = frozenset({"mulu", "muls", "divu", "divs"})
_NO_WRITE = frozenset({"tst", "cmp", "cmpa", "cmpi", "cmpm", "btst"})
_CONTROL = TERMINAL_MNEMONICS | COND_BRANCH_MNEMONICS | CALL_MNEMONICS | UNCOND_BRANCH_MNEMONICS | {"trap", "nop"}


def _reg(cs_reg: int) -> int | None:
    """Map a Capstone register id to the f3rt::UnitRegister encoding."""
    index = cs_reg - 1
    return index if 0 <= index < REG_COUNT else None


def _size(insn) -> int:
    mnemonic = insn.mnemonic
    suffix = mnemonic[-2:] if "." in mnemonic else ""
    if suffix == ".b":
        return 1
    if suffix == ".w":
        return 2
    return 4


def _addr_tag(address: int, rom_size: int) -> str:
    if 0 <= address < rom_size:
        return "rom"
    if SPRITE_RAM_START <= address < SPRITE_RAM_END:
        return "sprite"
    if RAM_START <= address < RAM_END:
        return "ram"
    return "io"


@dataclass(frozen=True, order=True)
class RootField:
    """A record field: offset read from the register holding the record."""

    base: int
    offset: int

    @property
    def base_name(self) -> str:
        return register_name(self.base)


def register_name(index: int) -> str:
    if 0 <= index < 8:
        return f"d{index}"
    if 8 <= index < 15:
        return f"a{index - 8}"
    return "sp"


@dataclass(frozen=True)
class Word:
    """A 16-bit register lane.

    ``direct`` are the additive anchor leaves, ``roots`` every leaf that
    contributed (direct or folded into an offset), ``exact`` says whether
    ``direct`` fully describes the provenance on every path.
    """

    direct: frozenset[RootField] = frozenset()
    exact: bool = True
    roots: frozenset[RootField] = frozenset()
    const: int | None = None
    tags: frozenset[str] = frozenset()
    # Set by arithmetic/logic/scale, cleared by plain copies.  Distinguishes a
    # same-record projection (`anchor - zoom_offset`) from a plain field copy
    # (`pos = other_field`), which is a spawn/teleport shape.
    transformed: bool = False


@dataclass(frozen=True)
class Value:
    """A 32-bit register value, split into word lanes.

    ``cursor``/``offsets`` describe a register proven to hold a sprite-RAM
    write cursor; ``offsets`` holds the possible offsets modulo 16.
    """

    hi: Word = Word()
    lo: Word = Word()
    cursor: bool = False
    offsets: frozenset[int] = frozenset()
    # Provenance class of the 32-bit value: "param"/"mem"/"stream"/"rom"/
    # "ram"/"const"/"value"/"other".  Root bases are pointer classes other
    # than a constant, a cursor, the stack or a ROM pointer.
    kind: str = "unknown"

    @staticmethod
    def unknown(kind: str = "unknown") -> Value:
        word = Word(exact=False, tags=frozenset({kind}))
        return Value(hi=word, lo=word, kind=kind)

    @staticmethod
    def constant(value: int, rom_size: int) -> Value:
        value &= 0xFFFFFFFF
        tag = frozenset({_addr_tag(value, rom_size)})
        return Value(
            hi=Word(const=(value >> 16) & 0xFFFF, tags=tag),
            lo=Word(const=value & 0xFFFF, tags=tag),
            kind=_addr_tag(value, rom_size),
        )

    def const32(self) -> int | None:
        if self.hi.const is None or self.lo.const is None:
            return None
        return (self.hi.const << 16) | self.lo.const

    def all_roots(self) -> frozenset[RootField]:
        return self.hi.roots | self.lo.roots


UNKNOWN = Value.unknown("unknown")


def _merge_word(a: Word, b: Word) -> Word:
    if a == b:
        return a
    return Word(
        direct=a.direct | b.direct,
        exact=a.exact and b.exact and a.direct == b.direct,
        roots=a.roots | b.roots,
        const=a.const if a.const == b.const else None,
        tags=a.tags | b.tags,
        transformed=a.transformed or b.transformed,
    )


_POINTER_KINDS = frozenset({"param", "mem", "derived", "stream", "ram", "other", "io", "rom"})


def join_kind(a: str, b: str) -> str:
    if a == b:
        return a
    if a == "unknown":
        return b
    if b == "unknown":
        return a
    return "other"


def merge_value(a: Value, b: Value) -> Value:
    if a == b:
        return a
    return Value(
        hi=_merge_word(a.hi, b.hi),
        lo=_merge_word(a.lo, b.lo),
        cursor=a.cursor or b.cursor,
        offsets=a.offsets | b.offsets,
        kind=join_kind(a.kind, b.kind),
    )


State = tuple[Value, ...]


def merge_state(a: State, b: State) -> State:
    return tuple(merge_value(x, y) for x, y in zip(a, b, strict=True))


def _add_word(a: Word, b: Word, subtract: bool = False) -> Word:
    direct = a.direct | b.direct
    exact = a.exact and b.exact and not (a.direct and b.direct and a.direct != b.direct)
    const = None
    if a.const is not None and b.const is not None:
        const = (a.const - b.const if subtract else a.const + b.const) & 0xFFFF
    return Word(
        direct=direct,
        exact=exact,
        roots=a.roots | b.roots,
        const=const,
        tags=a.tags | b.tags,
        transformed=True,
    )


def _logic_const(base: str, value: int, immediate: int) -> int:
    if base in ("and", "andi"):
        return value & immediate
    if base in ("or", "ori"):
        return value | immediate
    return value ^ immediate


def _derive_word(a: Word, b: Word | None = None) -> Word:
    """A data-dependent transform keeps provenance but drops the anchor."""
    other = b or Word()
    return Word(
        direct=frozenset(),
        exact=a.exact and other.exact,
        roots=a.roots | other.roots | a.direct | other.direct,
        const=None,
        tags=(a.tags | other.tags) | {"derived"},
        transformed=True,
    )


@dataclass(frozen=True)
class Writer:
    pc: int
    cursor_reg: int
    base: int
    lane: int
    x_offset: int
    y_offset: int
    group_class: int
    rigid: bool
    root_offsets: tuple[RootField, ...]


@dataclass(frozen=True)
class UnresolvedWriter:
    """A sprite X/Y store that was found but whose root field is not proven."""

    pc: int
    cursor_reg: int
    base_reg: int
    lane: int
    destination: str
    reason: str


@dataclass(frozen=True)
class ResetWriter:
    pc: int
    base: int
    offset: int
    instruction: str
    evidence: str


@dataclass(frozen=True)
class Classification:
    pc: int
    base: int
    offset: int
    kind: str
    instruction: str
    evidence: str


@dataclass(frozen=True)
class TraceResult:
    game_id: str
    writers: tuple[Writer, ...]
    resets: tuple[ResetWriter, ...]
    classifications: tuple[Classification, ...]
    stats: Mapping[str, int]
    unresolved: tuple[UnresolvedWriter, ...] = ()

    def to_json(self) -> dict:
        return {
            "game": self.game_id,
            "summary": dict(self.stats),
            "writes": [
                {
                    "pc": f"0x{writer.pc:06x}",
                    "cursor_register": register_name(writer.cursor_reg),
                    "base": writer.base,
                    "base_register": register_name(writer.base),
                    "lane": writer.lane,
                    "lane_names": lane_names(writer.lane),
                    "group_class": writer.group_class,
                    "shift": DEFAULT_SHIFT,
                    "x_offset": writer.x_offset,
                    "y_offset": writer.y_offset,
                    "rigid": writer.rigid,
                    "root_fields": [f"{f.base_name}+0x{f.offset:02x}" for f in writer.root_offsets],
                }
                for writer in self.writers
            ],
            "unresolved": [
                {
                    "pc": f"0x{writer.pc:06x}",
                    "cursor_register": register_name(writer.cursor_reg),
                    "base_register": register_name(writer.base_reg),
                    "lane": writer.lane,
                    "destination": writer.destination,
                    "reason": writer.reason,
                }
                for writer in self.unresolved
            ],
            "resets": [
                {
                    "pc": f"0x{reset.pc:06x}",
                    "base": reset.base,
                    "base_register": register_name(reset.base),
                    "offset": reset.offset,
                    "instruction": reset.instruction,
                    "evidence": reset.evidence,
                }
                for reset in self.resets
            ],
            "classifications": [
                {
                    "pc": f"0x{item.pc:06x}",
                    "base": item.base,
                    "base_register": register_name(item.base),
                    "offset": item.offset,
                    "kind": item.kind,
                    "instruction": item.instruction,
                    "evidence": item.evidence,
                }
                for item in self.classifications
            ],
        }


def _unresolved_reason(word: Word) -> str:
    if len(word.direct) > 1:
        return "ambiguous root fields " + _fields_text(word.direct)
    if not word.exact:
        return "unproven value (merged or unknown provenance)"
    if "stream" in word.tags:
        return "value read from a stream cursor (no record field)"
    if word.const is not None:
        return "constant value"
    if word.roots:
        return "no direct root among " + _fields_text(word.roots)
    return "value has no record field"


def _fields_text(fields: Iterable[RootField]) -> str:
    text = ", ".join(f"{field.base_name}+0x{field.offset:02x}" for field in sorted(fields))
    return text or "none"


def lane_names(lane: int) -> list[str]:
    return [name for bit, name in ((LANE_X, "X"), (LANE_Y, "Y")) if lane & bit]


# ---------------------------------------------------------------------------
# Static reachability and dispatch targets


@dataclass(frozen=True)
class _Info:
    insn: object
    base: str
    size: int
    length: int
    operands: tuple


def _handler_table(instructions: Mapping[int, object]) -> dict[int, set[int]]:
    """Code pointers stored at small record displacements.

    ``jmp ([$8,a3])`` selects the handler from the object's own +8 slot; the
    slot is filled at runtime, but every value written into it is a constant
    in the image.  Mapping displacement -> stored code pointers resolves the
    dispatch without guessing which object is live.
    """
    table: dict[int, set[int]] = {}
    for pc in sorted(instructions):
        info = _info(instructions[pc])
        if info.base != "move" or info.size != 4 or len(info.operands) != 2:
            continue
        src, dst = info.operands
        if src.type != M68K_OP_IMM or dst.type != M68K_OP_MEM:
            continue
        if dst.address_mode != M68K_AM_REGI_ADDR_DISP or dst.mem.index_reg:
            continue
        target = src.imm & 0xFFFFFFFF
        if target % 2 or target not in instructions:
            continue
        table.setdefault(dst.mem.disp, set()).add(target)
    return table


def _info(insn) -> _Info:
    base = insn.mnemonic.split(".")[0].lower()
    return _Info(insn=insn, base=base, size=_size(insn), length=insn.size, operands=tuple(insn.operands))


class SpriteTrace:
    """Whole-image sprite X/Y writer trace for one decoded program image."""

    def __init__(
        self,
        rom: bytes,
        discovery,
        config: Mapping | None = None,
        writer_seeds: Iterable[int] = (),
        writer_lanes: Mapping[int, int] | None = None,
    ) -> None:
        self.rom = rom
        self.rom_size = len(rom)
        self.instructions: Mapping[int, object] = getattr(discovery, "instructions", {}) or {}
        self.functions = set(getattr(discovery, "functions", ()) or ())
        self.branch_targets: Mapping[int, list[int]] = getattr(discovery, "branch_targets", {}) or {}
        self.indirect_targets: Mapping[int, list[int]] = getattr(discovery, "indirect_targets", {}) or {}
        self.game_id = str((config or {}).get("game", {}).get("id", "unknown"))
        self.extra_writers = sorted({int(pc) & 0xFFFFFF for pc in writer_seeds})
        # Explicit --sprite-writers hints are an additional writer seed: the
        # lane is trusted when the cursor proof cannot see the store's entry
        # offset (runtime discovery logs mid-entry, for example).
        self.writer_lanes = {
            int(pc) & 0xFFFFFF: lane for pc, lane in (writer_lanes or {}).items() if lane in (LANE_X, LANE_Y, LANE_XY)
        }
        self.handlers = _handler_table(self.instructions)
        self.work_bases: tuple[int, ...] = ()
        self.ram_constants: dict[int, frozenset[int]] = {}
        self.entry_state: dict[int, State] = {}
        self._succ: dict[int, tuple[int, ...]] = {}
        self._infos: dict[int, _Info] = {}

    def info(self, pc: int) -> _Info:
        info = self._infos.get(pc)
        if info is None:
            info = _info(self.instructions[pc])
            self._infos[pc] = info
        return info

    # -- constant-store summary -------------------------------------------------

    def _find_work_bases(self) -> None:
        candidates: set[int] = set()
        for pc in sorted(self.instructions):
            instruction = self.instructions[pc]
            info = _info(instruction)
            if info.base not in ("lea", "move", "movea") or len(info.operands) < 2:
                continue
            src, dst = info.operands[0], info.operands[-1]
            if dst.type != M68K_OP_REG or _reg(dst.reg) != A5:
                continue
            value = self._static_ea(instruction, src)
            if value is not None and WORK_BASE_MIN <= value < WORK_BASE_MAX:
                candidates.add(value)
        self.work_bases = tuple(sorted(candidates))

    def _find_ram_constants(self) -> None:
        """Constants stored to absolute or a5-relative RAM addresses."""
        summary: dict[int, set[int]] = {}
        bases = self.work_bases
        for pc in sorted(self.instructions):
            info = _info(self.instructions[pc])
            if info.base not in ("move", "clr") or len(info.operands) < 2:
                continue
            dst = info.operands[-1]
            if dst.type != M68K_OP_MEM or dst.mem.index_reg:
                continue
            if dst.address_mode not in (
                M68K_AM_ABSOLUTE_DATA_LONG,
                M68K_AM_ABSOLUTE_DATA_SHORT,
                M68K_AM_REGI_ADDR,
                M68K_AM_REGI_ADDR_DISP,
            ):
                continue
            addresses: list[int]
            if dst.address_mode in (M68K_AM_ABSOLUTE_DATA_LONG, M68K_AM_ABSOLUTE_DATA_SHORT):
                addresses = [dst.imm & 0xFFFFFFFF]
            else:
                base = self._mem_base(dst)
                if base != A5:
                    continue
                addresses = [(work + dst.mem.disp) & 0xFFFFFFFF for work in bases]
            src = info.operands[0]
            if info.base == "clr":
                value = 0
            elif src.type == M68K_OP_IMM:
                value = src.imm & 0xFFFFFFFF
            else:
                continue
            if info.size != 4:
                value &= (1 << (info.size * 8)) - 1
            for address in addresses:
                if not (RAM_START <= address < RAM_END or SPRITE_RAM_START <= address < SPRITE_RAM_END):
                    continue
                summary.setdefault(address, set()).add(value)
        self.ram_constants = {address: frozenset(values) for address, values in summary.items()}

    # -- values -----------------------------------------------------------------

    def _entry_default(self) -> State:
        regs = [Value.unknown("param") for _ in range(REG_COUNT)]
        regs[SP] = Value.unknown("stack")
        if self.work_bases:
            regs[A5] = self._const_union(self.work_bases)
        return tuple(regs)

    def _const_union(self, values: Iterable[int]) -> Value:
        values = tuple(sorted({value & 0xFFFFFFFF for value in values}))
        first = Value.constant(values[0], self.rom_size)
        for value in values[1:]:
            first = merge_value(first, Value.constant(value, self.rom_size))
        return first

    def _is_root_base(self, value: Value, register: int) -> bool:
        if register in (SP, A5):
            return False
        if value.cursor:
            return False
        if value.const32() is not None:
            return False
        return value.kind not in ("rom", "stack")

    def _ea_constant(self, insn, op, state: State) -> int | None:
        if op.type != M68K_OP_MEM:
            return None
        mode = op.address_mode
        mem = op.mem
        if mem.index_reg:
            return None
        if mode in (M68K_AM_ABSOLUTE_DATA_LONG, M68K_AM_ABSOLUTE_DATA_SHORT):
            return op.imm & 0xFFFFFFFF
        if mode == M68K_AM_PCI_DISP:
            return (insn.address + 2 + mem.disp) & 0xFFFFFFFF
        if mode in (M68K_AM_REGI_ADDR, M68K_AM_REGI_ADDR_DISP):
            base = self._mem_base(op)
            if base is None:
                return None
            address = state[base].const32()
            if address is None:
                return None
            return (address + mem.disp) & 0xFFFFFFFF
        return None

    def _cursor_from_address(self, address: int | None) -> tuple[bool, frozenset[int]]:
        if address is None or not (SPRITE_RAM_START <= address < SPRITE_RAM_END):
            return False, frozenset()
        return True, frozenset({address & 15})

    def _static_ea(self, insn, op) -> int | None:
        """Absolute or PC-relative address with no register state."""
        if op.type != M68K_OP_MEM or op.mem.index_reg:
            return None
        if op.address_mode in (M68K_AM_ABSOLUTE_DATA_LONG, M68K_AM_ABSOLUTE_DATA_SHORT):
            # Capstone reports absolute addresses through the immediate field.
            return op.imm & 0xFFFFFFFF
        if op.address_mode == M68K_AM_PCI_DISP:
            return (insn.address + 2 + op.mem.disp) & 0xFFFFFFFF
        return None

    @staticmethod
    def _mem_base(op) -> int | None:
        if op.type != M68K_OP_MEM:
            return None
        if op.address_mode in (
            M68K_AM_REGI_ADDR,
            M68K_AM_REGI_ADDR_POST_INC,
            M68K_AM_REGI_ADDR_PRE_DEC,
        ):
            # These modes carry the base in the operand register.
            return _reg(op.reg) if op.reg else None
        return _reg(op.mem.base_reg) if op.mem.base_reg else None

    def _read_mem(self, insn, op, size: int, state: State) -> Value:
        mode = op.address_mode
        mem = op.mem
        disp = mem.disp
        base = self._mem_base(op)
        if mode in (M68K_AM_REGI_ADDR_POST_INC, M68K_AM_REGI_ADDR_PRE_DEC):
            # A stream cursor: pointer provenance, never a record field.
            # A stream read is a loop-carried cursor: it never contributes a
            # record field, even when the pointer itself came from one.
            tags = frozenset({"stream"})
            if base is not None:
                base_value = state[base]
                tags |= base_value.hi.tags | base_value.lo.tags
            word = Word(exact=True, tags=tags)
            return Value(hi=word, lo=word, kind="stream")
        if mode in (M68K_AM_AREGI_INDEX_8_BIT_DISP, M68K_AM_AREGI_INDEX_BASE_DISP):
            roots = frozenset()
            tags = frozenset({"index"})
            if base is not None:
                base_value = state[base]
                roots = base_value.all_roots() | base_value.hi.direct | base_value.lo.direct
                tags |= base_value.hi.tags | base_value.lo.tags
            word = Word(exact=True, roots=roots, tags=tags)
            return Value(hi=word, lo=word, kind="index")
        if mode in (M68K_AM_MEMI_PRE_INDEX, M68K_AM_MEMI_POST_INDEX):
            word = Word(exact=True, tags=frozenset({"stream"}))
            return Value(hi=word, lo=word, kind="stream")
        if mode in (M68K_AM_ABSOLUTE_DATA_LONG, M68K_AM_ABSOLUTE_DATA_SHORT, M68K_AM_PCI_DISP):
            address = self._ea_constant(insn, op, state)
            tag = _addr_tag(address, self.rom_size) if address is not None else "mem"
            word = Word(exact=True, tags=frozenset({tag}))
            return Value(hi=word, lo=word, kind=tag)
        if base is not None:
            base_value = state[base]
            if self._is_root_base(base_value, base):
                if size == 4:
                    high = Word(direct=frozenset({RootField(base, disp)}), roots=frozenset({RootField(base, disp)}))
                    low = Word(
                        direct=frozenset({RootField(base, disp + 2)}),
                        roots=frozenset({RootField(base, disp + 2)}),
                    )
                    return Value(hi=high, lo=low, kind="mem")
                root = RootField(base, disp)
                return Value(lo=Word(direct=frozenset({root}), roots=frozenset({root})), kind="mem")
            roots = base_value.all_roots() | base_value.hi.direct | base_value.lo.direct
            tags = (base_value.hi.tags | base_value.lo.tags) | {"derived"}
            # A pointer read through a ROM/RAM table base stays in that class.
            kind = base_value.kind if base_value.kind in ("rom", "ram") else "derived"
            word = Word(exact=True, roots=roots, tags=tags)
            return Value(hi=word, lo=word, kind=kind)
        word = Word(exact=True, tags=frozenset({"mem"}))
        return Value(hi=word, lo=word, kind="mem")

    def _operand_value(self, insn, op, size: int, state: State) -> Value:
        if op.type == M68K_OP_REG:
            register = _reg(op.reg)
            return state[register] if register is not None else UNKNOWN
        if op.type == M68K_OP_IMM:
            raw = op.imm & 0xFFFFFFFF
            if size == 1:
                return Value(lo=Word(const=raw & 0xFF))
            if size == 2:
                return Value(lo=Word(const=raw & 0xFFFF))
            return Value.constant(raw, self.rom_size)
        if op.type == M68K_OP_MEM:
            return self._read_mem(insn, op, size, state)
        return UNKNOWN

    def _bump(self, value: Value, delta: int) -> Value:
        if value.cursor:
            offsets = frozenset((offset + delta) & 15 for offset in value.offsets)
            return replace(value, offsets=offsets)
        return value

    def _sign_extend(self, word: Word) -> Word:
        if word.const is None:
            return Word(exact=word.exact, tags=word.tags | {"derived"}, transformed=True)
        value = word.const & 0xFFFF
        if value & 0x8000:
            value -= 0x10000
        return Word(const=value & 0xFFFF, tags=word.tags, transformed=True)

    # -- transfer ---------------------------------------------------------------

    def transfer(self, pc: int, state: State) -> State:
        insn = self.instructions[pc]
        info = self.info(pc)
        base = info.base
        size = info.size
        operands = info.operands
        regs = list(state)

        for op in operands:
            if op.type == M68K_OP_MEM and op.address_mode == M68K_AM_REGI_ADDR_PRE_DEC:
                register = _reg(op.reg)
                if register is not None:
                    regs[register] = self._bump(regs[register], -size)

        if base in _CONTROL:
            if base in ("dbra", "dbf") or base.startswith("db"):
                register = _reg(operands[0].reg) if operands and operands[0].type == M68K_OP_REG else None
                if register is not None:
                    low = regs[register].lo
                    const = None if low.const is None else (low.const - 1) & 0xFFFF
                    regs[register] = replace(regs[register], lo=replace(low, const=const))
            return tuple(regs)

        destination_register: int | None = None
        if base in ("link",):
            register = _reg(operands[0].reg) if operands else None
            if register is not None:
                regs[register] = Value.unknown("stack")
        elif base in ("unlk",):
            if operands:
                register = _reg(operands[0].reg)
                if register is not None:
                    regs[SP] = regs[register]
        elif base == "movem":
            destination = operands[-1] if operands else None
            if destination is not None and destination.type == M68K_OP_MEM:
                pass  # registers are stored
            else:
                for op in operands[:-1]:
                    if op.type in (M68K_OP_REG, M68K_OP_REG_PAIR):
                        register = _reg(op.reg)
                        if register is not None:
                            regs[register] = Value.unknown("stack")
        elif base == "exg":
            if len(operands) == 2 and operands[0].type == M68K_OP_REG and operands[1].type == M68K_OP_REG:
                first, second = _reg(operands[0].reg), _reg(operands[1].reg)
                if first is not None and second is not None:
                    regs[first], regs[second] = regs[second], regs[first]
        elif base == "swap":
            register = _reg(operands[0].reg) if operands else None
            if register is not None:
                value = regs[register]
                regs[register] = Value(
                    hi=value.lo, lo=value.hi, cursor=value.cursor, offsets=value.offsets, kind=value.kind
                )
        elif base in ("ext", "extb"):
            register = _reg(operands[0].reg) if operands else None
            if register is not None:
                value = regs[register]
                regs[register] = Value(
                    hi=self._sign_extend(value.lo),
                    lo=value.lo,
                    cursor=False,
                    offsets=frozenset(),
                    kind="value",
                )
        elif base == "clr":
            target = operands[0] if operands else None
            if target is not None and target.type == M68K_OP_REG:
                register = _reg(target.reg)
                if register is not None:
                    if size == 4:
                        regs[register] = Value(hi=Word(const=0), lo=Word(const=0), kind="const")
                    else:
                        regs[register] = Value(hi=regs[register].hi, lo=Word(const=0), kind="const")
        elif base in ("move", "movea", "moveq", "lea"):
            self._transfer_move(insn, info, regs, state)
            destination_register = self._written_register(operands)
        elif base in ("add", "adda", "addi", "addq", "sub", "suba", "subi", "subq"):
            self._transfer_arith(insn, info, regs, state)
            destination_register = self._written_register(operands)
        elif base in _LOGIC_IMM | _LOGIC_REG:
            self._transfer_logic(insn, info, regs, state)
            destination_register = self._written_register(operands)
        elif base in _SHIFT or base in _MUL:
            self._transfer_data(insn, info, regs, state)
            destination_register = self._written_register(operands)
        elif base in ("not", "neg", "negx"):
            target = operands[0] if operands else None
            if target is not None and target.type == M68K_OP_REG:
                register = _reg(target.reg)
                if register is not None:
                    value = regs[register]
                    if base == "not":
                        word_hi = _derive_word(value.hi)
                        word_lo = _derive_word(value.lo)
                    else:
                        word_hi = replace(value.hi, transformed=True)
                        word_lo = replace(value.lo, transformed=True)
                        if value.lo.const is not None:
                            word_lo = replace(word_lo, const=(-value.lo.const) & 0xFFFF)
                    regs[register] = Value(hi=word_hi, lo=word_lo, kind="value")
        elif base in ("bset", "bclr", "bchg"):
            target = operands[-1] if operands else None
            if target is not None and target.type == M68K_OP_REG:
                register = _reg(target.reg)
                if register is not None:
                    value = regs[register]
                    regs[register] = Value(hi=_derive_word(value.hi), lo=_derive_word(value.lo), kind="value")
        elif base == "movep":
            # Destination is always memory in the forms seen; nothing to model.
            pass
        elif base in _NO_WRITE:
            pass
        else:
            # Unknown instruction: clobber every register it writes so stale
            # values never masquerade as provenance.
            for op in operands:
                if op.type == M68K_OP_REG:
                    register = _reg(op.reg)
                    if register is not None and op is operands[-1]:
                        regs[register] = UNKNOWN
            try:
                _, written = insn.regs_access()
            except Exception:  # pragma: no cover - capstone always supports this
                written = []
            for cs_reg in written:
                register = _reg(cs_reg)
                if register is not None:
                    regs[register] = UNKNOWN

        # `move.w` into a register: the cursor property never survives a
        # partial write, only the full 32-bit forms.
        if destination_register is not None and base in ("move", "movea", "moveq", "lea") and size != 4:
            regs[destination_register] = replace(regs[destination_register], cursor=False, offsets=frozenset())

        for op in operands:
            if op.type == M68K_OP_MEM and op.address_mode == M68K_AM_REGI_ADDR_POST_INC:
                register = _reg(op.reg)
                if register is not None:
                    regs[register] = self._bump(regs[register], size)
        return tuple(regs)

    @staticmethod
    def _written_register(operands) -> int | None:
        if not operands:
            return None
        destination = operands[-1]
        if destination.type == M68K_OP_REG:
            return _reg(destination.reg)
        return None

    def _transfer_move(self, insn, info: _Info, regs: list[Value], state: State) -> None:
        base, size, operands = info.base, info.size, info.operands
        if len(operands) < 2:
            return
        src, dst = operands[0], operands[-1]
        if base == "moveq":
            register = _reg(dst.reg) if dst.type == M68K_OP_REG else None
            if register is not None:
                # MOVEQ sign-extends the byte.
                imm = src.imm & 0xFF
                if imm & 0x80:
                    imm -= 0x100
                regs[register] = Value.constant(imm & 0xFFFFFFFF, self.rom_size)
            return
        if base == "lea":
            address = self._ea_constant(insn, src, state)
            register = _reg(dst.reg) if dst.type == M68K_OP_REG else None
            if register is None:
                return
            if address is None:
                regs[register] = Value.unknown("addr")
                return
            cursor, offsets = self._cursor_from_address(address)
            value = Value.constant(address, self.rom_size)
            regs[register] = replace(value, cursor=cursor, offsets=offsets)
            return
        value = self._operand_value(insn, src, size, state)
        register = _reg(dst.reg) if dst.type == M68K_OP_REG else None
        if register is None:
            return
        if base == "movea" and size == 2:
            regs[register] = Value(hi=self._sign_extend(value.lo), lo=value.lo, kind="other")
            return
        cursor = False
        offsets: frozenset[int] = frozenset()
        if size == 4:
            if src.type == M68K_OP_REG:
                source_register = _reg(src.reg)
                if source_register is not None:
                    cursor = regs[source_register].cursor
                    offsets = regs[source_register].offsets
            elif src.type == M68K_OP_MEM:
                address = self._ea_constant(insn, src, state)
                if address is not None and src.address_mode in (
                    M68K_AM_REGI_ADDR,
                    M68K_AM_REGI_ADDR_DISP,
                    M68K_AM_ABSOLUTE_DATA_LONG,
                    M68K_AM_ABSOLUTE_DATA_SHORT,
                ):
                    constants = self.ram_constants.get(address)
                    if constants:
                        sprite = frozenset(v for v in constants if SPRITE_RAM_START <= v < SPRITE_RAM_END)
                        other = frozenset(v for v in constants if not SPRITE_RAM_START <= v < SPRITE_RAM_END)
                        if sprite and not other:
                            offsets = frozenset(value & 15 for value in sprite)
                            cursor = True
                else:
                    cursor, offsets = self._cursor_from_address(address)
            regs[register] = replace(value, cursor=cursor, offsets=offsets)
            return
        if size == 2:
            regs[register] = Value(hi=regs[register].hi, lo=value.lo, kind="other")
        else:
            regs[register] = Value(hi=regs[register].hi, lo=replace(value.lo, const=value.lo.const), kind="other")

    def _transfer_arith(self, insn, info: _Info, regs: list[Value], state: State) -> None:
        base, size, operands = info.base, info.size, info.operands
        if len(operands) < 2:
            return
        src, dst = operands[0], operands[-1]
        if dst.type != M68K_OP_REG:
            return
        register = _reg(dst.reg)
        if register is None:
            return
        subtract = base.startswith("sub")
        source = self._operand_value(insn, src, size, state)
        current = regs[register]
        if base in ("adda", "suba") or register >= 8:
            # Address arithmetic: the low word is the address delta; the value
            # is no longer a record pointer's field.
            if src.type == M68K_OP_IMM:
                delta = src.imm & 0xFFFF
                if delta & 0x8000:
                    delta -= 0x10000
                if subtract:
                    delta = -delta
                new_offsets: frozenset[int] = frozenset()
                if current.cursor:
                    new_offsets = frozenset((offset + delta) & 15 for offset in current.offsets)
                regs[register] = replace(current, offsets=new_offsets)
            else:
                # Adding a data value to a pointer keeps the pointer's class.
                kind = current.kind if current.kind in _POINTER_KINDS else join_kind(current.kind, source.kind)
                regs[register] = Value.unknown(kind)
            return
        kind = join_kind(current.kind, source.kind)
        if size == 4:
            hi = _add_word(current.hi, source.hi, subtract)
            lo = _add_word(current.lo, source.lo, subtract)
            regs[register] = Value(hi=hi, lo=lo, kind=kind)
        else:
            lo = _add_word(current.lo, source.lo, subtract)
            regs[register] = Value(hi=current.hi, lo=lo, kind=kind)
        regs[register] = replace(regs[register], cursor=False, offsets=frozenset())

    def _transfer_logic(self, insn, info: _Info, regs: list[Value], state: State) -> None:
        base, size, operands = info.base, info.size, info.operands
        if len(operands) < 2:
            return
        src, dst = operands[0], operands[-1]
        if dst.type != M68K_OP_REG:
            return
        register = _reg(dst.reg)
        if register is None:
            return
        current = regs[register]
        source = self._operand_value(insn, src, size, state)
        if src.type == M68K_OP_IMM:
            if size == 4:
                immediate = src.imm & 0xFFFFFFFF
                high_mask, low_mask = (immediate >> 16) & 0xFFFF, immediate & 0xFFFF
            else:
                immediate = src.imm & (0xFF if size == 1 else 0xFFFF)
                high_mask, low_mask = None, immediate
            hi, lo = current.hi, current.lo
            if high_mask is not None:
                hi = replace(
                    hi, const=None if hi.const is None else _logic_const(base, hi.const, high_mask), transformed=True
                )
            lo = replace(
                lo, const=None if lo.const is None else _logic_const(base, lo.const, low_mask), transformed=True
            )
            regs[register] = Value(hi=hi, lo=lo, kind="value")
            return
        if size == 4:
            hi = _derive_word(current.hi, source.hi)
            lo = _derive_word(current.lo, source.lo)
        else:
            hi = current.hi
            lo = _derive_word(current.lo, source.lo)
        regs[register] = Value(hi=hi, lo=lo, kind="value")

    def _transfer_data(self, insn, info: _Info, regs: list[Value], state: State) -> None:
        base, size, operands = info.base, info.size, info.operands
        if not operands:
            return
        if base in _MUL:
            if len(operands) < 2:
                return
            dst, src = operands[-1], operands[0]
            register = _reg(dst.reg) if dst.type == M68K_OP_REG else None
            if register is None:
                return
            current = regs[register]
            source = self._operand_value(insn, src, size, state)
            constant = None
            if current.lo.const is not None and source.lo.const is not None:
                if base == "mulu":
                    constant = (current.lo.const * source.lo.const) & 0xFFFFFFFF
                elif base == "muls":
                    a = current.lo.const - 0x10000 if current.lo.const & 0x8000 else current.lo.const
                    b = source.lo.const - 0x10000 if source.lo.const & 0x8000 else source.lo.const
                    constant = (a * b) & 0xFFFFFFFF
                elif base == "divu" and source.lo.const:
                    constant = (current.lo.const // source.lo.const) & 0xFFFF
            if constant is None:
                regs[register] = Value(hi=_derive_word(current.hi), lo=_derive_word(current.lo, source.lo))
            else:
                regs[register] = Value.constant(constant, self.rom_size)
            return
        # Shift/rotate forms: one operand shifts the register by the immediate,
        # two operands shift the last operand by the first (register or count).
        if len(operands) == 1:
            dst = operands[0]
            count_op = None
        else:
            count_op, dst = operands[0], operands[-1]
        if dst.type != M68K_OP_REG:
            return
        register = _reg(dst.reg)
        if register is None:
            return
        current = regs[register]
        constant = None
        if count_op is not None and count_op.type == M68K_OP_IMM:
            count_op = count_op.imm & 63
        elif count_op is None:
            count_op = 1
        else:
            count_op = None
        if count_op is not None and current.lo.const is not None:
            value = current.lo.const
            count = count_op
            if base == "lsl":
                constant = (value << count) & (0xFFFFFFFF if size == 4 else 0xFFFF)
            elif base == "lsr":
                constant = (value >> count) & (0xFFFFFFFF if size == 4 else 0xFFFF)
            elif base == "asl":
                constant = (value << count) & (0xFFFFFFFF if size == 4 else 0xFFFF)
            elif base == "asr":
                signed = value - 0x10000 if (size == 2 and value & 0x8000) else value
                constant = (signed >> count) & (0xFFFFFFFF if size == 4 else 0xFFFF)
        if constant is not None:
            regs[register] = Value.constant(constant, self.rom_size)
        else:
            regs[register] = Value(hi=_derive_word(current.hi), lo=_derive_word(current.lo))

    # -- CFG --------------------------------------------------------------------

    def successors(self, pc: int, state: State | None = None) -> tuple[int, ...]:
        static = self._succ.get(pc)
        if static is None:
            info = self.info(pc)
            base = info.base
            fallthrough = pc + info.length
            targets: list[int] = []
            operand = info.operands[-1] if info.operands else None
            direct = list(self.branch_targets.get(pc, ()))
            if base in COND_BRANCH_MNEMONICS:
                targets = [*direct, fallthrough]
            elif base in CALL_MNEMONICS:
                if direct:
                    targets = direct
                elif operand is not None and operand.type == M68K_OP_MEM:
                    targets = self._indirect_targets(operand, None)
                targets = [*targets, fallthrough]
            elif base in UNCOND_BRANCH_MNEMONICS:
                if direct:
                    targets = direct
                elif operand is not None and operand.type == M68K_OP_MEM:
                    targets = self._indirect_targets(operand, None)
            elif base in TERMINAL_MNEMONICS:
                targets = []
            else:
                targets = [fallthrough]
            static = tuple(sorted({target for target in targets if 0 <= target < self.rom_size}))
            self._succ[pc] = static
        if state is None:
            return static
        info = self.info(pc)
        if info.base in CALL_MNEMONICS | UNCOND_BRANCH_MNEMONICS and not self.branch_targets.get(pc):
            operand = info.operands[-1] if info.operands else None
            if operand is not None and operand.type == M68K_OP_MEM:
                dynamic = self._indirect_targets(operand, state)
                if dynamic:
                    return tuple(sorted(set(static) | set(dynamic)))
        return static

    def _indirect_targets(self, operand, state: State | None) -> list[int]:
        mode = operand.address_mode
        mem = operand.mem
        if mode in (M68K_AM_MEMI_PRE_INDEX, M68K_AM_MEMI_POST_INDEX):
            disp = mem.in_disp if mode == M68K_AM_MEMI_PRE_INDEX else mem.out_disp
            return sorted(self.handlers.get(disp, ()))
        if mode in (M68K_AM_REGI_ADDR, M68K_AM_REGI_ADDR_DISP):
            base = self._mem_base(operand)
            if base is None or state is None:
                return []
            address = state[base].const32()
            if address is None:
                return []
            address = (address + mem.disp) & 0xFFFFFFFF
            return [address] if address in self.instructions else []
        return []

    # -- fixpoint ---------------------------------------------------------------

    def run(self) -> TraceResult:
        if not self.instructions:
            return TraceResult(self.game_id, (), (), (), {"instructions": 0, "writers": 0, "resolved": 0})
        self._find_work_bases()
        self._find_ram_constants()

        seeds = sorted(pc for pc in self.functions if pc in self.instructions)
        seeds += [pc for pc in self.extra_writers if pc in self.instructions and pc not in seeds]
        if not seeds:
            seeds = [min(self.instructions)]

        # Static reachability decides which seeds carry an entry state: a seed
        # with a real predecessor inherits its registers from that edge.
        reachable: set[int] = set()
        stack = list(reversed(seeds))
        while stack:
            pc = stack.pop()
            if pc in reachable or pc not in self.instructions:
                continue
            reachable.add(pc)
            stack.extend(reversed([s for s in self.successors(pc) if s not in reachable]))
        predecessors: dict[int, int] = {}
        for pc in reachable:
            for target in self.successors(pc):
                predecessors[target] = predecessors.get(target, 0) + 1

        queue = []
        for seed in seeds:
            if predecessors.get(seed, 0) == 0:
                self.entry_state[seed] = self._entry_default()
            queue.append(seed)
        queued = set(queue)
        transfers = 0
        limit = 4_000_000
        while queue:
            pc = queue.pop()
            queued.discard(pc)
            state = self.entry_state.get(pc)
            if state is None:
                continue
            out_state = self.transfer(pc, state)
            transfers += 1
            if transfers > limit:  # pragma: no cover - pathological image guard
                raise RuntimeError("sprite trace did not converge")
            for target in self.successors(pc, state):
                if target not in self.instructions:
                    continue
                new_state = out_state
                if target in self.functions and self.work_bases:
                    # a5 is the game work-RAM page pointer, fixed at reset; pin
                    # it at call boundaries so a caller's dead a5 never hides
                    # the RAM addresses the callee reads or writes.
                    pinned = self._const_union(self.work_bases)
                    new_state = (*new_state[:A5], pinned, *new_state[A5 + 1 :])
                previous = self.entry_state.get(target)
                if previous is None:
                    self.entry_state[target] = new_state
                    if target not in queued:
                        queue.append(target)
                        queued.add(target)
                else:
                    merged = merge_state(previous, new_state)
                    if merged != previous:
                        self.entry_state[target] = merged
                        if target not in queued:
                            queue.append(target)
                            queued.add(target)

        # A writer seed that no static path reaches (e.g. copied from a runtime
        # log) still gets a default parameter state so its own operands resolve.
        for pc in self.extra_writers:
            if pc in self.instructions and pc not in self.entry_state:
                self.entry_state[pc] = self._entry_default()

        analysed = set(self.entry_state)
        writers, unresolved = self._collect_writers(analysed)
        classifications = self._classify_fields(analysed, writers)
        resets = tuple(
            ResetWriter(item.pc, item.base, item.offset, item.instruction, item.evidence)
            for item in classifications
            if item.kind == "reset"
        )
        stats = {
            "instructions": len(self.instructions),
            "analysed_instructions": len(analysed),
            "dispatch_targets": sum(len(targets) for targets in self.handlers.values()),
            "x_y_writers": self.stats_writers,
            "resolved_writers": len(writers),
            "unresolved_writers": len(unresolved),
            "undeclared_x_y_writers": self.stats_writers - len(writers),
            "root_fields": len(
                {(w.base, offset) for w in writers for offset in (w.x_offset, w.y_offset) if offset != 0xFFFF}
            ),
            "group_classes": len({w.group_class for w in writers}),
            "field_writers": len(classifications),
            "resets": len(resets),
        }
        return TraceResult(self.game_id, tuple(writers), resets, tuple(classifications), stats, tuple(unresolved))

    @staticmethod
    def _merge_entry(pc: int, state: State) -> State:
        return state

    # -- writers ----------------------------------------------------------------

    def _collect_writers(self, analysed: set[int]) -> tuple[list[Writer], list[UnresolvedWriter]]:
        found: dict[int, Writer] = {}
        unresolved: list[UnresolvedWriter] = []
        self.stats_writers = 0
        for pc in sorted(analysed):
            insn = self.instructions[pc]
            info = self.info(pc)
            if info.base not in _STORE_BASE or len(info.operands) < 2:
                continue
            destination = info.operands[-1]
            if destination.type != M68K_OP_MEM or destination.mem.index_reg:
                continue
            if destination.address_mode not in (
                M68K_AM_REGI_ADDR,
                M68K_AM_REGI_ADDR_DISP,
                M68K_AM_REGI_ADDR_POST_INC,
                M68K_AM_REGI_ADDR_PRE_DEC,
            ):
                continue
            base = self._mem_base(destination)
            if base is None:
                continue
            size = info.size
            if size not in (2, 4):
                continue
            state = self.entry_state[pc]
            cursor = state[base]
            lane = 0
            forced = self.writer_lanes.get(pc)
            if forced is not None and (forced == LANE_XY) == (size == 4):
                lane = forced
            elif cursor.cursor and len(cursor.offsets) == 1:
                offset = (next(iter(cursor.offsets)) + destination.mem.disp) & 15
                if size == 2 and offset == 4:
                    lane = LANE_X
                elif size == 2 and offset == 6:
                    lane = LANE_Y
                elif size == 4 and offset == 4:
                    lane = LANE_XY
            if lane == 0:
                continue
            self.stats_writers += 1
            source = self._operand_value(insn, info.operands[0], size, state)
            x_root: RootField | None = None
            y_root: RootField | None = None
            x_word = source.hi if size == 4 else source.lo
            y_word = source.lo
            rigid = True
            if lane & LANE_X:
                x_root = self._anchor(x_word)
                rigid = rigid and x_word.exact and x_word.roots <= x_word.direct
            if lane & LANE_Y:
                y_root = self._anchor(y_word)
                rigid = rigid and y_word.exact and y_word.roots <= y_word.direct
            destination_text = info.insn.op_str.rsplit(",", 1)[-1].strip() if info.insn.op_str else ""
            missing = [
                name
                for name, root in (("X", x_root), ("Y", y_root))
                if lane & (LANE_X if name == "X" else LANE_Y) and root is None
            ]
            if missing:
                words = {"X": x_word, "Y": y_word}
                reason = "; ".join(f"{name}: {_unresolved_reason(words[name])}" for name in missing)
                unresolved.append(
                    UnresolvedWriter(
                        pc,
                        base,
                        x_root.base if x_root is not None else (y_root.base if y_root is not None else base),
                        lane,
                        destination_text,
                        reason,
                    )
                )
                continue
            if lane == LANE_XY and x_root is not None and y_root is not None and x_root.base != y_root.base:
                unresolved.append(
                    UnresolvedWriter(
                        pc,
                        base,
                        x_root.base,
                        lane,
                        destination_text,
                        f"ambiguous root base: {_fields_text({x_root, y_root})}",
                    )
                )
                continue
            roots = tuple(sorted({root for root in (x_root, y_root) if root is not None}))
            found[pc] = Writer(
                pc=pc,
                cursor_reg=base,
                base=x_root.base if x_root is not None else y_root.base,
                lane=lane,
                x_offset=x_root.offset if x_root is not None else 0xFFFF,
                y_offset=y_root.offset if y_root is not None else 0xFFFF,
                group_class=0,
                rigid=rigid,
                root_offsets=roots,
            )
        writers = list(found.values())
        self._assign_classes(writers)
        writers.sort(key=lambda writer: writer.pc)
        unresolved.sort(key=lambda writer: writer.pc)
        return writers, unresolved

    @staticmethod
    def _anchor(word: Word) -> RootField | None:
        if len(word.direct) != 1 or not word.exact:
            return None
        return next(iter(word.direct))

    def _assign_classes(self, writers: list[Writer]) -> None:
        # A group is a pair of position fields.  XY writers state the pair
        # directly; single-lane writers are paired with the field two bytes
        # away, the layout every record in both games uses (x at +n, y at +n+2).
        pairs: dict[tuple[int, int], set[tuple[int, int]]] = {}
        for writer in writers:
            if writer.lane == LANE_XY:
                pairs.setdefault((writer.base, writer.x_offset), set()).add((writer.x_offset, writer.y_offset))
        for writer in writers:
            if writer.lane == LANE_XY:
                continue
            if writer.lane == LANE_X:
                candidate = (writer.x_offset, writer.x_offset + 2)
            else:
                candidate = (writer.y_offset - 2, writer.y_offset)
            known = pairs.setdefault((writer.base, writer.x_offset if writer.lane == LANE_X else candidate[0]), set())
            known.add(candidate)
        keys = sorted((base, x, y) for (base, _), group in pairs.items() for (x, y) in group)
        classes = {key: index + 1 for index, key in enumerate(dict.fromkeys(keys))}
        for index, writer in enumerate(writers):
            if writer.lane == LANE_XY:
                key = (writer.base, writer.x_offset, writer.y_offset)
            elif writer.lane == LANE_X:
                key = (writer.base, writer.x_offset, writer.x_offset + 2)
            else:
                key = (writer.base, writer.y_offset - 2, writer.y_offset)
            writers[index] = replace(writer, group_class=classes.get(key, 0))

    # -- continuity -------------------------------------------------------------

    def _classify_fields(self, analysed: set[int], writers: list[Writer]) -> list[Classification]:
        fields = sorted(
            {
                (writer.base, offset)
                for writer in writers
                for offset in (writer.x_offset, writer.y_offset)
                if offset != 0xFFFF
            }
        )
        fallback = self._entry_default()
        items: list[Classification] = []
        for pc in sorted(self.instructions):
            insn = self.instructions[pc]
            info = self.info(pc)
            if info.base in _NO_WRITE or info.base not in _STORE_BASE or not info.operands:
                continue
            destination = info.operands[-1]
            if destination.type != M68K_OP_MEM or destination.mem.index_reg:
                continue
            if destination.address_mode not in (M68K_AM_REGI_ADDR, M68K_AM_REGI_ADDR_DISP):
                continue
            base = self._mem_base(destination)
            if base is None:
                continue
            start = destination.mem.disp
            covered = [
                (field_base, field_offset)
                for field_base, field_offset in fields
                if field_base == base
                and start <= field_offset
                and field_offset + 2 <= start + info.size
                and (field_offset - start) % 2 == 0
            ]
            if not covered:
                continue
            field_base, field_offset = covered[0]
            state = self.entry_state.get(pc, fallback)
            kind = "continuation" if info.base in _RMW_BASE else "reset"
            text = info.insn.mnemonic + (" " + info.insn.op_str if info.insn.op_str else "")
            evidence = text
            if kind == "reset" and len(info.operands) >= 2:
                value = self._operand_value(insn, info.operands[0], info.size, state)
                lane_word = value.lo if field_offset != start else (value.hi if info.size == 4 else value.lo)
                roots = sorted(lane_word.roots | lane_word.direct)
                has_field = any(root.base == field_base and root.offset == field_offset for root in roots)
                # A derived per-frame projection of the SAME object continues the
                # group: its value is a function of other record fields, so it was
                # computed from the previous origin (body/shadow anchors in
                # commandw's 0xa1c8 are written exactly this way every frame).
                # A constant, table, foreign record or unproven value replaces the
                # position and therefore starts a new generation.
                same_record = bool(roots) and all(root.base == field_base for root in roots)
                projection = {root for root in roots if root.offset != field_offset}
                # A plain single-field copy is the spawn/teleport shape and must
                # stay a birth.  A projection combines two or more record fields,
                # or one field plus arithmetic/scale, so it was computed from the
                # object's previous origin.
                projected = len(projection) >= 2 or (len(projection) == 1 and lane_word.transformed)
                if has_field:
                    kind = "continuation"
                    evidence += " <- field"
                elif same_record and lane_word.exact and projected:
                    kind = "continuation"
                    evidence += " <- same-record projection from " + _fields_text(roots)
                else:
                    evidence += " <- " + _fields_text(roots)
            elif kind == "continuation":
                evidence += " (read-modify-write of the field)"
            if len(covered) > 1:
                evidence += " (also writes " + ", ".join(f"+0x{off:02x}" for _, off in covered[1:]) + ")"
            items.append(Classification(pc, field_base, field_offset, kind, text, evidence))
        return items


def trace(
    rom: bytes,
    discovery,
    config: Mapping | None = None,
    writer_seeds: Iterable[int] = (),
    writer_lanes: Mapping[int, int] | None = None,
) -> TraceResult:
    """Trace sprite X/Y writers and root-field writers for one image."""
    return SpriteTrace(rom, discovery, config, writer_seeds, writer_lanes).run()


# ---------------------------------------------------------------------------
# Output


def render_header(result: TraceResult) -> str:
    lines = [
        "/* Generated by recomp/sprite_trace.py. Do not commit. */\n",
        "#pragma once\n",
        "#include <span>\n",
        '#include "f3rt/sprite_group.hpp"\n',
        "namespace f3rt::sprite_groups {\n",
    ]
    if result.writers:
        lines.append("inline constexpr SpriteGroupWrite writes[] = {\n")
        for writer in result.writers:
            lines.append(
                f"    {{0x{writer.pc:08x}u, {writer.base}u, {writer.lane}u, {writer.group_class}u, "
                f"{DEFAULT_SHIFT}u, 0x{writer.x_offset:04x}u, 0x{writer.y_offset:04x}u}},\n"
            )
        lines.append("};\n")
    if result.resets:
        lines.append("inline constexpr SpriteGroupReset resets[] = {\n")
        for reset in result.resets:
            lines.append(f"    {{0x{reset.pc:08x}u, {reset.base}u, 0u, 0x{reset.offset:04x}u}},\n")
        lines.append("};\n")
    writes_span = (
        "std::span<const SpriteGroupWrite>(writes)" if result.writers else "std::span<const SpriteGroupWrite>{}"
    )
    resets_span = (
        "std::span<const SpriteGroupReset>(resets)" if result.resets else "std::span<const SpriteGroupReset>{}"
    )
    lines.append(f"inline constexpr SpriteGroupTable table{{{writes_span}, {resets_span}}};\n")
    lines.append("} // namespace f3rt::sprite_groups\n")
    return "".join(lines)


def write_outputs(result: TraceResult, output: Path, write_if_changed) -> None:
    """Write ``sprite_groups.hpp`` and ``sprite_groups.json`` into ``output``."""
    output = Path(output)
    write_if_changed(output / "sprite_groups.hpp", render_header(result))
    write_if_changed(output / "sprite_groups.json", json.dumps(result.to_json(), indent=2) + "\n")


def load_writer_seeds(path: str | Path) -> tuple[list[int], dict[int, int]]:
    """Read ``--sprite-writers`` JSON: an array of PCs or ``{"writers": [...]}``."""
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    lanes: dict[int, int] = {}
    if isinstance(data, list):
        records = data
    elif isinstance(data, dict):
        records = data.get("writers", [])
    else:
        raise ValueError("sprite writers file must contain an array or an object with 'writers'")
    seeds: list[int] = []
    for record in records:
        if isinstance(record, int) and not isinstance(record, bool):
            pc = record
        elif isinstance(record, dict) and "pc" in record:
            pc = record["pc"]
            lane_name = record.get("lane")
            if lane_name is not None:
                lanes[pc] = {"X": LANE_X, "Y": LANE_Y, "XY": LANE_XY}[str(lane_name).upper()]
        else:
            raise ValueError(f"invalid sprite writer record: {record!r}")
        if isinstance(pc, bool) or not isinstance(pc, int) or pc < 0 or pc % 2:
            raise ValueError(f"sprite writer PC must be an even non-negative integer: {pc!r}")
        seeds.append(pc)
    return sorted(set(seeds)), lanes
