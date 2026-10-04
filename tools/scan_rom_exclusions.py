#!/usr/bin/env python3
"""Report conservative CPU-ROM exclusion proposals; never modify game configs.

All addresses are guest addresses and intervals are half-open. Entropy and an
all-aligned decode are statistical/syntactic observations, not reachability
proofs. JSON preserves rejected candidates so evidence conflicts remain visible.
"""
from __future__ import annotations

import argparse
from collections import Counter, deque
from copy import deepcopy
import json
import math
from pathlib import Path
import re
import sys
import tomllib

ROOT_DIR = Path(__file__).resolve().parents[1]
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from recomp.discovery import (
    CALL_MNEMONICS, COND_BRANCH_MNEMONICS, TERMINAL_MNEMONICS,
    UNCOND_BRANCH_MNEMONICS, _parse_int_address, _resolve_target,
    capstone, discover, load_rom,
)

MIN_FILL = 4096
WINDOW = 4096


def overlaps(a: int, b: int, c: int, d: int) -> bool:
    return a < d and c < b


def interval(value: object) -> tuple[int, int]:
    if not isinstance(value, (list, tuple)) or len(value) != 2:
        raise ValueError(f"Expected [start,end] interval, got {value!r}")
    start, end = (_parse_int_address(x) for x in value)
    if not 0 <= start < end <= 0x100000000:
        raise ValueError(f"Invalid half-open guest interval: {value!r}")
    return start, end


def profile_evidence(profile: dict, cpu: str) -> tuple[list, list, list]:
    section = profile.get(cpu, {})
    if not isinstance(section, dict):
        raise ValueError(f"Profile {cpu} section must be an object")
    fetched = [interval(v) for v in section.get("fetched", [])]
    reads = [interval(v) for v in section.get("data_reads", [])]
    known = []
    for record in section.get("known_data", []):
        start, end = interval([record["start"], record["end"]])
        for name in ("reason", "evidence"):
            if not isinstance(record.get(name), str) or not record[name].strip():
                raise ValueError(f"known_data requires nonempty {name}")
        known.append({**record, "start": start, "end": end})
    return fetched, reads, known


def covers(start: int, end: int, ranges: list[tuple[int, int]]) -> bool:
    cursor = start
    for low, high in sorted(ranges):
        if high <= cursor:
            continue
        if low > cursor:
            return False
        cursor = max(cursor, high)
        if cursor >= end:
            return True
    return False


def statistics(data: bytes) -> dict:
    counts = Counter(data)
    size = len(data)
    return {
        "bytes": size,
        "entropy_bits_per_byte": round(-sum((n / size) * math.log2(n / size)
                                             for n in counts.values()), 6),
        "distinct_bytes": len(counts),
        "dominant_byte_fraction": round(max(counts.values()) / size, 6),
    }


def candidate_ranges(rom: bytes, base: int) -> list[dict]:
    """Exact fills, exact short low-entropy periods, and entropy windows.

    Trim inward to word boundaries, never round into non-fill/checksum bytes.
    Only exact periodicity qualifies; arbitrary low entropy does not.
    """
    candidates = []
    occupied = []
    for match in re.finditer(rb"\x00{4096,}|\xff{4096,}", rom):
        low, high = match.span()
        low = (low + 1) & ~1
        high &= ~1
        if high - low < MIN_FILL:
            continue
        candidates.append({"start": base + low, "end": base + high,
                           "kind": "constant_fill", "pattern": rom[low:low + 1].hex()})
        occupied.append((low, high))
    # Exact two/four/eight-byte periods made only of mixed 00/FF bytes.
    # Do not classify repeated NOPs/other instruction patterns as padding.
    pc = 0
    while pc + MIN_FILL <= len(rom):
        containing = next((end for start, end in occupied if start <= pc < end), None)
        if containing is not None:
            pc = containing
            continue
        found = False
        for period in (2, 4, 8):
            pattern = rom[pc:pc + period]
            if set(pattern) != {0x00, 0xff}:
                continue
            if rom[pc:pc + MIN_FILL] != pattern * (MIN_FILL // period):
                continue
            end = pc + MIN_FILL
            while end + period <= len(rom) and rom[end:end + period] == pattern:
                end += period
            # Retain a final complete word even if the last period is partial.
            while end + 2 <= len(rom) and rom[end:end + 2] == pattern[(end - pc) % period:(end - pc) % period + 2]:
                end += 2
            candidates.append({"start": base + pc, "end": base + end,
                               "kind": "periodic_fill", "pattern": pattern.hex()})
            occupied.append((pc, end))
            pc = end
            found = True
            break
        if not found:
            pc += 2
    for low in range(0, len(rom) - WINDOW + 1, WINDOW):
        high = low + WINDOW
        if any(overlaps(low, high, a, b) for a, b in occupied):
            continue
        if statistics(rom[low:high])["entropy_bits_per_byte"] >= 7.5:
            candidates.append({"start": base + low, "end": base + high,
                               "kind": "high_entropy_window"})
    return sorted(candidates, key=lambda c: (c["start"], c["end"]))


def declared_roots(rom: bytes, config: dict, cpu: str, base: int) -> dict[int, set[str]]:
    roots: dict[int, set[str]] = {}

    def add(value: int | str, source: str) -> None:
        address = _parse_int_address(value)
        if base <= address < base + len(rom):
            if address & 1:
                raise ValueError(f"Unaligned {source}: {address:#x}")
            roots.setdefault(address, set()).add(source)

    # Both CPUs expose 256 four-byte vectors. The initial stack pointer is
    # not an executable entry.
    for offset in range(4, min(1024, len(rom)), 4):
        value = int.from_bytes(rom[offset:offset + 4], "big")
        if not value & 1:
            add(value, f"vector[{offset // 4}]")
    spec = config.get("discovery", {}) if cpu == "main" else config.get("sound", {}).get("discovery", {})
    for value in spec.get("entry_points", []):
        add(value, "declared_entry")
    hooks = config.get("hooks", []) if cpu == "main" else config.get("sound", {}).get("hooks", [])
    for hook in hooks:
        if hook.get("cpu", cpu) == cpu and "address" in hook:
            add(hook["address"], "declared_hook")
    for table in spec.get("jump_tables", []) + spec.get("pointer_tables", []):
        for value in table.get("targets", []):
            add(value, "explicit_pointer_table_target")
        if "table" in table:
            address = _parse_int_address(table["table"])
            count = int(table["count"])
            offset = address - base
            if count < 0 or offset < 0 or offset + count * 4 > len(rom):
                raise ValueError(f"Explicit pointer table outside {cpu} ROM")
            for pos in range(offset, offset + count * 4, 4):
                value = int.from_bytes(rom[pos:pos + 4], "big")
                if not value & 1:
                    add(value, "explicit_pointer_table_target")
    return roots


def rooted_code(rom: bytes, config: dict, cpu: str, base: int,
                roots: dict[int, set[str]]) -> tuple[dict, list[dict], int]:
    if cpu == "main":
        recursive = deepcopy(config)
        # Scanner must assess declared exclusions, not trust them as evidence.
        recursive.pop("exclude", None)
        spec = recursive.setdefault("discovery", {})
        spec["coverage"] = "recursive"
        spec["scan_task_traps"] = False
        spec["scan_callbacks"] = False
        spec["entry_points"] = sorted(roots)
        instructions = discover(rom, recursive).instructions
    else:
        md = capstone.Cs(capstone.CS_ARCH_M68K,
                         capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_000)
        md.detail = True
        instructions = {}
        pending = deque(roots)
        while pending:
            pc = pending.popleft()
            while base <= pc < base + len(rom) and not pc & 1 and pc not in instructions:
                offset = pc - base
                insn = next(md.disasm(rom[offset:offset + 24].ljust(24, b"\0"), pc, count=1), None)
                if (insn is None or not insn.id or insn.mnemonic.startswith("dc")
                        or insn.size & 1 or offset + insn.size > len(rom)):
                    break
                instructions[pc] = insn
                mnemonic = insn.mnemonic.split(".")[0]
                if mnemonic in CALL_MNEMONICS | UNCOND_BRANCH_MNEMONICS | COND_BRANCH_MNEMONICS:
                    target = _resolve_target(insn, insn.operands[-1]) if insn.operands else None
                    if target is not None:
                        pending.append(target)
                if mnemonic in TERMINAL_MNEMONICS | UNCOND_BRANCH_MNEMONICS:
                    break
                pc += insn.size
    references = []
    unresolved = 0
    for pc, insn in instructions.items():
        mnemonic = insn.mnemonic.split(".")[0]
        if mnemonic in CALL_MNEMONICS | UNCOND_BRANCH_MNEMONICS | COND_BRANCH_MNEMONICS:
            target = _resolve_target(insn, insn.operands[-1]) if insn.operands else None
            if target is None:
                unresolved += 1
            elif base <= target < base + len(rom):
                references.append({"start": target, "end": target + 2,
                                   "source_pc": pc, "kind": "rooted_control_reference"})
    return instructions, references, unresolved


def scan_rom(rom: bytes, config: dict, cpu: str = "main", profile: dict | None = None) -> dict:
    if cpu not in ("main", "sound"):
        raise ValueError("CPU must be main or sound")
    base = 0 if cpu == "main" else 0xc00000
    if len(rom) & 1 or len(rom) < 1024:
        raise ValueError("CPU ROM must be even and contain a complete vector table")
    profile = profile or {}
    fetched, reads, known = profile_evidence(profile, cpu)
    roots = declared_roots(rom, config, cpu, base)
    instructions, references, unresolved = rooted_code(rom, config, cpu, base, roots)
    blockers = [{"start": pc, "end": pc + 2, "kind": source}
                for pc, sources in roots.items() for source in sorted(sources)]
    blockers.extend({"start": pc, "end": pc + insn.size, "kind": "rooted_recursive_instruction"}
                    for pc, insn in instructions.items())
    blockers.extend(references)
    blockers.extend({"start": a, "end": b, "kind": "observed_instruction_fetch"} for a, b in fetched)
    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN |
                     (capstone.CS_MODE_M68K_020 if cpu == "main" else capstone.CS_MODE_M68K_000))
    candidates = []
    ranges = candidate_ranges(rom, base)
    for record in known:
        start = (max(base, record["start"]) + 1) & ~1
        end = min(base + len(rom), record["end"]) & ~1
        if start < end and not any(c["start"] == start and c["end"] == end for c in ranges):
            ranges.append({"start": start, "end": end, "kind": "known_data"})
    for candidate in ranges:
        start, end = candidate["start"], candidate["end"]
        conflicts = [b for b in blockers if overlaps(start, end, b["start"], b["end"])]
        read_intersections = [[max(start, a), min(end, b)] for a, b in reads if overlaps(start, end, a, b)]
        known_intersections = [k for k in known if overlaps(start, end, k["start"], k["end"])]
        explicit = covers(start, end, [(k["start"], k["end"]) for k in known_intersections])
        kind = candidate["kind"]
        fill = kind in ("constant_fill", "periodic_fill")
        safe = not conflicts and (fill or explicit)
        strength = "explicit_known_data" if explicit else "structural_fill" if fill else "statistical_only"
        if read_intersections and not explicit:
            strength += "+observed_data_reads"
        evidence = [f"Loaded CPU ROM; {kind}; half-open word-aligned interval.",
                    "No rooted code or fetch conflict found." if not conflicts else "CONFLICT: rooted code/entry/reference or observed instruction fetch overlaps.",
                    "Entropy and all-aligned decodes are not reachability proof; unobserved bytes are not proven data."]
        if fill:
            evidence.append(f"Exact repeated byte pattern {candidate['pattern']}; nonmatching boundary bytes preserved.")
        if read_intersections:
            evidence.append("Observed data reads overlap only the reported subintervals; observations do not prove the rest of this range is data.")
        evidence.extend(f"Known data [{k['start']:#x},{k['end']:#x}): {k['reason']}; {k['evidence']}" for k in known_intersections)
        # Bounded evenly spaced aligned samples: no exhaustive-decoding claim.
        step = max(2, ((end - start) // 128) & ~1)
        sampled = decoded = 0
        for pc in range(start, end, step):
            offset = pc - base
            insn = next(md.disasm(rom[offset:offset + 24].ljust(24, b"\0"), pc, count=1), None)
            sampled += 1
            decoded += bool(insn is not None and insn.id and not insn.mnemonic.startswith("dc")
                            and not insn.size & 1 and pc + insn.size <= base + len(rom))
        candidates.append({**candidate, "cpu": cpu,
                           "reason": ("Exact ROM padding/fill" if fill else
                                      "Known runtime data interval" if kind == "known_data" else
                                      "Statistical high-entropy data candidate"),
                           "evidence": " ".join(evidence),
                           "statistics": statistics(rom[start - base:end - base]),
                           "syntactic_candidates": {"aligned_starts_sampled": sampled, "decodable_samples": decoded,
                                                    "interpretation": "Syntactic candidates only, not evidence of reachability"},
                           "blockers": conflicts, "evidence_strength": strength,
                           "data_read_intersections": read_intersections,
                           "known_data_intersections": known_intersections,
                           "safe_to_propose": safe,
                           "status": "conflicting" if conflicts else "safe-to-propose" if safe else "rejected",
                           "rejection_reason": None if safe else "code_or_fetch_conflict" if conflicts else "statistics_without_explicit_data_evidence"})
    return {"schema_version": 1, "game": config.get("game", {}).get("id", "unknown"),
            "cpu": cpu, "rom_start": base, "rom_end": base + len(rom),
            "profile_runs": profile.get("runs", []),
            "rooted_instruction_count": len(instructions),
            "declared_targets": [{"address": pc, "sources": sorted(sources)} for pc, sources in sorted(roots.items())],
            "unresolved_rooted_transfers": unresolved, "candidates": candidates,
            "limitations": ["Only the selected CPU ROM is scanned; separate sample/tile ROMs are not decoded.",
                            "Periodic-fill detection is restricted to exact 2/4/8-byte mixed 00/FF patterns lasting at least 4096 bytes; repeated instruction patterns are not classified as fill.",
                            "Missing rooted references or profile fetches do not prove absence of computed jumps or unobserved execution.",
                            "Sound recursive traversal resolves direct transfers and declared pointer tables; unresolved indirect transfers are reported, not assumed covered.",
                            "Safe-to-propose means no detected conflict and structural fill or explicit known-data evidence; review is required before adding exclusions.",
                            "High entropy alone never proves data. Profiles cover only supplied runs and intervals."]}


def toml_proposals(report: dict) -> str:
    lines = ["# Unapplied proposals requiring review; scanner does not modify game configuration."]
    selected = []
    for candidate in sorted(report["candidates"], key=lambda c: (c["start"], -c["end"])):
        if not candidate["safe_to_propose"] or any(
                overlaps(candidate["start"], candidate["end"], start, end) for start, end in selected):
            continue
        selected.append((candidate["start"], candidate["end"]))
        lines.extend(["", "[[exclude]]", f'cpu = {json.dumps(candidate["cpu"])}',
                      f'start = 0x{candidate["start"]:x}', f'end = 0x{candidate["end"]:x}',
                      f'reason = {json.dumps(candidate["reason"], ensure_ascii=False)}',
                      f'evidence = {json.dumps(candidate["evidence"], ensure_ascii=False)}'])
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--rom-dir", type=Path, required=True)
    parser.add_argument("--cpu", choices=("main", "sound"), default="main")
    parser.add_argument("--output", type=Path, required=True, help="JSON evidence report")
    parser.add_argument("--toml", type=Path, help="Write safe-to-propose [[exclude]] records")
    parser.add_argument("--profile", type=Path, help="Guest-address fetch/data-read/known-data JSON")
    args = parser.parse_args()
    protected = {args.config.resolve()}
    if args.profile:
        protected.add(args.profile.resolve())
    if args.output.resolve() in protected:
        parser.error("--output must not overwrite input config/profile")
    if args.toml and args.toml.resolve() in protected | {args.output.resolve()}:
        parser.error("--toml must not overwrite inputs or the JSON report")
    if args.cpu == "main":
        rom, config = load_rom(args.config, args.rom_dir)
    else:
        config = tomllib.loads(args.config.read_text("utf-8"))
        if config.get("game", {}).get("id") != "landmakrj":
            parser.error("Sound ROM metadata is known only for landmakrj; refusing set substitution")
        from tools.compile_sound import load_sound_rom
        rom = load_sound_rom(args.rom_dir)
    profile = json.loads(args.profile.read_text("utf-8")) if args.profile else None
    report = scan_rom(rom, config, args.cpu, profile)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.toml:
        args.toml.parent.mkdir(parents=True, exist_ok=True)
        args.toml.write_text(toml_proposals(report), encoding="utf-8")


if __name__ == "__main__":
    main()
