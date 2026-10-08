"""Emit-unit declarations ([[video.emit_units]], [video.frame_writers]).

A unit is a span of game code that draws one object/record into sprite RAM.
This module is the single parser for the schema: tools/compile_sprite_units.py
renders the C/C++ headers from it and recomp/generate.py plants the hooks and
the digest guard from it, so both sides always agree on ids and PCs.
"""
from __future__ import annotations

from dataclasses import dataclass
import re
import zlib

REGISTERS = tuple(f"d{i}" for i in range(8)) + tuple(f"a{i}" for i in range(7))
OWNERS = ("unit", "writer")
_UNIT_KEYS = {"name", "start", "end", "unit", "size", "owner"}
_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
# Names the generated namespace already defines, plus C++ keywords that a plain
# `inline constexpr EmitUnit <name>` would not survive.
_RESERVED = {"all", "frame_writers", "digest", "detail",
             "alignas", "alignof", "asm", "auto", "bool", "break", "case", "catch",
             "char", "class", "concept", "const", "constexpr", "continue", "default",
             "delete", "do", "double", "else", "enum", "explicit", "export", "extern",
             "false", "float", "for", "friend", "goto", "if", "inline", "int", "long",
             "namespace", "new", "noexcept", "nullptr", "operator", "private",
             "protected", "public", "register", "requires", "return", "short",
             "signed", "sizeof", "static", "struct", "switch", "template", "this",
             "throw", "true", "try", "typedef", "typename", "union", "unsigned",
             "using", "virtual", "void", "volatile", "while"}
_MAX_PC = 1 << 24


@dataclass(frozen=True)
class EmitUnitSpec:
    id: int
    name: str
    starts: tuple[int, ...]
    ends: tuple[int, ...]
    reg: str
    size: int
    owner: str


@dataclass(frozen=True)
class SpriteUnitsSpec:
    units: tuple[EmitUnitSpec, ...]
    frame_writers: tuple[tuple[int, int], ...]

    def hook_pcs(self) -> tuple[dict[int, list[int]], dict[int, list[int]]]:
        """(enter, exit) maps: PC -> unit ids, in declaration order."""
        enter: dict[int, list[int]] = {}
        leave: dict[int, list[int]] = {}
        for unit in self.units:
            for pc in unit.starts:
                enter.setdefault(pc, []).append(unit.id)
            for pc in unit.ends:
                leave.setdefault(pc, []).append(unit.id)
        return enter, leave


def _pc(value, what: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{what} must be an integer, got {value!r}")
    if value < 0 or value >= _MAX_PC:
        raise ValueError(f"{what} {value:#x} outside the 24-bit address space")
    if value & 1:
        raise ValueError(f"{what} {value:#x} must be even")
    return value


def _pc_list(value, what: str) -> tuple[int, ...]:
    items = value if isinstance(value, list) else [value]
    if not items:
        raise ValueError(f"{what} must not be empty")
    return tuple(_pc(item, what) for item in items)


def parse_sprite_units(config: dict) -> SpriteUnitsSpec:
    video = config.get("video", {})
    raw_units = video.get("emit_units", [])
    if not isinstance(raw_units, list):
        raise ValueError("video.emit_units must be an array of tables")
    units: list[EmitUnitSpec] = []
    names: set[str] = set()
    for index, raw in enumerate(raw_units):
        if not isinstance(raw, dict):
            raise ValueError(f"video.emit_units[{index}] must be a table")
        unknown = set(raw) - _UNIT_KEYS
        if unknown:
            raise ValueError(f"video.emit_units[{index}]: unknown keys {sorted(unknown)}")
        for key in ("name", "start", "end", "unit"):
            if key not in raw:
                raise ValueError(f"video.emit_units[{index}]: missing '{key}'")
        name = raw["name"]
        if (not isinstance(name, str) or not _IDENTIFIER.match(name)
                or name in _RESERVED):
            raise ValueError(f"video.emit_units[{index}]: invalid unit name {name!r}")
        if name in names:
            raise ValueError(f"duplicate emit unit name {name!r}")
        names.add(name)
        where = f"emit unit {name!r}"
        starts = _pc_list(raw["start"], f"{where} start")
        ends = _pc_list(raw["end"], f"{where} end")
        if len(starts) != len(ends):
            raise ValueError(f"{where}: start has {len(starts)} entries, end has {len(ends)}")
        if len(set(starts)) != len(starts) or len(set(ends)) != len(ends):
            raise ValueError(f"{where}: duplicate start or end PC")
        for start, end in zip(starts, ends):
            if start >= end:
                raise ValueError(f"{where}: start {start:#x} must be below end {end:#x}")
        reg = raw["unit"]
        if reg not in REGISTERS:
            raise ValueError(f"{where}: unit register {reg!r} must be one of d0-d7/a0-a6")
        size = raw.get("size", 0)
        if isinstance(size, bool) or not isinstance(size, int) or size < 0:
            raise ValueError(f"{where}: size must be a non-negative integer")
        owner = raw.get("owner", "unit")
        if owner not in OWNERS:
            raise ValueError(f"{where}: owner must be 'unit' or 'writer', got {owner!r}")
        units.append(EmitUnitSpec(len(units), name, starts, ends, reg, size, owner))

    writers_table = video.get("frame_writers", {})
    if not isinstance(writers_table, dict) or set(writers_table) - {"ranges"}:
        raise ValueError("video.frame_writers must be a table with only 'ranges'")
    writers: list[tuple[int, int]] = []
    for index, raw in enumerate(writers_table.get("ranges", [])):
        if (not isinstance(raw, list) or len(raw) != 2
                or any(isinstance(v, bool) or not isinstance(v, int) for v in raw)):
            raise ValueError(f"video.frame_writers.ranges[{index}] must be [first, last]")
        first, last = raw
        # `last` is inclusive and may name the final byte of a word.
        _pc(first, f"frame writer range {index} first")
        if last < first or last >= _MAX_PC:
            raise ValueError(f"frame writer range {index}: invalid [{first:#x}, {last:#x}]")
        writers.append((first, last))
    return SpriteUnitsSpec(tuple(units), tuple(writers))


def canonical(spec: SpriteUnitsSpec) -> str:
    units = ";".join(
        f"{u.name}|{','.join(f'{pc:x}' for pc in u.starts)}|"
        f"{','.join(f'{pc:x}' for pc in u.ends)}|{u.reg}|{u.size}|{u.owner}"
        for u in spec.units)
    writers = ",".join(f"{a:x}-{b:x}" for a, b in spec.frame_writers)
    return f"units:{units}\nframe_writers:{writers}\n"


def digest(spec: SpriteUnitsSpec) -> int:
    return zlib.crc32(canonical(spec).encode("ascii")) & 0xffffffff


def guard_lines(spec: SpriteUnitsSpec) -> str:
    """C text for every generated file that contains a unit hook."""
    return ('#include "sprite_units.h"\n'
            f'#if F3_SPRITE_UNITS_DIGEST != 0x{digest(spec):08x}u\n'
            '#error "Generated program and sprite_units.h describe different emit units"\n'
            '#endif\n')


_BANNER = "/* Generated by tools/compile_sprite_units.py from config.toml. Do not commit. */\n"


def render_c_header(spec: SpriteUnitsSpec) -> str:
    return (_BANNER + "#ifndef F3_SPRITE_UNITS_H\n#define F3_SPRITE_UNITS_H\n"
            f"#define F3_SPRITE_UNITS_DIGEST 0x{digest(spec):08x}u\n"
            f"#define F3_SPRITE_UNIT_COUNT {len(spec.units)}\n#endif\n")


def _array(values, fmt) -> str:
    return ", ".join(fmt(v) for v in values)


def render_cpp_header(spec: SpriteUnitsSpec) -> str:
    out = [_BANNER, "#pragma once\n", "#include <array>\n#include <cstddef>\n#include <cstdint>\n",
           '#include "f3rt/emit_unit.hpp"\n#include "sprite_units.h"\n\n',
           "namespace f3rt::sprite_units {\n\n"]
    if spec.units:
        out.append("namespace detail {\n")
        for unit in spec.units:
            n = len(unit.starts)
            out.append(f"inline constexpr std::array<uint32_t, {n}> {unit.name}_starts{{"
                       f"{_array(unit.starts, lambda v: f'0x{v:x}u')}}};\n")
            out.append(f"inline constexpr std::array<uint32_t, {n}> {unit.name}_ends{{"
                       f"{_array(unit.ends, lambda v: f'0x{v:x}u')}}};\n")
        out.append("} // namespace detail\n\n")
    for unit in spec.units:
        out.append(
            f'inline constexpr EmitUnit {unit.name}{{{unit.id}u, "{unit.name}", '
            f"detail::{unit.name}_starts, detail::{unit.name}_ends, "
            f"UnitRegister::{unit.reg.upper()}, {unit.size}u, "
            f"UnitOwner::{unit.owner.capitalize()}}};\n")
    out.append(f"\ninline constexpr std::array<const EmitUnit *, {len(spec.units)}> all{{"
               f"{_array(spec.units, lambda u: '&' + u.name)}}};\n")
    out.append(f"inline constexpr std::array<PcRange, {len(spec.frame_writers)}> "
               f"frame_writers{{{_array(spec.frame_writers, lambda r: f'PcRange{{0x{r[0]:x}u, 0x{r[1]:x}u}}')}}};\n")
    out.append("inline constexpr uint32_t digest = F3_SPRITE_UNITS_DIGEST;\n\n")
    out.append(
        "constexpr bool valid_units() {\n"
        "    for (std::size_t i = 0; i < all.size(); ++i) {\n"
        "        const EmitUnit &unit = *all[i];\n"
        "        if (unit.id != i || unit.starts.size() == 0 || unit.starts.size() != unit.ends.size())\n"
        "            return false;\n"
        "        for (std::size_t j = 0; j < unit.starts.size(); ++j)\n"
        "            if ((unit.starts[j] & 1u) || (unit.ends[j] & 1u) || unit.starts[j] >= unit.ends[j])\n"
        "                return false;\n"
        "    }\n"
        "    return true;\n"
        "}\n"
        "static_assert(valid_units(), \"emit unit ids must equal their index; PCs even, start < end, "
        "starts/ends of equal arity\");\n"
        f"static_assert(all.size() == F3_SPRITE_UNIT_COUNT, \"unit count mismatch\");\n\n"
        "} // namespace f3rt::sprite_units\n")
    return "".join(out)
