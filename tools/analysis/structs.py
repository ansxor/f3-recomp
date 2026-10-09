"""Structure field analysis: lists every field accessed through an address register."""
from __future__ import annotations

import re
from dataclasses import dataclass
from typing import TYPE_CHECKING

from capstone import m68k as M

from .game import (
    Game,
    access_size,
    base_mnemonic,
    h,
)

if TYPE_CHECKING:
    import argparse
    from .cli import Out

BIT_OPS = {"btst", "bset", "bclr", "bchg"}
MASK_OPS = {"andi": "clear", "ori": "set", "eori": "flip"}
ADDRESS_REGISTERS = {"a0", "a1", "a2", "a3", "a4", "a5", "a6"}


def insn_text(insn) -> str:
    text = f"{insn.mnemonic} {insn.op_str}".strip()
    return re.sub(r",\s*invalid\.[wl](\s*\*\s*\d)?", "", text.replace("(a16,", "(pc,"))


def mask_bits(insn, base: str) -> tuple[str, list[int]] | None:
    """`ori.b #4,$1(a0)` -> ('set', [2]): bits of the byte changed."""
    if base not in MASK_OPS or not insn.operands or insn.operands[0].type != M.M68K_OP_IMM:
        return None
    size = access_size(insn)
    imm = insn.operands[0].imm & ((1 << (8 * size)) - 1)
    byte = (imm >> (8 * (size - 1))) & 0xff
    if base == "andi":
        byte = ~byte & 0xff
    return MASK_OPS[base], [b for b in range(8) if (byte >> b) & 1]


def extract_bits(insn, base: str) -> list[str]:
    bits = []
    if base in BIT_OPS and insn.operands:
        op0 = insn.operands[0]
        if op0.type == M.M68K_OP_IMM:
            bits.append(f"{base} {op0.imm & 7}")
        elif op0.type == M.M68K_OP_REG:
            bits.append(f"{base} {insn.reg_name(op0.reg)}")
        else:
            bits.append(base)
    elif base in MASK_OPS:
        masked = mask_bits(insn, base)
        if masked:
            _, bit_list = masked
            for b in bit_list:
                bits.append(f"{base} {b}")
    return bits


def base_id(val: Game.Value | None) -> tuple | None:
    if val is None:
        return None
    if val.kind == "in":
        return ("in", val.reg)
    if val.kind == "field":
        return ("field", val.reg, val.value)
    if val.kind == "stream":
        return ("stream", val.reg)
    return (val.kind, val.value)


@dataclass
class Access:
    pc: int
    entry: int
    offset: int
    size: int
    role: str
    insn: object
    reg: str
    alias: str | None
    bits: list[str]


@dataclass
class IndexedAccess:
    pc: int
    entry: int
    insn: object
    op: object


def follow_routine(
    cg: Game,
    entry: int,
    target_reg: str,
    depth: int,
    max_depth: int,
    visited: set[int],
    accesses: list[Access],
    indexed: list[IndexedAccess],
    seen_ops: set[tuple[int, int]],
    is_callee: bool = False,
) -> None:
    if entry in visited or entry not in cg.routines:
        return
    visited.add(entry)

    states = cg.states(entry)
    pcs = cg.routines[entry].pcs

    if is_callee:
        first_base = ("in", target_reg)
        active_regs = {target_reg}
        used = False
        for pc in pcs:
            insn = cg.code[pc]
            st = states.get(pc, {})
            if base_id(st.get(target_reg)) != first_base:
                break
            for op in cg.operands(insn):
                if op.reg == target_reg and op.mode in ("areg", "areg-idx") and op.role in ("read", "write", "rmw"):
                    used = True
                    break
            if used:
                break
        if not used:
            return
        first_use_found = True
    else:
        first_base = None
        active_regs = set()
        first_use_found = False

    for pc in pcs:
        insn = cg.code[pc]
        base = base_mnemonic(insn)
        st = states.get(pc, {})

        if not first_use_found:
            for op in cg.operands(insn):
                if op.reg == target_reg and op.mode in ("areg", "areg-idx"):
                    first_use_found = True
                    first_base = base_id(st.get(target_reg))
                    active_regs = {target_reg}
                    break

        if not first_use_found:
            continue

        to_drop = {r for r in active_regs if base_id(st.get(r)) != first_base}
        active_regs -= to_drop
        if not active_regs:
            break

        if base in ("movea", "move") and len(insn.operands) == 2:
            op0, op1 = insn.operands
            if op0.type == M.M68K_OP_REG and op1.type == M.M68K_OP_REG:
                src_r = insn.reg_name(op0.reg)
                dst_r = insn.reg_name(op1.reg)
                if src_r in active_regs and dst_r.startswith("a") and dst_r != "a7":
                    active_regs.add(dst_r)
        elif base == "lea" and len(insn.operands) == 2:
            op0, op1 = insn.operands
            if op1.type == M.M68K_OP_REG:
                dst_r = insn.reg_name(op1.reg)
                mem_ops = [o for o in cg.operands(insn) if o.reg in active_regs]
                if mem_ops and mem_ops[0].disp == 0 and dst_r.startswith("a") and dst_r != "a7":
                    active_regs.add(dst_r)

        if base in ("bsr", "jsr") and depth < max_depth:
            kind, targets = cg._transfer_targets(pc, insn)
            if kind == "call":
                for tgt in targets:
                    for r in sorted(active_regs):
                        follow_routine(
                            cg, tgt, r, depth + 1, max_depth, visited, accesses, indexed, seen_ops, is_callee=True
                        )

        for op in cg.operands(insn):
            if op.reg in active_regs:
                op_key = (pc, getattr(op, "index", 0))
                if op_key in seen_ops:
                    continue
                seen_ops.add(op_key)
                sz = access_size(insn)
                cur_val = st.get(op.reg)
                step = cur_val.step if cur_val else 0
                if op.mode == "areg-idx" or getattr(op, "index_reg", None):
                    indexed.append(IndexedAccess(pc=pc, entry=entry, insn=insn, op=op))
                elif op.mode == "areg" and op.role in ("read", "write", "rmw"):
                    if getattr(op, "postinc", 0) < 0:
                        offset = op.disp + step - sz
                    else:
                        offset = op.disp + step
                    alias = op.reg if op.reg != target_reg else None
                    accesses.append(Access(
                        pc=pc,
                        entry=entry,
                        offset=offset,
                        size=sz,
                        role=op.role,
                        insn=insn,
                        reg=op.reg,
                        alias=alias,
                        bits=extract_bits(insn, base),
                    ))


def cmd_struct(args) -> None:
    from .cli import Out, load, resolve_target, routine_for

    reg = args.reg.lower().strip()
    if reg not in ADDRESS_REGISTERS:
        raise SystemExit(f"f3a: struct: {args.reg!r} is not an address register (a0..a6)")

    g = load(args)
    out = Out(args)

    target = resolve_target(g, args.routine)
    entry = routine_for(g, target)

    depth = getattr(args, "depth", 2)
    if depth is None or depth < 1:
        depth = 1

    accesses: list[Access] = []
    indexed: list[IndexedAccess] = []
    seen_ops: set[tuple[int, int]] = set()

    follow_routine(g, entry, reg, 1, depth, set(), accesses, indexed, seen_ops, is_callee=False)

    if not accesses and not indexed:
        raise SystemExit(f"f3a: struct: {reg} is never used as a base in {g.label(entry)}")

    out.note(f"structure accesses via {reg} in {g.label(entry)}" + (f" (depth {depth})" if depth > 1 else "") + ":")

    grouped: dict[tuple[int, int], list[Access]] = {}
    for a in accesses:
        key = (a.offset, a.size)
        grouped.setdefault(key, []).append(a)

    for (off, sz) in sorted(grouped.keys()):
        matching = grouped[(off, sz)]
        reads = sum(1 for a in matching if a.role == "read")
        writes = sum(1 for a in matching if a.role == "write")
        rmw = sum(1 for a in matching if a.role == "rmw")

        bits = []
        for a in matching:
            bits.extend(a.bits)
        bits = list(dict.fromkeys(bits))

        def routine_label(a: Access) -> str:
            lbl = g.label(a.entry)
            if a.alias:
                lbl += f" ({a.alias})"
            return lbl

        r_routines = sorted(set(routine_label(a) for a in matching if a.role == "read"))
        w_routines = sorted(set(routine_label(a) for a in matching if a.role == "write"))
        rmw_routines = sorted(set(routine_label(a) for a in matching if a.role == "rmw"))
        all_routines = sorted(set(g.label(a.entry) for a in matching))
        pcs = [h(p) for p in sorted(set(a.pc for a in matching))]

        parts = []
        if reads:
            parts.append(f"r  x{reads} {', '.join(r_routines)}")
        if writes:
            w_text = f"w x{writes}"
            if w_routines != r_routines:
                w_text += f" {', '.join(w_routines)}"
            parts.append(w_text)
        if rmw:
            rmw_text = f"rmw x{rmw}"
            if rmw_routines != r_routines:
                rmw_text += f" {', '.join(rmw_routines)}"
            parts.append(rmw_text)
        if bits:
            parts.append(f"bits: {', '.join(bits)}")

        suffix = {1: "b", 2: "w", 4: "l"}.get(sz, str(sz))
        off_str = f"+0x{off:02x}.{suffix}" if off >= 0 else f"-0x{-off:02x}.{suffix}"
        line = f"  {off_str:<9}  {'  '.join(parts)}"
        out.row(
            line,
            offset=off,
            size=sz,
            reads=reads,
            writes=writes,
            rmw=rmw,
            bits=bits,
            routines=all_routines,
            pcs=pcs,
        )

    if accesses:
        min_off = min(a.offset for a in accesses)
        max_end = max(a.offset + a.size for a in accesses)
        span_bytes = max_end - min_off
        min_s = f"+0x{min_off:02x}" if min_off >= 0 else f"-0x{-min_off:02x}"
        max_s = f"+0x{max_end:02x}" if max_end >= 0 else f"-0x{-max_end:02x}"
        out.note(f"span: {min_s}..{max_s} ({span_bytes} bytes, {len(grouped)} fields)")

    if indexed:
        out.note(f"{len(indexed)} indexed access{'es' if len(indexed) != 1 else ''} (variable displacement):")
        for ia in indexed:
            out.note(f"  {h(ia.pc)}  {g.owner_label(ia.pc):<20}  {insn_text(ia.insn)}")

    out.done()
