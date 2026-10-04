"""Export CPU (not game) timing metadata from the pinned generated Musashi table.

Usage: python3 tools/differential/export_cycles.py build/differential/musashi/m68kops.c
"""
from pathlib import Path
import argparse
import re


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generated_ops", type=Path)
    parser.add_argument("--output", type=Path,
                        default=Path(__file__).resolve().parents[2] / "recomp/68020_cycles.csv")
    args = parser.parse_args()
    rows = re.findall(
        r"\{m68k_op_\w+\s*,\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*\{\s*\d+,\s*\d+,\s*(\d+),",
        args.generated_ops.read_text())
    if not rows:
        parser.error("No Musashi opcode descriptors found")
    header = (
        "# Musashi 68020 base-cycle descriptors, mask,match,cycles.\n"
        "# Generated from m68kmake / upstream 313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd.\n"
        "# Copyright 1998-2001 Karl Stenerud, MIT; see runtime/third_party/musashi/readme.txt.\n")
    args.output.write_text(header + ''.join(f"{mask[2:]},{match[2:]},{cycles}\n" for mask, match, cycles in rows))
    print(f"Exported {len(rows)} 68020 timing descriptors to {args.output}")


if __name__ == "__main__":
    main()
