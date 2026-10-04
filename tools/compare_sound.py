#!/usr/bin/env python3
"""Compare complete sound bus traces, including timing, reads and driver metadata.

No lag fitting, event sorting, write deduplication or timestamp tolerance. The
first difference is reported with preceding main commands. WAV equality is a
separate check: matching audible output alone cannot prove driver parity.
"""
import argparse
from collections import Counter, deque
from itertools import zip_longest
import json
from pathlib import Path

from decode_sound import CommandRing, records


FIELDS = ('tick', 'sample', 'pc', 'address', 'value', 'kind', 'width')


def describe(row):
    if row is None:
        return None
    result = dict(zip(FIELDS, row))
    for field in ('pc', 'address', 'value'):
        result[field] = f'0x{result[field]:x}'
    return result


def compare(oracle, model):
    commands = CommandRing()
    recent = deque(maxlen=8)
    counts = Counter()
    total = 0
    for expected, actual in zip_longest(records(oracle), records(model)):
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('oracle', type=Path)
    parser.add_argument('model', type=Path)
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    try:
        result = compare(args.oracle, args.model)
    except (ValueError, OSError) as error:
        result = dict(equal=False, error=str(error))
    text = json.dumps(result, indent=2) + '\n'
    if args.json:
        args.json.write_text(text)
    print(text, end='')
    return 0 if result['equal'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
