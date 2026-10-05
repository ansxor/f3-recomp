"""Shared physical-chip validation and ordered lane placement for ROM manifests.

Lane size/CRC/SHA1 describe the full file; source_offset/length select a CONTINUE
or RELOAD slice only after integrity validation. Ordered overlapping lanes are
intentional. Unwritten region bytes retain fill (default zero, or explicit 255);
mirror_size repeats the entire physical region, including its sparse contents.
"""
from __future__ import annotations

from collections import Counter
import hashlib
from pathlib import Path
import zlib

REGIONS = ("rom", "sprites", "sprites_hi", "tiles", "tiles_hi", "sound", "samples", "eeprom")


def region_spec(config: dict, name: str) -> tuple[int, int, list[dict]]:
    region = config.get(name, {"size": 0, "lanes": []}) if name == "eeprom" else config[name]
    size, fill = region["size"], region.get("fill", 0)
    if not isinstance(size, int) or size < 0 or fill not in (0, 255):
        raise ValueError(f"Invalid {name} region size/fill")
    if name == "eeprom" and (size not in (0, 128) or region.get("mirror_size", size) != size):
        raise ValueError("EEPROM region must be absent or exactly 128 bytes")
    lanes = []
    identities = {}
    for source in region.get("lanes", []):
        lane = dict(source)
        lane.setdefault("stride", region.get("interleave", 1))
        lane.setdefault("group", 1)
        lane.setdefault("offset", 0)
        lane.setdefault("source_offset", 0)
        lane.setdefault("length", lane["size"] - lane["source_offset"])
        physical_size, source_offset, length, offset, stride, group = (
            lane[key] for key in ("size", "source_offset", "length", "offset", "stride", "group"))
        if not all(isinstance(value, int) for value in
                   (physical_size, source_offset, length, offset, stride, group)):
            raise ValueError(f"Invalid {name} lane geometry")
        if (physical_size <= 0 or source_offset < 0 or length <= 0 or
                source_offset + length > physical_size or group <= 0 or
                stride < group or offset < 0 or length % group):
            raise ValueError(f"Invalid {name} lane geometry: {lane['file']}")
        if offset + (length // group - 1) * stride + group > size:
            raise ValueError(f"{name} lane exceeds region: {lane['file']}")
        for key, digits in (("crc", 8), ("sha1", 40)):
            value = lane[key]
            if not isinstance(value, str) or len(value) != digits:
                raise ValueError(f"Invalid {name} {key}: {lane['file']}")
            int(value, 16)
        if "short_size" in lane:
            if not 0 < lane["short_size"] < physical_size:
                raise ValueError(f"Invalid short dump size: {lane['file']}")
            for key, digits in (("short_crc", 8), ("short_sha1", 40)):
                if len(lane[key]) != digits:
                    raise ValueError(f"Invalid {key}: {lane['file']}")
                int(lane[key], 16)
        identity = tuple(lane.get(key) for key in
                         ("size", "crc", "sha1", "short_size", "short_crc", "short_sha1"))
        previous = identities.setdefault(lane["file"], identity)
        if previous != identity:
            raise ValueError(f"Inconsistent physical chip identity: {lane['file']}")
        lanes.append(lane)
    if size and not lanes:
        raise ValueError(f"Missing {name} ROM lanes")
    return size, fill, lanes


def _validate(data: bytes, lane: dict, prefix: str = "") -> None:
    if len(data) != lane[prefix + "size"]:
        raise ValueError(f"ROM chip {lane['file']!r} size mismatch")
    if zlib.crc32(data) & 0xffffffff != int(lane[prefix + "crc"], 16):
        raise ValueError(f"ROM chip {lane['file']!r} CRC32 mismatch")
    if hashlib.sha1(data).hexdigest() != lane[prefix + "sha1"].lower():
        raise ValueError(f"ROM chip {lane['file']!r} SHA1 mismatch")


def load_region(config: dict, name: str, rom_dir: Path) -> bytes:
    size, fill, lanes = region_spec(config, name)
    image = bytearray([fill]) * size
    # CONTINUE lanes share a validated full physical chip; slicing never weakens integrity.
    chips = {}
    remaining = Counter(lane["file"] for lane in lanes)
    for lane in lanes:
        data = chips.get(lane["file"])
        if data is None:
            data = (Path(rom_dir) / lane["file"]).read_bytes()
            if len(data) == lane.get("short_size"):
                _validate(data, lane, "short_")
                data += b"\xff" * (lane["size"] - len(data))
            _validate(data, lane)
            chips[lane["file"]] = data
        offset, stride, group = (lane[key] for key in ("offset", "stride", "group"))
        start, length = lane["source_offset"], lane["length"]
        view = memoryview(data)
        for byte in range(group):
            end = offset + byte + (length // group - 1) * stride + 1
            image[offset + byte:end:stride] = view[start + byte:start + length:group]
        remaining[lane["file"]] -= 1
        if not remaining[lane["file"]]:
            del chips[lane["file"]]
    mapped_size = config.get(name, {}).get("mirror_size", size)
    if mapped_size < size or (size and mapped_size % size) or (not size and mapped_size):
        raise ValueError(f"Invalid {name} mirrored size")
    return bytes(image) * (mapped_size // size if size else 1)
