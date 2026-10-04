#!/usr/bin/env python3
"""Merge address-only profiles and measure coverage/native binary contributions."""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from recomp.block_profile import BlockProfile, read_profile


# Descriptive bins, never discovery exclusions or proofs of code/data ownership.
# The fill bounds were measured from the supplied Japan ROM. The middle range is
# data/gfx-looking (zero-heavy tables); actual execution is reported, not assumed.
def region(cpu: str, pc: int) -> str:
    if cpu == "sound":
        return "sound_program"
    if 0x7030 <= pc < 0x10000 or 0x11b362 <= pc < 0x1ffffe:
        return "main_padding"
    if 0x10000 <= pc < 0x90000:
        return "main_gfx_looking"
    return "main_code"


def macho_text(binary: Path) -> tuple[int, int, int]:
    with binary.open("rb") as file:
        header = file.read(32)
        magic, _, _, _, commands, _, _, _ = struct.unpack("<8I", header)
        if magic != 0xfeedfacf:
            raise ValueError("measurement requires a thin little-endian Mach-O 64 executable")
        segment_size = section_address = section_size = 0
        for _ in range(commands):
            cmd, size = struct.unpack("<2I", file.read(8))
            body = file.read(size - 8)
            if cmd != 0x19:
                continue
            name, _, vmsize, _, _, _, _, sections, _ = struct.unpack_from("<16s4Q4I", body)
            if name.rstrip(b"\0") == b"__TEXT":
                segment_size = vmsize
                for index in range(sections):
                    section, _, address, length = struct.unpack_from("<16s16s2Q", body, 64 + index * 80)
                    if section.rstrip(b"\0") == b"__text":
                        section_address, section_size = address, length
        return segment_size, section_address, section_size


def text_contributions(binary: Path, start: int, size: int) -> dict[str, int]:
    symbols = subprocess.run(["nm", "-n", str(binary)], check=True, text=True, capture_output=True).stdout
    entries = {}
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) < 3:
            continue
        try:
            address = int(fields[0], 16)
        except ValueError:
            continue
        if start <= address < start + size:
            entries.setdefault(address, fields[-1])
    addresses = sorted(entries)
    contribution = Counter()
    for index, address in enumerate(addresses):
        end = addresses[index + 1] if index + 1 < len(addresses) else start + size
        match = re.search(r"_f3_(native|sound_block)_([0-9a-f]{6})", entries[address])
        category = region("main" if match[1] == "native" else "sound", int(match[2], 16)) if match else "runtime_shared"
        contribution[category] += end - address
    contribution["runtime_shared"] += (addresses[0] - start) if addresses else size
    return dict(contribution)


def source_contributions(directory: Path, cpu: str) -> dict[str, int]:
    contribution = Counter()
    manifest = json.loads((directory / ("lowering.json" if cpu == "main" else "coverage.json")).read_text())
    for name in manifest["source_files"]:
        path = directory / name
        context = "generated_shared_overhead"
        with path.open("rb") as file:
            for line in file:
                match = re.search(rb"(?:L_|f3_native_|f3_sound_block_)([0-9a-f]{6})(?:\b|_)", line)
                entry = re.search(rb"(?:case\s+|\{\s*)0x([0-9a-f]{8})u", line)
                if match:
                    context = region(cpu, int(match[1], 16))
                category = region(cpu, int(entry[1], 16)) if entry else context
                contribution[category] += len(line)
    return dict(contribution)


def report(args) -> dict:
    profile = read_profile(args.profile)
    result = {"profile": str(args.profile), "profile_bytes": args.profile.stat().st_size,
              "regions": {}, "classification": "Descriptive only: padding is homogeneous-fill ranges; gfx-looking is zero-heavy 0x10000..0x8ffff, not a non-code proof."}
    source_bytes = Counter()
    for directory in (args.main_generated, args.sound_generated):
        inventory = json.loads((directory / "profile_inventory.json").read_text())
        cpu, crc = inventory["region"], int(inventory["rom_crc32"], 16)
        if profile.roms.get((cpu, crc)) != (inventory["base"], inventory["size"]):
            raise ValueError("profile and generated inventory ROM identities disagree")
        hot = {pc for (name, identity, pc), count in profile.hits.items() if name == cpu and identity == crc and count}
        for pc in inventory["entries"]:
            row = result["regions"].setdefault(region(cpu, pc), {"ever_executed": 0, "total_entries": 0, "retained_entries": 0})
            row["total_entries"] += 1
            row["ever_executed"] += int(pc in hot)
            row["retained_entries"] += int(inventory["retained_entries"] == len(inventory["entries"]) or pc in hot)
        source_bytes.update(source_contributions(directory, cpu))
    for name, row in result["regions"].items():
        row["coverage_percent"] = round(100 * row["ever_executed"] / row["total_entries"], 4)
        row["dispatch_table_bytes"] = row["retained_entries"] * 16
    result["generated_c_bytes"] = dict(source_bytes)
    result["generated_c_total"] = sum(source_bytes.values())
    if args.binary:
        text_size, address, size = macho_text(args.binary)
        result["binary"] = {"path": str(args.binary), "file_bytes": args.binary.stat().st_size,
                            "text_segment_bytes": text_size, "text_section_bytes": size,
                            "function_span_bytes_by_region": text_contributions(args.binary, address, size),
                            "attribution_limit": "Native function spans include alignment, page packing and compiler outlining; __TEXT non-code overhead is not attributed to guest regions."}
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    merge = sub.add_parser("merge", help="Union identities/addresses, sum hit and miss counts with uint64 saturation")
    merge.add_argument("--output", type=Path, required=True)
    merge.add_argument("inputs", type=Path, nargs="+")
    measure = sub.add_parser("report", help="Coverage and C bytes per descriptive ROM region; optional Mach-O sizes")
    measure.add_argument("profile", type=Path)
    measure.add_argument("--main-generated", type=Path, required=True)
    measure.add_argument("--sound-generated", type=Path, required=True)
    measure.add_argument("--binary", type=Path)
    measure.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "merge":
            combined = BlockProfile()
            paths = [path.resolve() for path in args.inputs]
            if len(set(paths)) != len(paths):
                raise ValueError("duplicate input paths would double-count one run")
            for path in paths:
                combined.merge(read_profile(path))
            combined.write(args.output)
            print(json.dumps({"output": str(args.output), "roms": len(combined.roms), "ever_executed": len(combined.hits), "cold_misses": len(combined.misses)}))
        else:
            data = json.dumps(report(args), indent=2) + "\n"
            if args.output:
                args.output.write_text(data)
            print(data, end="")
        return 0
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"block-profile: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
