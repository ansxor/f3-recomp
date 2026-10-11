#!/usr/bin/env python3
"""
tools/mame/stage_roms.py - Stage and verify Land Maker (landmakrj) ROMs for MAME.

Features:
- Locates ROM files from directory or mislabeled parent ZIP (e.g. landmakr.zip).
- Verifies exact CRCs against MAME taito_f3.cpp definitions.
- Automatically handles short 128KB sound ROMs (e61-14.32 and e61-15.33),
  appending 0x20000 bytes of 0xFF padding to yield 256KB ROMs matching MAME's
  expected CRCs (18961bbb and 2c64557a).
- Packages into an untracked landmakrj.zip or stages to a directory for MAME.
"""

import argparse
import os
import struct
import sys
import zipfile
import zlib

EXPECTED_ROMS = {
    # 68020 program code
    "e61-13.20": {"crc": 0x0af756a2, "size": 0x80000, "desc": "68020 program byte 0"},
    "e61-12.19": {"crc": 0x636b3df9, "size": 0x80000, "desc": "68020 program byte 1"},
    "e61-11.18": {"crc": 0x279a0ee4, "size": 0x80000, "desc": "68020 program byte 2"},
    "e61-10.17": {"crc": 0xdaabf2b2, "size": 0x80000, "desc": "68020 program byte 3"},
    # Sprites
    "e61-03.12": {"crc": 0xe8abfc46, "size": 0x200000, "desc": "Sprites byte 0"},
    "e61-02.08": {"crc": 0x1dc4a164, "size": 0x200000, "desc": "Sprites byte 1"},
    "e61-01.04": {"crc": 0x6cdd8311, "size": 0x200000, "desc": "Sprites hi plane"},
    # Tilemap
    "e61-09.47": {"crc": 0x6ba29987, "size": 0x200000, "desc": "Tiles word 0"},
    "e61-08.45": {"crc": 0x76c98e14, "size": 0x200000, "desc": "Tiles word 1"},
    "e61-07.43": {"crc": 0x4a57965d, "size": 0x200000, "desc": "Tiles hi plane"},
    # Sound 68000 CPU (MAME expects 0x40000 bytes; short dumps are 0x20000 padded with FF)
    "e61-14.32": {
        "crc": 0x18961bbb,
        "size": 0x40000,
        "short_crc": 0xb905f4a7,
        "short_size": 0x20000,
        "desc": "Sound 68000 code byte 0",
    },
    "e61-15.33": {
        "crc": 0x2c64557a,
        "size": 0x40000,
        "short_crc": 0x87909869,
        "short_size": 0x20000,
        "desc": "Sound 68000 code byte 1",
    },
    # Ensoniq audio samples
    "e61-04.38": {"crc": 0xc27aec0c, "size": 0x200000, "desc": "Ensoniq samples bank 1"},
    "e61-05.39": {"crc": 0x83920d9d, "size": 0x200000, "desc": "Ensoniq samples bank 2"},
    "e61-06.40": {"crc": 0x2e717bfe, "size": 0x200000, "desc": "Ensoniq samples bank 3"},
    # Identical F3 motherboard PLDs are available in the supplied Puchi Car set.
    "pal16l8a-d77-09.ic14": {"crc": 0xb371532b, "size": 0x104, "desc": "F3 board PLD"},
    "pal16l8a-d77-10.ic28": {"crc": 0x42f59227, "size": 0x104, "desc": "F3 board PLD"},
    "palce16v8q-d77-11.ic37": {"crc": 0xeacc294e, "size": 0x117, "desc": "F3 board PLD"},
    "palce16v8q-d77-12.ic48": {"crc": 0xe9920cfe, "size": 0x117, "desc": "F3 board PLD"},
}

DEFAULT_SEARCH_PATHS = [
    "roms/landmakr",
    "roms/landmakrj",
    "../../roms/landmakr",
    "../../roms/landmakrj",
    "../roms/landmakr",
    "../roms/landmakrj",
]


def load_from_source(source_path):
    """Loads all ROM data from either a directory or a zip file."""
    roms = {}
    if os.path.isdir(source_path):
        for fname in os.listdir(source_path):
            fpath = os.path.join(source_path, fname)
            if os.path.isfile(fpath):
                # normalize name if it has suffix like .04, .17
                base = fname.split("/")[-1]
                with open(fpath, "rb") as f:
                    roms[base] = f.read()
    elif os.path.isfile(source_path) and zipfile.is_zipfile(source_path):
        with zipfile.ZipFile(source_path, "r") as z:
            for info in z.infolist():
                if not info.is_dir():
                    base = os.path.basename(info.filename)
                    roms[base] = z.read(info)
    return roms


def find_source(explicit_source=None):
    if explicit_source and os.path.exists(explicit_source):
        return explicit_source
    for p in DEFAULT_SEARCH_PATHS:
        if os.path.exists(p):
            return p
    return None


def match_rom(name, available_roms):
    """Finds exact or case-insensitive match for a ROM name."""
    if name in available_roms:
        return available_roms[name]
    for k, v in available_roms.items():
        if k.lower() == name.lower():
            return v
        # check without extension or prefix
        if k.startswith(name) or name.startswith(k):
            return v
    return None


def verify_and_prepare_roms(available_roms):
    """
    Verifies all expected ROMs and pads sound ROMs if necessary.
    Returns (prepared_dict, report_lines, all_ok).
    """
    prepared = {}
    reports = []
    all_ok = True

    for name, spec in EXPECTED_ROMS.items():
        data = match_rom(name, available_roms)
        if data is None:
            data = next((v for v in available_roms.values()
                         if len(v) == spec["size"] and zlib.crc32(v) == spec["crc"]), None)
        if data is None:
            reports.append(f"  [MISSING] {name:12s} ({spec['desc']})")
            all_ok = False
            continue

        crc = zlib.crc32(data) & 0xFFFFFFFF
        size = len(data)

        # Check if already padded / full
        if size == spec["size"] and crc == spec["crc"]:
            reports.append(f"  [OK]      {name:12s} len=0x{size:06x} crc={crc:08x} ({spec['desc']})")
            prepared[name] = data
            continue

        # Check if short sound rom needs FF padding
        if "short_size" in spec and size == spec["short_size"]:
            if crc == spec["short_crc"]:
                # Pad with 0x20000 bytes of 0xFF
                pad_len = spec["size"] - spec["short_size"]
                padded_data = data + (b"\xFF" * pad_len)
                padded_crc = zlib.crc32(padded_data) & 0xFFFFFFFF
                if padded_crc == spec["crc"]:
                    reports.append(
                        f"  [PADDED]  {name:12s} padded 0x{size:05x} -> 0x{len(padded_data):06x}, crc {crc:08x} -> {padded_crc:08x} [MATCH]"
                    )
                    prepared[name] = padded_data
                    continue
                else:
                    reports.append(
                        f"  [ERROR]   {name:12s} padded CRC {padded_crc:08x} != expected {spec['crc']:08x}"
                    )
                    all_ok = False
            else:
                reports.append(
                    f"  [ERROR]   {name:12s} short CRC {crc:08x} != expected {spec['short_crc']:08x}"
                )
                all_ok = False
        else:
            reports.append(
                f"  [MISMATCH]{name:12s} size 0x{size:x} (expected 0x{spec['size']:x}), crc {crc:08x} (expected {spec['crc']:08x})"
            )
            all_ok = False

    return prepared, reports, all_ok


def stage_to_zip(prepared_roms, zip_path):
    os.makedirs(os.path.dirname(os.path.abspath(zip_path)), exist_ok=True)
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for name in sorted(prepared_roms.keys()):
            z.writestr(name, prepared_roms[name])


def stage_to_dir(prepared_roms, dir_path):
    os.makedirs(dir_path, exist_ok=True)
    for name, data in prepared_roms.items():
        with open(os.path.join(dir_path, name), "wb") as f:
            f.write(data)


def main():
    parser = argparse.ArgumentParser(
        description="Verify and stage Land Maker (landmakrj) ROMs for MAME."
    )
    parser.add_argument("--source", help="Source ROMs directory or zip file")
    parser.add_argument("--board-source", default="roms/puchicarj",
                        help="Directory/ZIP with CRC-identical F3 motherboard PLDs (default: roms/puchicarj)")
    parser.add_argument(
        "--out-zip",
        default="tools/mame/staged_roms/landmakrj.zip",
        help="Path for output ZIP file (default: tools/mame/staged_roms/landmakrj.zip)",
    )
    parser.add_argument("--out-dir", help="Path for output directory (optional)")
    parser.add_argument(
        "--check-only",
        action="store_true",
        help="Verify CRCs and report status without writing files",
    )
    args = parser.parse_args()

    source = find_source(args.source)
    if not source:
        print(
            "Error: Could not locate landmakr ROM source directory or zip.",
            file=sys.stderr,
        )
        print("Checked paths:", file=sys.stderr)
        for p in DEFAULT_SEARCH_PATHS:
            print(f"  {p}", file=sys.stderr)
        sys.exit(1)

    print(f"Loading ROMs from source: {source}")
    available_roms = load_from_source(source)
    available_roms.update(load_from_source(args.board_source))
    print(f"Found {len(available_roms)} files.")

    prepared, reports, all_ok = verify_and_prepare_roms(available_roms)

    print("\nROM Verification Report:")
    for r in reports:
        print(r)

    if not all_ok:
        print("\n[FAIL] Some ROMs are missing or mismatched.", file=sys.stderr)
        sys.exit(1)

    print(f"\n[PASS] All {len(EXPECTED_ROMS)} ROM files verified successfully!")

    if args.check_only:
        sys.exit(0)

    # Write output
    if args.out_zip:
        stage_to_zip(prepared, args.out_zip)
        print(f"Wrote staged archive: {args.out_zip} ({os.path.getsize(args.out_zip):,} bytes)")

    if args.out_dir:
        stage_to_dir(prepared, args.out_dir)
        print(f"Wrote staged directory: {args.out_dir}")


if __name__ == "__main__":
    main()
