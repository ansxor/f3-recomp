"""Dump frozen Python recompiler decisions as UTF-8, LF-terminated TSV.

The header is COLUMNS in the declared order. Rows sort by numeric PC, then
ordinary entry, sprite-writer, sprite-reset. There is at most one ordinary
row and one row of each sprite kind per PC. "-" marks an
empty set/list or a field that does not apply.

pc uses lowercase hexadecimal with 0x and at least six digits, zero padded.
disposition is native, fallback, ram, ram-fallback, exception:<decimal vector>,
  sprite-writer, or sprite-reset. Tiers retain the baseline entries and do
  not encode hot/cold placement here. Slim omits cold ROM entries.
length is the decimal Capstone instruction size in bytes, or "-" for exceptions
  and sprite rows.
flow is "next" for pure fallthrough. Otherwise use "branch:" followed by
  comma-separated explicit target PCs in numeric order without duplicates,
  then "next" if there is fallthrough, then "escape" if control can leave the
  known graph. A sole escape is "escape". Exceptions use "escape".
  On sprite rows this field carries semicolon-separated distinct records
  sorted lexicographically, with comma-separated fields and no whitespace.
  writer = cursor_reg,base,lane,x_offset,y_offset,group_class,rigid,roots
  reset = base,offset
  All sprite numbers are decimal (rigid is 0 or 1). roots is "-" or a
  plus-separated list of base:offset pairs sorted numerically by base and
  then offset, with no duplicates. Register indices 0-7 are D0-D7 and
  8-15 are A0-A7. Lane is the trace bitmask (1=X, 2=Y, 3=XY).
  x_offset or y_offset is 65535 when its lane is absent.
reads, writes, and live_out are ordered subsets of XNZVC with no separator,
  or "-". reads and writes are raw flag_effects (before pinning). live_out uses the
  emitter's retained graph and pinned PCs. All three are "-" on exception
  and sprite rows.
idle is spin, poll, affine, or "-".
chain_candidates is a list of comma-separated target PCs, ascending and unique, or "-".
  Includes eligible static successors and the first 256 eligible indirect
  targets in numeric order. Eligible targets are retained decoded ROM
  entries below 0x200000. Fallback and bodies ending in return have no chains.

Sprite metadata comes from the full ROM trace even in slim mode.
"""

from __future__ import annotations

import argparse
import sys
from collections import ChainMap, defaultdict
from pathlib import Path

from tools.recomp_py.block_profile import load_hot
from tools.recomp_py.discovery import discover, load_rom, parse_exclusions
from tools.recomp_py.emitter import CCR_C, CCR_N, CCR_V, CCR_X, CCR_Z, lower, static_flow
from tools.recomp_py.idle import find_idle_loops
from tools.recomp_py.liveness import analyse, flag_effects
from tools.recomp_py.opcodes import LEGAL_68020
from tools.recomp_py.ram_code import decode_ram_routines
from tools.recomp_py.sprite_trace import load_writer_seeds, trace
from tools.recomp_py.sprite_units import parse_sprite_units

COLUMNS = ("pc", "disposition", "length", "flow", "reads", "writes", "live_out", "idle", "chain_candidates")
SPRITE_KINDS = ("sprite-writer", "sprite-reset")


def pc_text(pc: int) -> str:
    return f"0x{pc:06x}"


def flags_text(mask: int) -> str:
    return (
        "".join(name for name, bit in zip("XNZVC", (CCR_X, CCR_N, CCR_Z, CCR_V, CCR_C), strict=True) if mask & bit)
        or "-"
    )


def flow_text(flow) -> str:
    parts = [pc_text(target) for target in sorted(set(flow.targets))]
    if flow.falls_through:
        parts.append("next")
    if flow.escapes:
        parts.append("escape")
    if flow.targets or len(parts) > 1:
        return "branch:" + ",".join(parts)
    return parts[0] if parts else "-"


def rom_exceptions(rom, discovery, config):
    # Frozen generate() keeps exception selection inline.
    if config.get("discovery", {}).get("coverage") != "all_aligned":
        return {}
    entries = {}
    for pc in discovery.invalid_pcs:
        opcode = int.from_bytes(rom[pc : pc + 2], "big")
        if opcode >> 12 in (10, 15):
            entries[pc] = 10 if opcode >> 12 == 10 else 11
        elif not LEGAL_68020[opcode] and opcode != 0x4E70:
            entries[pc] = 4
    return entries


def sprite_rows(groups):
    writers = defaultdict(set)
    resets = defaultdict(set)
    for writer in groups.writers:
        roots = sorted({(field.base, field.offset) for field in writer.root_offsets})
        root_text = "+".join(f"{base}:{offset}" for base, offset in roots) or "-"
        record = (
            writer.cursor_reg,
            writer.base,
            writer.lane,
            writer.x_offset,
            writer.y_offset,
            writer.group_class,
            int(writer.rigid),
            root_text,
        )
        writers[writer.pc].add(",".join(map(str, record)))
    for reset in groups.resets:
        resets[reset.pc].add(f"{reset.base},{reset.offset}")
    return writers, resets


def decision_rows(rom, config, discovery, *, profile_path=None, slim=False, seeds=(), lanes=None):
    decoded = discovery.instructions
    exceptions = rom_exceptions(rom, discovery, config)
    entries = decoded.keys() | exceptions.keys()
    if profile_path is not None:
        exclusions = parse_exclusions(config, len(rom), image=rom)
        hot = load_hot(profile_path, "main", rom, exclusions=exclusions) & entries
        if not hot:
            raise ValueError("profile contains no matching main executable hits")
        if slim:
            entries = hot

    units = parse_sprite_units(config)
    unit_enter, unit_exit = units.hook_pcs()
    for unit in units.units:
        for kind, pcs in (("start", unit.starts), ("end", unit.ends)):
            for pc in pcs:
                if pc not in decoded:
                    raise ValueError(f"Emit unit {unit.name!r} {kind} {pc:#x} is not a decoded instruction")
                if pc not in entries:
                    raise ValueError(
                        f"Emit unit {unit.name!r} {kind} {pc:#x} is not retained by the "
                        "slim profile; add it to the profile or use tiers"
                    )

    ram = {pc: insn for routine in decode_ram_routines(rom, config) for pc, insn in routine.instructions.items()}
    instructions = ChainMap(decoded, ram)
    nodes = (decoded.keys() & entries) | ram.keys()
    if not entries and not nodes:
        raise ValueError("discovery produced no instructions")
    fallback = {pc for pc in nodes if lower(instructions[pc]) is None}
    groups = trace(rom, discovery, config, seeds, lanes)
    writers, resets = sprite_rows(groups)
    pinned = fallback | unit_enter.keys() | unit_exit.keys() | resets.keys() | ram.keys()
    liveness = analyse(instructions, nodes, pinned=pinned)
    idle = find_idle_loops(decoded, lambda pc: pc in nodes and pc not in pinned)

    for pc in sorted(entries | ram.keys() | writers.keys() | resets.keys()):
        address = pc_text(pc)
        if pc in nodes:
            insn = instructions[pc]
            flow = static_flow(insn)
            effect = liveness.effects[pc]
            if pc in pinned:
                effect = flag_effects(insn)
            disposition = "fallback" if pc in fallback else "native"
            if pc in ram:
                disposition = "ram-fallback" if pc in fallback else "ram"
            candidates = set()
            body = None if pc in fallback else lower(insn, liveness.live_out[pc])
            # Frozen generate() assembles chains inline after lowering.
            if body and body[-1].strip() != "return;":
                static = set(flow.targets)
                if flow.falls_through:
                    static.add(pc + insn.size)
                candidates = {target for target in static if target in nodes and target < 0x200000}
                indirect = {
                    target for target in discovery.indirect_targets.get(pc, ()) if target in nodes and target < 0x200000
                }
                candidates.update(sorted(indirect)[:256])
            yield (
                address,
                disposition,
                str(insn.size),
                flow_text(flow),
                flags_text(effect.reads),
                flags_text(effect.writes),
                flags_text(liveness.live_out[pc]),
                idle.get(pc, ("-",))[0],
                ",".join(map(pc_text, sorted(candidates))) or "-",
            )
        elif pc in entries:
            yield address, f"exception:{exceptions[pc]}", "-", "escape", "-", "-", "-", "-", "-"
        for kind, records in zip(SPRITE_KINDS, (writers, resets), strict=True):
            if pc in records:
                yield address, kind, "-", ";".join(sorted(records[pc])), "-", "-", "-", "-", "-"


def main() -> int:
    parser = argparse.ArgumentParser(description="Dump the frozen Python recompiler's decision table.")
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--rom-dir", type=Path, required=True)
    profiles = parser.add_mutually_exclusive_group()
    profiles.add_argument("--profile-tiers", type=Path)
    profiles.add_argument("--profile-slim", type=Path)
    parser.add_argument("--sprite-writers", type=Path, help="extra writer seeds in the frozen emitter's JSON format")
    args = parser.parse_args()
    try:
        rom, config = load_rom(args.config, args.rom_dir)
        discovery = discover(rom, config)
        seeds, lanes = load_writer_seeds(args.sprite_writers) if args.sprite_writers else ((), {})
        rows = decision_rows(
            rom,
            config,
            discovery,
            profile_path=args.profile_tiers or args.profile_slim,
            slim=args.profile_slim is not None,
            seeds=seeds,
            lanes=lanes,
        )
        sys.stdout.reconfigure(encoding="utf-8", newline="\n")
        print("\t".join(COLUMNS))
        for row in rows:
            print("\t".join(row))
    except (OSError, ValueError, KeyError) as error:
        print(f"decisions: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
