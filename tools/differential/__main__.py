"""CLI entry point for 68EC020 differential test harness."""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

from .runner import run_differential


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Instruction-level differential self-test using Musashi 68EC020 reference core."
    )
    parser.add_argument(
        "--musashi",
        type=Path,
        default=None,
        help="Path to Musashi source directory containing m68k.h (searched if omitted)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("build/differential"),
        help="Output directory for generated test C and binaries (default: build/differential)",
    )
    parser.add_argument(
        "--cases",
        type=int,
        default=500,
        help="Number of test cases to generate (default: 500)",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="Deterministic PRNG seed for reproducible test cases (default: 42)",
    )
    parser.add_argument(
        "--instructions",
        type=Path,
        default=None,
        help="Optional path to external instructions JSON from discovery [{'pc': ..., 'bytes': ...}]",
    )
    parser.add_argument(
        "--filter",
        type=str,
        default=None,
        help="Optional filter substring for instruction mnemonic (e.g. 'add', 'move')",
    )
    parser.add_argument(
        "--compile-only",
        action="store_true",
        help="Only generate and compile the differential runner binary, do not execute",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Enable verbose output",
    )

    args = parser.parse_args()

    try:
        return run_differential(
            musashi_path=args.musashi,
            output_dir=args.output,
            cases_count=args.cases,
            seed=args.seed,
            instructions_json=args.instructions,
            filter_pattern=args.filter,
            compile_only=args.compile_only,
            verbose=args.verbose,
        )
    except (FileNotFoundError, ValueError, ImportError) as err:
        print(f"differential: error: {err}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
