"""Compare complete sound bus traces, including timing, reads and driver metadata."""
from __future__ import annotations

import argparse
from collections import Counter, deque
from itertools import zip_longest
import json
from pathlib import Path
import sys

from tools.decode.trace import CommandRing, records

FIELDS = ("tick", "sample", "pc", "address", "value", "kind", "width")


def describe(row):
    if row is None:
        return None
    result = dict(zip(FIELDS, row))
    for field in ("pc", "address", "value"):
        result[field] = f"0x{result[field]:x}"
    return result


def compare_traces(oracle_path: Path, model_path: Path) -> dict:
    commands = CommandRing()
    recent = deque(maxlen=8)
    counts = Counter()
    total = 0
    for expected, actual in zip_longest(records(oracle_path), records(model_path)):
        if expected != actual:
            return dict(equal=False, records_compared=total, oracle=describe(expected),
                        model=describe(actual), recent_commands=list(recent))
        tick, _, _, address, value, kind, _ = expected
        counts[kind] += 1
        if kind == 1:
            for command in commands.main_write(address, value):
                recent.append(dict(tick=tick, **command))
        total += 1
    return dict(equal=True, records_compared=total, record_kinds=dict(sorted(counts.items())))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="f3 compare trace",
        description="Compare complete sound bus traces, including timing, reads and driver metadata."
    )
    parser.add_argument("oracle", type=Path, help="Oracle sound trace (.sound-trace)")
    parser.add_argument("model", type=Path, help="Model sound trace (.sound-trace)")
    parser.add_argument("--json", nargs="?", const=True, default=False,
                        help="Output results in JSON format (optional path to save)")
    args = parser.parse_args(argv)
    try:
        result = compare_traces(args.oracle, args.model)
    except (ValueError, OSError) as error:
        result = dict(equal=False, error=str(error))

    text = json.dumps(result, indent=2) + "\n"
    if isinstance(args.json, str):
        Path(args.json).write_text(text)
    if args.json:
        print(text, end="")
    else:
        print(text, end="")
    return 0 if result.get("equal") else 1


if __name__ == "__main__":
    sys.exit(main())
