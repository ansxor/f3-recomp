"""Compare two sorted decision TSVs and report unexplained cells by column.

expected.tsv is UTF-8 TSV with header pc, column, reason. Each record is an
exact hexadecimal PC or an inclusive 0xSTART-0xEND range, a comparison
column name, and a nonempty reason. The exception applies to any row kind
at those PCs and any pair of values in the specified column.

PC is the address key. Sprite rows have a separate key at the same PC for
each sprite kind. An added or removed row is a disposition difference.
--columns selects which differences to report. With --rom-image, compare
baseline decoder entries and check for missing ordinary rows when disposition
is selected. This mode omits sprite metadata. Pass --config with the ROM image
to enable discovery and CCR evidence rules. For slim dumps,
pass --after-baseline with the unpruned OCaml dump for jump-table scan evidence.
The expected table selects rules with rule:NAME in the pc field.
Exit status is 0 for no unexplained differences,
1 for unexplained differences, or 2 for invalid input.
"""

from __future__ import annotations

import argparse
import csv
import re
import shutil
import sys
import tomllib
from collections import Counter, defaultdict
from contextlib import ExitStack
from pathlib import Path
from tempfile import SpooledTemporaryFile

from tools.recomp_compare.decisions import COLUMNS, SPRITE_KINDS, pc_text
from tools.recomp_compare.decoder import category
from tools.recomp_compare.discovery import DiscoveryRules
from tools.recomp_compare.flag_analysis import Replay, effect_category

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
    rules = {}
    for line, (address, column, reason) in read_tsv(path, EXPECTED_HEADER):
        if address.startswith("rule:") and column in (*COLUMNS[1:], "entry") and reason.strip():
            rules[address.removeprefix("rule:"), column] = reason
            continue
        match = PC_RANGE.fullmatch(address)
        if not match or column not in COLUMNS[1:] or not reason.strip():
            raise ValueError(f"{path}:{line}: invalid expected difference")
        start = int(match[1], 16)
        end = int(match[2] or match[1], 16)
        if end < start:
            raise ValueError(f"{path}:{line}: PC range is reversed")
        ranges[column].append((start, end))
    return ranges, rules


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


def changes(before, after, columns, *, decoder=False):
    left = next(before, None)
    right = next(after, None)
    while left is not None or right is not None:
        if right is None or (left is not None and left[0] < right[0]):
            if decoder:
                yield left[0], "entry", left[1], None
            elif "disposition" in columns:
                yield left[0], "disposition", left[1], None
            left = next(before, None)
        elif left is None or right[0] < left[0]:
            if decoder:
                yield right[0], "entry", None, right[1]
            elif "disposition" in columns:
                yield right[0], "disposition", None, right[1]
            right = next(after, None)
        else:
            for index, column in enumerate(COLUMNS[1:], 1):
                if column in columns and left[1][index] != right[1][index]:
                    yield left[0], column, left[1], right[1]
            left = next(before, None)
            right = next(after, None)


def cell(row, column):
    if row is None:
        return "<missing>"
    if column == "entry":
        return ", ".join(row[2:4])
    return row[COLUMNS.index(column)]


def ordinary(path):
    return (record for record in decision_records(path) if record[0][1] == 0)


def main() -> int:
    parser = argparse.ArgumentParser(description="Group unexplained decision TSV differences by column.")
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--columns", default=",".join(COLUMNS[1:]), help="comma-separated comparison columns")
    parser.add_argument("--expected", type=Path, default=EXPECTED_PATH, help="expected differences TSV")
    parser.add_argument("--rom-image", type=Path, help="main ROM image for baseline decoder rules")
    parser.add_argument("--config", type=Path, help="game config for discovery rules and RAM payload bytes")
    parser.add_argument("--after-baseline", type=Path, help="unpruned OCaml decisions for validating slim jump-table scans")
    args = parser.parse_args()
    if args.config and not args.rom_image:
        parser.error("--config requires --rom-image")
    if args.after_baseline and not args.config:
        parser.error("--after-baseline requires --config")
    columns = {column.strip() for column in args.columns.split(",")}
    if unknown := columns.difference(COLUMNS[1:]):
        parser.error(f"unknown comparison columns: {', '.join(sorted(unknown))}")
    try:
        expected, rules = read_expected(args.expected)
        rom = args.rom_image.read_bytes() if args.rom_image else None
        records = ordinary if rom is not None else decision_records
        left, right = records(args.before), records(args.after)
        discovery = None
        if args.config:
            config = tomllib.loads(args.config.read_text(encoding="utf-8"))
            after_baseline = ordinary(args.after_baseline) if args.after_baseline else None
            discovery = DiscoveryRules(
                rom, config, ordinary(args.before), ordinary(args.after), after_baseline=after_baseline
            )
        replay = None
        if discovery is not None and "live_out" in columns:
            replay = (
                Replay(discovery, config, decision_records(args.before), frozen=True),
                Replay(discovery, config, decision_records(args.after), frozen=False),
            )
        counts = Counter()
        samples = {}
        with ExitStack() as stack:
            reports = {}
            entries = rom is not None and "disposition" in columns
            for (pc, kind), column, old, new in changes(left, right, columns, decoder=entries):
                before, after = cell(old, column), cell(new, column)
                if any(start <= pc <= end for start, end in expected[column]):
                    continue
                if rom is not None and not kind:
                    rule = category(rom, pc, column, before, after)
                    if rule is None and discovery is not None:
                        rule = discovery.category(pc, column, before, after, old, new)
                    if rule is None and discovery is not None:
                        if column in ("reads", "writes"):
                            rule = effect_category(discovery, pc, after, old, new)
                        elif column == "live_out":
                            if new[1].startswith(("exception:", "ram-exception:")):
                                rule = effect_category(discovery, pc, after, old, new)
                            elif replay and all(
                                graph.matches(pc, flags)
                                for graph, flags in zip(replay, (before, after), strict=True)
                            ):
                                rule = "flag-cfg-replay"
                    if (rule, column) in rules:
                        counts[rule] += 1
                        samples.setdefault(rule, (pc, before, after))
                        continue
                if column not in reports:
                    reports[column] = stack.enter_context(
                        SpooledTemporaryFile(max_size=1 << 20, mode="w+t", encoding="utf-8")
                    )
                row_kind = SPRITE_KINDS[kind - 1] if kind else "entry"
                reports[column].write(f"{pc_text(pc)}\t{row_kind}\t{before}\t{after}\n")
            if counts:
                print("[expected decision differences]" if discovery else "[expected decoder differences]")
                for rule, count in sorted(counts.items()):
                    pc, before, after = samples[rule]
                    window = (discovery.window(pc) if discovery else rom[pc : pc + 12]).hex(" ")
                    print(f"{rule}\t{count}\t{pc_text(pc)}\t{window}\t{before}\t{after}")
            for column in (*COLUMNS, "entry"):
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
