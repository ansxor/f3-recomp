"""Command-line ROM verification, discovery, and C generation."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('discover', 'emit'))
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--rom-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True,
                        help='ignored generated-output directory (e.g. build/landmakrj)')
    parser.add_argument('--max-block-instructions', type=int, default=32)
    profiles = parser.add_mutually_exclusive_group()
    profiles.add_argument('--profile-tiers', type=Path,
                          help='retain all entries, splitting hot and cold sources using this profile')
    profiles.add_argument('--profile-slim', type=Path,
                          help='retain only executed entries from this profile (strict sparse build)')
    args = parser.parse_args()
    try:
        from .discovery import load_rom, discover
        rom, config = load_rom(args.config, args.rom_dir)
        result = discover(rom, config)
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / 'coverage.json').write_text(json.dumps(result.report, indent=2) + '\n')
        print(json.dumps({"coverage": result.report["summary"],
                          "report": str(args.output / "coverage.json")}, indent=2))
        if args.command == 'emit':
            from .generate import generate
            report = generate(rom, result, args.output, config,
                              max_block_instructions=args.max_block_instructions,
                              profile_tiers=args.profile_tiers,
                              profile_slim=args.profile_slim)
            print(json.dumps({key: value for key, value in report.items()
                              if key not in ('fallback_pcs', 'source_files')}, indent=2))
        return 0
    except (OSError, ValueError, KeyError, ImportError) as error:
        print(f'f3-recomp: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
