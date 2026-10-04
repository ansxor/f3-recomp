"""Versioned address-only execution profiles shared by both native compilers."""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import zlib

VERSION = 1
MAX_COUNT = (1 << 64) - 1


@dataclass
class BlockProfile:
    roms: dict[tuple[str, int], tuple[int, int]] = field(default_factory=dict)
    hits: dict[tuple[str, int, int], int] = field(default_factory=dict)
    misses: dict[tuple[str, int, int], int] = field(default_factory=dict)

    def merge(self, other: BlockProfile) -> None:
        for key, bounds in other.roms.items():
            if key in self.roms and self.roms[key] != bounds:
                raise ValueError(f"profile ROM bounds disagree for {key}")
            self.roms[key] = bounds
        for destination, source in ((self.hits, other.hits), (self.misses, other.misses)):
            for key, count in source.items():
                destination[key] = min(MAX_COUNT, destination.get(key, 0) + count)

    def write(self, path: Path) -> None:
        lines = [f"F3-BLOCK-PROFILE {VERSION}"]
        for (region, crc), (base, size) in sorted(self.roms.items()):
            lines.append(f"rom {region} {crc:08x} {base:08x} {size:08x}")
        for kind, rows in (("hit", self.hits), ("miss", self.misses)):
            for (region, crc, address), count in sorted(rows.items()):
                if count:
                    lines.append(f"{kind} {region} {crc:08x} {address:08x} {count}")
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + ".tmp")
        temporary.write_text("\n".join(lines) + "\n", encoding="ascii")
        temporary.replace(path)


def read_profile(path: Path) -> BlockProfile:
    lines = Path(path).read_text(encoding="ascii").splitlines()
    if not lines or lines[0] != f"F3-BLOCK-PROFILE {VERSION}":
        raise ValueError(f"unsupported block profile version: {path}")
    profile = BlockProfile()
    rows = []
    for number, line in enumerate(lines[1:], 2):
        fields = line.split()
        if not fields:
            continue
        try:
            kind, region, crc_text, address_text, value_text = fields
            if region not in ("main", "sound") or kind not in ("rom", "hit", "miss"):
                raise ValueError("unknown record")
            if any(len(value) != 8 or any(c not in "0123456789abcdefABCDEF" for c in value)
                   for value in (crc_text, address_text)):
                raise ValueError("expected eight hex digits")
            crc, address = int(crc_text, 16), int(address_text, 16)
            if kind == "rom":
                if len(value_text) != 8 or any(c not in "0123456789abcdefABCDEF" for c in value_text):
                    raise ValueError("expected eight hex size digits")
                size = int(value_text, 16)
                if address & 1 or not size or size & 1 or address + size > 1 << 32:
                    raise ValueError("invalid ROM bounds")
                if (region, crc) in profile.roms:
                    raise ValueError("duplicate ROM identity")
                profile.roms[region, crc] = (address, size)
            else:
                if not value_text.isascii() or not value_text.isdecimal():
                    raise ValueError("expected decimal count")
                count = int(value_text)
                if not 0 < count <= MAX_COUNT:
                    raise ValueError("count outside uint64")
                rows.append((kind, region, crc, address, count))
        except ValueError as error:
            raise ValueError(f"invalid profile record {path}:{number}: {error}") from error
    for kind, region, crc, address, count in rows:
        bounds = profile.roms.get((region, crc))
        if bounds is None:
            raise ValueError(f"profile record without ROM identity: {region} {crc:08x}")
        base, size = bounds
        if address & 1 or not base <= address < base + size:
            raise ValueError(f"profile address outside aligned ROM: {address:08x}")
        destination = profile.hits if kind == "hit" else profile.misses
        key = region, crc, address
        destination[key] = min(MAX_COUNT, destination.get(key, 0) + count)
    return profile


def load_hot(path: Path, region: str, rom: bytes, base: int = 0) -> set[int]:
    profile = read_profile(path)
    crc = zlib.crc32(rom) & 0xffffffff
    if profile.roms.get((region, crc)) != (base, len(rom)):
        raise ValueError(f"profile lacks matching {region} ROM CRC {crc:08x}, base {base:08x}, size {len(rom):08x}")
    return {address for (cpu, identity, address), count in profile.hits.items()
            if cpu == region and identity == crc and count}
