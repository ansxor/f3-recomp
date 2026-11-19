"""Compare two sorted decision TSVs and report unexplained cells by column.

expected.tsv is UTF-8 TSV with header pc, column, reason. Each record is an
exact hexadecimal PC or an inclusive 0xSTART-0xEND range, a comparison
column name, and a nonempty reason. The exception applies to any row kind
at those PCs and any pair of values in the specified column.

PC is the address key. Sprite rows have a separate key at the same PC for
each sprite kind. An added or removed row is a disposition difference.
--columns selects which differences to report. Exit status is 0 for no
unexplained differences, 1 for differences, or 2 for invalid input.
"""

from __future__ import annotations

import argparse
import csv
import re
import shutil
import sys
from collections import defaultdict
from contextlib import ExitStack
from pathlib import Path
from tempfile import SpooledTemporaryFile

from tools.recomp_compare.decisions import COLUMNS, SPRITE_KINDS, pc_text

EXPECTED_HEADER = ("pc", "column", "reason")
EXPECTED_PATH = Path(__file__).with_name("expected.tsv")
PC_RANGE = re.compile(r"(0x[0-9a-fA-F]{1,8})(?:-(0x[0-9a-fA-F]{1,8}))?")
ROW_KINDS = {kind: index for index, kind in enumerate(SPRITE_KINDS, 1)}


def read_tsv(path, header):
    with path.open(encoding="utf-8", newline="") as stream:
        rows = csv.reader(stream, delimiter="\t", quoting=csv.QUOTE_NONE)
        if next(rows, None) != list(header):
            raise ValueError(f"{path}: expected header {' '.join(header)}")
        for line, row in enumerate(rows, 2):
            if len(row) != len(header) or not all(row):
                raise ValueError(f"{path}:{line}: expected {len(header)} nonempty fields")
            yield line, row


def read_expected(path):
    ranges = defaultdict(list)
    for line, (address, column, reason) in read_tsv(path, EXPECTED_HEADER):
        match = PC_RANGE.fullmatch(address)
        if not match or column not in COLUMNS[1:] or not reason.strip():
            raise ValueError(f"{path}:{line}: invalid expected difference")
        start = int(match[1], 16)
        end = int(match[2] or match[1], 16)
        if end < start:
            raise ValueError(f"{path}:{line}: PC range is reversed")
        ranges[column].append((start, end))
    return ranges


def decision_records(path):
    previous = (-1, -1)
    for line, row in read_tsv(path, COLUMNS):
        pc = int(row[0], 16)
        if not 0 <= pc <= 0xFFFFFFFF or row[0] != pc_text(pc):
            raise ValueError(f"{path}:{line}: invalid PC {row[0]!r}")
        key = (pc, ROW_KINDS.get(row[1], 0))
        if key <= previous:
            raise ValueError(f"{path}:{line}: duplicate or out-of-order row")
        yield key, row
        previous = key


def changes(before, after, columns):
    left = next(before, None)
    right = next(after, None)
    while left is not None or right is not None:
        if right is None or (left is not None and left[0] < right[0]):
            if "disposition" in columns:
                yield left[0], "disposition", left[1][1], "<missing>"
            left = next(before, None)
        elif left is None or right[0] < left[0]:
            if "disposition" in columns:
                yield right[0], "disposition", "<missing>", right[1][1]
            right = next(after, None)
        else:
            for index, column in enumerate(COLUMNS[1:], 1):
                if column in columns and left[1][index] != right[1][index]:
                    yield left[0], column, left[1][index], right[1][index]
            left = next(before, None)
            right = next(after, None)


def main() -> int:
    parser = argparse.ArgumentParser(description="Group unexplained decision TSV differences by column.")
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--columns", default=",".join(COLUMNS[1:]), help="comma-separated comparison columns")
    parser.add_argument("--expected", type=Path, default=EXPECTED_PATH, help="expected differences TSV")
    args = parser.parse_args()
    columns = {column.strip() for column in args.columns.split(",")}
    if unknown := columns.difference(COLUMNS[1:]):
        parser.error(f"unknown comparison columns: {', '.join(sorted(unknown))}")
    try:
        expected = read_expected(args.expected)
        with ExitStack() as stack:
            reports = {}
            for (pc, kind), column, before, after in changes(
                decision_records(args.before), decision_records(args.after), columns
            ):
                if any(start <= pc <= end for start, end in expected[column]):
                    continue
                if column not in reports:
                    reports[column] = stack.enter_context(
                        SpooledTemporaryFile(max_size=1 << 20, mode="w+t", encoding="utf-8")
                    )
                row_kind = SPRITE_KINDS[kind - 1] if kind else "entry"
                reports[column].write(f"{pc_text(pc)}\t{row_kind}\t{before}\t{after}\n")
            for column in COLUMNS:
                if column in reports:
                    print(f"[{column}]")
                    reports[column].seek(0)
                    shutil.copyfileobj(reports[column], sys.stdout)
            return int(bool(reports))
    except (OSError, ValueError, csv.Error) as error:
        print(f"diff: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
