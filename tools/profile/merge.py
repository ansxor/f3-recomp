"""Merge address-only execution profiles into one profile."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from recomp.block_profile import BlockProfile, read_profile


def merge_profiles(inputs: list[Path], output: Path) -> dict:
    combined = BlockProfile()
    paths = [path.resolve() for path in inputs]
    if len(set(paths)) != len(paths):
        raise ValueError("duplicate input paths would double-count one run")
    for path in paths:
        combined.merge(read_profile(path))
    output.parent.mkdir(parents=True, exist_ok=True)
    combined.write(output)
    return {
        "output": str(output),
        "roms": len(combined.roms),
        "ever_executed": len(combined.hits),
        "cold_misses": len(combined.misses),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="f3 profile merge",
        description="Union identities/addresses, sum hit and miss counts with uint64 saturation."
    )
    parser.add_argument("--output", type=Path, required=True, help="Output .profile path")
    parser.add_argument("inputs", type=Path, nargs="+", help="Input .profile paths to merge")
    args = parser.parse_args(argv)
    try:
        result = merge_profiles(args.inputs, args.output)
        print(json.dumps(result))
        return 0
    except Exception as err:
        print(f"f3 profile merge: error: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
