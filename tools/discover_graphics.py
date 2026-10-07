#!/usr/bin/env python3
"""Catalog rooted F3 graphics code and cross-ROM instruction families, not drivers."""
from __future__ import annotations

import argparse
from collections import defaultdict, deque
from copy import deepcopy
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
# Import discovery first: it supplies the project's build/python Capstone fallback.
from recomp.discovery import (CALL_MNEMONICS, COND_BRANCH_MNEMONICS,
                              TERMINAL_MNEMONICS, UNCOND_BRANCH_MNEMONICS,
                              _require_included, _resolve_target,
                              _scan_pc_memi_table, _scan_pci_index_table,
                              _scan_register_table, _validate_code_sequence,
                              discover, exclusion_at,
                              parse_exclusions)
from recomp.roms import load_region
from capstone.m68k import (M68K_OP_IMM, M68K_OP_REG, M68K_OP_MEM,
                          M68K_OP_REG_BITS, M68K_OP_REG_PAIR,
                          M68K_AM_ABSOLUTE_DATA_LONG, M68K_AM_ABSOLUTE_DATA_SHORT,
                          M68K_AM_REGI_ADDR, M68K_AM_REGI_ADDR_DISP,
                          M68K_AM_REGI_ADDR_POST_INC, M68K_AM_REGI_ADDR_PRE_DEC,
                          M68K_AM_AREGI_INDEX_8_BIT_DISP, M68K_AM_AREGI_INDEX_BASE_DISP,
                          M68K_AM_PCI_DISP, M68K_AM_PCI_INDEX_8_BIT_DISP,
                          M68K_AM_PCI_INDEX_BASE_DISP, M68K_AM_PC_MEMI_POST_INDEX,
                          M68K_AM_PC_MEMI_PRE_INDEX)
from recomp.discovery import Cs, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_020

SCHEMA_VERSION = 1
# Physical MMIO ranges, not an assumed game-specific descriptor interpretation.
REGIONS = (("sprite", 0x600000, 0x610000), ("playfield", 0x610000, 0x61c000),
           ("text", 0x61c000, 0x61e000), ("characters", 0x61e000, 0x620000),
           ("line", 0x620000, 0x630000), ("pivot", 0x630000, 0x640000),
           ("control", 0x660000, 0x660020))
REGISTERS = tuple([f"d{i}" for i in range(8)] + [f"a{i}" for i in range(8)])
INDEXED = {M68K_AM_AREGI_INDEX_8_BIT_DISP, M68K_AM_AREGI_INDEX_BASE_DISP,
           M68K_AM_PCI_INDEX_8_BIT_DISP, M68K_AM_PCI_INDEX_BASE_DISP}
PC_RELATIVE = {M68K_AM_PCI_DISP, M68K_AM_PCI_INDEX_8_BIT_DISP,
               M68K_AM_PCI_INDEX_BASE_DISP}
INDIRECT = {M68K_AM_REGI_ADDR, M68K_AM_REGI_ADDR_DISP,
            M68K_AM_REGI_ADDR_POST_INC, M68K_AM_REGI_ADDR_PRE_DEC} | INDEXED
READ_ONLY = {"cmp", "cmpa", "cmpi", "cmpm", "tst", "btst", "chk", "chk2", "cmp2"}
RMW = {"add", "addi", "addq", "sub", "subi", "subq", "and", "andi", "or",
       "ori", "eor", "eori", "asl", "asr", "lsl", "lsr", "rol", "ror",
       "roxl", "roxr", "neg", "negx", "not", "bset", "bclr", "bchg", "tas"}
CALIBRATION = {0x55c2: "Land Maker palette-plus-XOR rectangle tile compiler",
               0x5614: "Land Maker XOR rectangle tile compiler", 0x56ae: "Land Maker rectangle tile erase",
               0x4688: "Land Maker single sprite compiler", 0x46c0: "Land Maker grid sprite compiler",
               0x480c: "Land Maker scaled grid sprite compiler"}


def region(address: int | None):
    if address is not None:
        address &= 0xffffff
        return next((name for name, start, end in REGIONS if start <= address < end), None)
    return None


def evidence(insn):
    return {"pc": insn.address, "bytes": bytes(insn.bytes).hex(),
            "mnemonic": insn.mnemonic, "operands": insn.op_str, "size": insn.size}


def base_mnemonic(insn):
    return insn.mnemonic.lower().split(".")[0]


def width(insn):
    suffix = insn.mnemonic.rsplit(".", 1)[-1]
    return {"b": 1, "w": 2, "l": 4}.get(suffix, 0)


@dataclass(frozen=True)
class Value:
    """Bounded symbolic term; unknown is None, never a fabricated zero."""
    kind: str
    number: int = 0
    name: str = ""
    args: tuple = ()
    origins: tuple[int, ...] = ()

    def json(self):
        result = {"kind": self.kind}
        if self.kind in {"constant", "input"}:
            result["value" if self.kind == "constant" else "offset"] = self.number
        elif self.kind in {"hardware_cursor", "input_cursor", "derived_cursor"}:
            result["offset"] = None
        if self.name:
            result["name"] = self.name
        if self.args:
            result["arguments"] = [arg.json() if isinstance(arg, Value) else arg for arg in self.args]
        if self.origins:
            result["evidence_pcs"] = list(self.origins)
        return result


def const(number, pc=None):
    return Value("constant", number & 0xffffffff, origins=() if pc is None else (pc,))


def add(value, amount, pc):
    if value is None:
        return None
    origins = tuple(sorted(set(value.origins + (pc,))))[:16]
    if value.kind == "constant":
        return Value("constant", (value.number + amount) & 0xffffffff, origins=origins)
    if value.kind == "input":
        return Value("input", value.number + amount, value.name, origins=origins)
    if value.kind in {"hardware_cursor", "input_cursor", "derived_cursor"}:
        return value
    return operation("add", (value, const(amount)), pc)


def operation(name, args, pc):
    if any(arg is None for arg in args):
        return None
    # Bound expression growth on loops and long arithmetic chains.
    def depth(value):
        return 1 + max((depth(arg) for arg in value.args if isinstance(arg, Value)), default=0)
    if max(map(depth, args), default=0) >= 5:
        return None
    return Value("expression", name=name, args=tuple(args), origins=(pc,))


def same_term(a, b):
    if a is None or b is None:
        return a is b
    return (a.kind, a.number, a.name, a.args) == (b.kind, b.number, b.name, b.args)


def merge_value(a, b):
    if same_term(a, b):
        if a is None:
            return None
        return Value(a.kind, a.number, a.name, a.args,
                     tuple(sorted(set(a.origins + b.origins)))[:16])
    # Retain parameter ancestry when only a loop cursor offset differs. This
    # proves a derived pointer, not its effective address or bounds.
    if (a is not None and b is not None and a.kind in {"input", "input_cursor"}
            and b.kind in {"input", "input_cursor"} and a.name == b.name):
        return Value("input_cursor", name=a.name,
                     origins=tuple(sorted(set(a.origins + b.origins)))[:16])
    def pointer_base(value):
        if value is None:
            return None
        if value.kind == "derived_cursor":
            return value.args[0]
        if value.kind == "expression" and value.name == "load":
            return value
        if (value.kind == "expression" and value.name == "add" and len(value.args) == 2
                and value.args[1].kind == "constant"):
            return pointer_base(value.args[0])
        return None
    left_base, right_base = pointer_base(a), pointer_base(b)
    if left_base is not None and same_term(left_base, right_base):
        return Value("derived_cursor", args=(left_base,),
                     origins=tuple(sorted(set(a.origins + b.origins)))[:16])
    # A cursor ancestry is useful evidence, but NOT a proven effective address.
    def anchor(value):
        if value is None:
            return None
        if value.kind == "hardware_cursor":
            return value.name
        if value.kind == "constant":
            return region(value.number)
        return None
    left, right = anchor(a), anchor(b)
    if left and left == right:
        return Value("hardware_cursor", name=left,
                     origins=tuple(sorted(set(a.origins + b.origins)))[:16])
    return None


def reg_name(insn, reg):
    name = insn.reg_name(reg)
    return "a7" if name == "sp" else name


def memory_span(insn):
    amount = width(insn)
    if base_mnemonic(insn) == "movem":
        masks = [op.register_bits for op in insn.operands if op.type == M68K_OP_REG_BITS]
        return amount * (masks[0].bit_count() if masks else 0)
    if base_mnemonic(insn) == "movep":
        return max(0, amount * 2 - 1)
    return amount


def pointer_step(insn, name):
    if base_mnemonic(insn) == "movem":
        return memory_span(insn)
    return 2 if name == "a7" and width(insn) == 1 else width(insn)


def full_base_extension(insn, op):
    raw = bytes(insn.bytes)
    if len(raw) < 4 or op.address_mode not in INDEXED:
        return None
    opcode = int.from_bytes(raw[:2], "big")
    ea = opcode & 0x3f
    if not (ea >> 3 == 6 or ea == 0x3b):
        return None
    extension = int.from_bytes(raw[2:4], "big")
    if not extension & 0x100 or extension & 7:
        return None  # Memory-indirect is a dereference, never this affine EA.
    size = {1: 0, 2: 2, 3: 4}.get((extension >> 4) & 3)
    if size is None or len(raw) < 4 + size:
        return None
    return {"base_suppressed": bool(extension & 0x80), "index_suppressed": bool(extension & 0x40),
            "base_displacement": int.from_bytes(raw[4:4 + size], "big", signed=True),
            "base_displacement_bytes": size}


def operand_address(insn, op, state):
    mode, pc = op.address_mode, insn.address
    if mode in {M68K_AM_ABSOLUTE_DATA_LONG, M68K_AM_ABSOLUTE_DATA_SHORT}:
        address = op.imm
        if mode == M68K_AM_ABSOLUTE_DATA_SHORT and address & 0x8000:
            address |= 0xffff0000
        return const(address, pc)
    if mode not in INDIRECT and mode not in PC_RELATIVE:
        # Memory-indirect full extensions need a real dereference: do not
        # mistake Capstone's base/outer displacement for a direct address.
        return None
    full = full_base_extension(insn, op)
    displacement = full["base_displacement"] if full else op.mem.disp
    if full and full["base_suppressed"]:
        address = const(displacement, pc)
    elif mode in PC_RELATIVE:
        address = const(pc + 2 + displacement, pc)
    else:
        name = reg_name(insn, op.mem.base_reg or op.reg)
        address = add(state.get(name), displacement, pc)
    if mode == M68K_AM_REGI_ADDR_PRE_DEC:
        step = pointer_step(insn, reg_name(insn, op.mem.base_reg or op.reg))
        address = add(address, -step, pc)
    if mode in INDEXED and op.mem.index_reg and not (full and full["index_suppressed"]):
        index = state.get(reg_name(insn, op.mem.index_reg))
        if index is not None and index.kind == "constant":
            bits = 32 if op.mem.index_size else 16
            number = index.number & ((1 << bits) - 1)
            if number & (1 << (bits - 1)):
                number -= 1 << bits
            address = add(address, number * (op.mem.scale or 1), pc)
        else:
            if index is None:
                index = Value("unknown", name=reg_name(insn, op.mem.index_reg))
            address = operation("indexed_address", (address, index, const(op.mem.scale or 1)), pc)
    return address


def stack_key(address):
    if address is not None and address.kind == "input" and address.name == "a7":
        return f"stack:{address.number}"
    return None


def operand_value(insn, op, state):
    if op.type == M68K_OP_IMM:
        return const(op.imm, insn.address)
    if op.type == M68K_OP_REG:
        return state.get(reg_name(insn, op.reg))
    if op.type == M68K_OP_MEM:
        address = operand_address(insn, op, state)
        key = stack_key(address)
        if key is not None and key in state and width(insn) == 4:
            return state[key]
        return operation("load", (address, const(width(insn))), insn.address)
    return None


def access_modes(insn):
    """Conservative integer-instruction memory direction, not regs_access()."""
    base, ops = base_mnemonic(insn), insn.operands
    if base in CALL_MNEMONICS | UNCOND_BRANCH_MNEMONICS or base in {"lea", "pea"}:
        return ["address"] * len(ops)
    if base == "movem":
        return ["write" if i == len(ops) - 1 else "read" for i in range(len(ops))]
    if base in READ_ONLY:
        return ["read"] * len(ops)
    if base in {"move", "movea", "moves", "moveq", "movep"}:
        return ["write" if i == len(ops) - 1 else "read" for i in range(len(ops))]
    if base in RMW:
        return ["read_write" if i == len(ops) - 1 else "read" for i in range(len(ops))]
    if base == "clr" or (base.startswith("s") and base not in {"stop", "swap", "suba", "subx"}):
        return ["write"] * len(ops)
    return ["unclassified"] * len(ops)


def transfer(insn, incoming):
    state = dict(incoming)
    base, ops, pc = base_mnemonic(insn), insn.operands, insn.address
    # Evaluate in actual operand order; postinc on a source can affect a
    # destination using the same An (e.g. move.w (a0)+,(a0)+).
    values, addresses, accesses = [], [], []
    modes = access_modes(insn)
    for i, op in enumerate(ops):
        address = operand_address(insn, op, state) if op.type == M68K_OP_MEM else None
        addresses.append(address)
        values.append(operand_value(insn, op, state))
        if op.type == M68K_OP_MEM:
            source = values[0] if i and modes[i] in {"write", "read_write"} else None
            accesses.append({"operand": i, "mode": modes[i], "address": address,
                             "width": width(insn), "span": memory_span(insn),
                             "layout": "every_other_byte" if base == "movep" else "contiguous", "source": source})
            key = stack_key(address)
            if key is not None and modes[i] in {"write", "read_write"}:
                state[key] = source if modes[i] == "write" and width(insn) == 4 else None
        if op.address_mode in {M68K_AM_REGI_ADDR_POST_INC, M68K_AM_REGI_ADDR_PRE_DEC}:
            name = reg_name(insn, op.mem.base_reg or op.reg)
            step = pointer_step(insn, name)
            state[name] = add(state.get(name), step if op.address_mode == M68K_AM_REGI_ADDR_POST_INC else -step, pc)
    destination = reg_name(insn, ops[-1].reg) if ops and ops[-1].type == M68K_OP_REG else None
    if destination in state:
        old = state[destination]
        if base in {"move", "movea", "moveq"}:
            value = values[0]
            if base == "movea" and width(insn) == 2:
                if value and value.kind == "constant":
                    word = value.number & 0xffff
                    value = const(word - 0x10000 if word & 0x8000 else word, pc)
                else:
                    value = operation("sign_extend_16", (value,), pc)
            elif base == "moveq" and value and value.kind == "constant":
                byte = value.number & 0xff
                value = const(byte - 0x100 if byte & 0x80 else byte, pc)
            elif destination.startswith("d") and width(insn) in {1, 2}:
                # 68k partial writes preserve the upper Dn bits.
                mask = (1 << (width(insn) * 8)) - 1
                if old and value and old.kind == value.kind == "constant":
                    value = const((old.number & ~mask) | (value.number & mask), pc)
                else:
                    value = operation(f"replace_low_{width(insn) * 8}", (old, value), pc)
            state[destination] = value
        elif base == "lea":
            state[destination] = addresses[0]
        elif base == "clr":
            state[destination] = const(0, pc) if width(insn) == 4 else None
        elif base in {"add", "adda", "addi", "addq", "sub", "suba", "subi", "subq"}:
            source = values[0]
            if source and source.kind == "constant" and (width(insn) == 4 or destination.startswith("a")):
                amount = source.number
                if amount & 0x80000000:
                    amount -= 0x100000000
                if destination.startswith("a") and width(insn) == 2:
                    amount &= 0xffff
                    if amount & 0x8000:
                        amount -= 0x10000
                state[destination] = add(old, -amount if base.startswith("sub") else amount, pc)
            else:
                state[destination] = operation(insn.mnemonic, (old, source), pc)
        elif base in RMW:
            state[destination] = operation(insn.mnemonic, (old, *values[:-1]), pc)
        elif base not in READ_ONLY and base not in CALL_MNEMONICS | UNCOND_BRANCH_MNEMONICS:
            state[destination] = None
    if base == "exg" and len(ops) == 2:
        names = [reg_name(insn, op.reg) for op in ops]
        state[names[0]], state[names[1]] = incoming.get(names[1]), incoming.get(names[0])
    if base.startswith("db") and ops and ops[0].type == M68K_OP_REG:
        state[reg_name(insn, ops[0].reg)] = None
    if base == "movem":
        # Restore lists invalidate their destination registers. A save list
        # supplies ABI evidence, but does not imply a preserved value after calls.
        if ops and ops[-1].type == M68K_OP_REG_BITS:
            for i, name in enumerate(REGISTERS):
                if ops[-1].register_bits & (1 << i):
                    if (ops[0].address_mode == M68K_AM_REGI_ADDR_POST_INC
                            and name == reg_name(insn, ops[0].mem.base_reg or ops[0].reg)):
                        continue  # Postincrement address wins over a loaded base An.
                    state[name] = None
    if base == "link" and len(ops) == 2:
        frame = reg_name(insn, ops[0].reg)
        state[frame] = add(state.get("a7"), -4, pc)
        state["a7"] = add(state[frame], ops[1].imm, pc)
    if base == "unlk":
        frame = reg_name(insn, ops[0].reg)
        state["a7"] = add(incoming.get(frame), 4, pc)
        state[frame] = None
    if base == "pea":
        state["a7"] = add(state.get("a7"), -4, pc)
        key = stack_key(state["a7"])
        if key is not None:
            state[key] = addresses[0] if addresses else None
    if base in CALL_MNEMONICS or base in {"trap", "aline"}:
        # No guessed compiler calling convention: callees/traps may clobber all
        # registers except the balanced return stack. Call arguments are captured
        # from incoming state before applying this conservative barrier.
        state = {name: (state.get(name) if name == "a7" else None) for name in REGISTERS}
    return state, accesses


def next_pc(insn, config, targets):
    pc = insn.address + insn.size
    if base_mnemonic(insn) in CALL_MNEMONICS:
        helpers = {int(p, 0) if isinstance(p, str) else p for p in config.get("discovery", {}).get("inline_string_helpers", [])}
        if any(target in helpers for target in targets):
            return None  # caller computes the byte-skipping successor from ROM
    return pc


def graph(discovery, rom, config):
    successors = {}
    for pc, insn in discovery.instructions.items():
        base = base_mnemonic(insn)
        targets = discovery.branch_targets.get(pc, [])
        following = next_pc(insn, config, targets)
        if following is None:
            following = pc + insn.size
            while following < len(rom) and rom[following]:
                following += 1
            following = (following + 2) & ~1
        if base in TERMINAL_MNEMONICS:
            edges = []
        elif base in UNCOND_BRANCH_MNEMONICS:
            edges = targets
        elif base in COND_BRANCH_MNEMONICS:
            edges = targets + [following]
        else:
            edges = [following]
        successors[pc] = sorted({edge for edge in edges if edge in discovery.instructions})
    return successors


def function_members(discovery, successors):
    members, owners = {}, defaultdict(list)
    entries = discovery.functions & discovery.instructions.keys()
    for entry in sorted(entries):
        seen, pending = set(), [entry]
        while pending:
            pc = pending.pop()
            if pc in seen or (pc != entry and pc in entries):
                continue
            seen.add(pc)
            pending.extend(successors.get(pc, []))
        members[entry] = sorted(seen)
        for pc in seen:
            owners[pc].append(entry)
    return members, owners


def analyze_function(entry, pcs, instructions, successors):
    initial = {name: Value("input", name=name) for name in REGISTERS}
    states, pending, queued = {entry: initial}, deque([entry]), {entry}
    allowed = set(pcs)
    while pending:
        pc = pending.popleft()
        queued.discard(pc)
        outgoing, _ = transfer(instructions[pc], states[pc])
        for target in successors.get(pc, []):
            if target not in allowed:
                continue
            if target not in states:
                joined = outgoing.copy()
            else:
                joined = {name: merge_value(states[target].get(name), outgoing.get(name))
                          for name in states[target].keys() | outgoing.keys()}
            if target not in states or joined != states[target]:
                states[target] = joined
                if target not in queued:
                    queued.add(target)
                    pending.append(target)
    accesses, calls = [], []
    for pc in sorted(states):
        insn = instructions[pc]
        _, memory = transfer(insn, states[pc])
        for access in memory:
            accesses.append({"pc": pc, **access})
        if base_mnemonic(insn) in CALL_MNEMONICS | UNCOND_BRANCH_MNEMONICS | {"aline"}:
            calls.append({"pc": pc, "state": states[pc]})
    return states, accesses, calls


def address_value(value):
    if value and value.kind == "constant":
        return value.number
    return None


def resolve_value(value, registers):
    if value is None:
        return None
    if value.kind == "input":
        rebound = registers.get(value.name)
        if value.number == 0 and not value.origins:
            return rebound
        return add(rebound, value.number, value.origins[-1] if value.origins else 0)
    if value.kind == "input_cursor":
        rebound = registers.get(value.name)
        if rebound is not None and rebound.kind == "constant" and region(rebound.number):
            return Value("hardware_cursor", name=region(rebound.number),
                         origins=tuple(sorted(set(value.origins + rebound.origins)))[:16])
        if rebound is not None and rebound.kind in {"input", "input_cursor"}:
            return Value("input_cursor", name=rebound.name,
                         origins=tuple(sorted(set(value.origins + rebound.origins)))[:16])
        if rebound is not None and rebound.kind == "hardware_cursor":
            return rebound
        return None
    if value.args:
        args = tuple(resolve_value(arg, registers) if isinstance(arg, Value) else arg for arg in value.args)
        if any(arg is None for arg in args):
            return None
        if value.kind == "derived_cursor" and args:
            anchor = graphics_anchor(args[0])
            if anchor:
                return Value("hardware_cursor", name=anchor,
                             origins=tuple(sorted(set(value.origins + args[0].origins)))[:16])
        if value.kind == "expression" and value.name == "load" and len(args) == 2:
            key = stack_key(args[0])
            if key is not None and address_value(args[1]) == 4 and registers.get(key) is not None:
                return registers[key]
        return Value(value.kind, value.number, value.name, args, value.origins)
    return value


def rom_pointer_origins(value, rom_size):
    if value is None:
        return set()
    result = set(value.origins) if value.kind == "constant" and 0x400 <= value.number < rom_size else set()
    for arg in value.args:
        if isinstance(arg, Value):
            result.update(rom_pointer_origins(arg, rom_size))
    return result


def input_symbols(value):
    if value is None:
        return set()
    result = {value.name} if value.kind in {"input", "input_cursor"} else set()
    for arg in value.args:
        if isinstance(arg, Value):
            result.update(input_symbols(arg))
    return result


class ALineInstruction:
    """Architectural A-line exception whose OS dispatcher consumes a NOP slot."""
    def __init__(self, md, rom, pc, caller_frame=None):
        self.caller_frame = caller_frame
        self.address = pc
        self.bytes = rom[pc:pc + 4]
        self.size = 4
        self.mnemonic = "aline"
        self.service = int.from_bytes(self.bytes[:2], "big") & 0x7f
        self.op_str = f"#{self.service:#x}"
        self._trap = next(md.disasm(bytes.fromhex("4e40"), pc, count=1))
        self.id = self._trap.id
        self.operands = self._trap.operands
        self.operands[0].value.imm = self.service

    def reg_name(self, register):
        return self._trap.reg_name(register)


def aline_dispatcher(rom, discovery):
    """Recognize the rooted Taito A-line dispatcher, including its table bound.

    No directory-name dispatch: prove the opcode load, two extra PC bytes,
    stacked-PC replacement, 7-bit mask, and full-extension indexed service call.
    """
    handler = int.from_bytes(rom[0x28:0x2c], "big")
    code = [discovery.instructions[pc] for pc in sorted(discovery.instructions)
            if handler <= pc < handler + 0x40]
    for i, insn in enumerate(code):
        raw = bytes(insn.bytes)
        if len(raw) != 6 or raw[:4] != bytes.fromhex("4ebb0521"):
            continue
        prefix = code[max(0, i - 8):i]
        opcode_reads = [item for item in prefix if base_mnemonic(item) == "move" and width(item) == 2
                        and item.operands and item.operands[0].address_mode == M68K_AM_REGI_ADDR_POST_INC]
        masks = [item for item in prefix if base_mnemonic(item) == "andi" and item.operands
                 and item.operands[0].type == M68K_OP_IMM and item.operands[0].imm == 0x7f]
        adjustments = [item for item in prefix if base_mnemonic(item) == "addq" and width(item) == 4
                       and item.operands and item.operands[0].imm == 2]
        replacements = [item for item in prefix if base_mnemonic(item) == "move" and width(item) == 4
                        and item.operands[-1].type == M68K_OP_MEM
                        and item.operands[-1].mem.disp == 6]
        frame_loads = [item for item in prefix if base_mnemonic(item) == "movea" and width(item) == 4
                       and item.operands[0].type == M68K_OP_MEM and item.operands[0].mem.disp == 6
                       and reg_name(item, item.operands[0].mem.base_reg) == "a6"]
        if not opcode_reads or not masks or not adjustments or not replacements or not frame_loads:
            continue
        source = reg_name(opcode_reads[-1], opcode_reads[-1].operands[0].mem.base_reg or opcode_reads[-1].operands[0].reg)
        index = reg_name(masks[-1], masks[-1].operands[-1].reg)
        if (reg_name(adjustments[-1], adjustments[-1].operands[-1].reg) != source
                or reg_name(replacements[-1], replacements[-1].operands[0].reg) != source
                or reg_name(frame_loads[-1], frame_loads[-1].operands[-1].reg) != source
                or reg_name(replacements[-1], replacements[-1].operands[-1].mem.base_reg) != "a6"
                or reg_name(opcode_reads[-1], opcode_reads[-1].operands[-1].reg) != index
                or index != "d0"):
            continue
        displacement = int.from_bytes(raw[4:6], "big", signed=True)
        table = insn.address + 2 + displacement
        if not 0 <= table <= len(rom) - 128 * 4:
            continue
        targets = [int.from_bytes(rom[p:p + 4], "big") for p in range(table, table + 128 * 4, 4)]
        if any(target & 1 or not 0x400 <= target < len(rom) for target in targets):
            continue
        # Rebind service arguments only for the fully proved dispatcher frame:
        # eight-byte exception + LINK, D0/A0 save, then the service JSR.
        frame_code = [discovery.instructions.get(pc) for pc in
                      (handler, handler + 4, insn.address + 6, insn.address + 8,
                       insn.address + 12, insn.address + 14)]
        caller_frame = None
        if (insn.address == handler + 24 and all(frame_code)
                and [bytes(item.bytes).hex() for item in frame_code] ==
                    ["4e560000", "48e78080", "4e71", "4cdf0101", "4e5e", "4e73"]):
            caller_frame = {"a6_from_caller_sp": -12, "a7_from_caller_sp": -24,
                            "evidence": [evidence(item) for item in frame_code]}
        return {"handler": handler, "call_pc": insn.address, "table": table,
                "count": 128, "targets": targets, "caller_frame": caller_frame,
                "evidence": [evidence(item) for item in prefix + [insn]],
                "certainty": "rooted_dispatcher_mask_and_index_prove_table_bound"}
    return None


def rooted_sequence(instructions, pc, raw):
    """Match bytes only at contiguous, already reached instruction starts."""
    sequence, offset = [], 0
    while offset < len(raw):
        insn = instructions.get(pc + offset)
        if insn is None or bytes(insn.bytes) != raw[offset:offset + insn.size]:
            return None
        sequence.append(insn)
        offset += insn.size
    return sequence if offset == len(raw) else None


def heuristic_table_probe(rom, md, insn, instructions):
    """Expose the shared guesses' byte producer/cells without making roots."""
    targets = _scan_register_table(rom, md, insn)
    op = insn.operands[-1] if insn.operands else None
    producer, producer_bytes, outer = insn.address, bytes(insn.bytes), 0
    if targets:
        method, step, count = "register_pointer_table", 4, 64
        end = insn.address
        while end >= insn.address - 2 and rom[end - 2:end] == bytes.fromhex("4e71"):
            end -= 2
        producer = end - 4
        producer_bytes = rom[producer:producer + 4]
        table = producer + 2 + int.from_bytes(producer_bytes[3:4], "big", signed=True)
    elif op is not None and op.address_mode in {M68K_AM_PCI_INDEX_8_BIT_DISP, M68K_AM_PCI_INDEX_BASE_DISP}:
        targets = _scan_pci_index_table(rom, md, insn, op)
        method, step, count = "heuristic_relative_word_table", 2, 128
        table = insn.address + 2 + op.mem.disp
    elif op is not None and op.address_mode in {M68K_AM_PC_MEMI_PRE_INDEX, M68K_AM_PC_MEMI_POST_INDEX}:
        targets = _scan_pc_memi_table(rom, md, insn)
        if not targets:
            return []
        method, step, count = "heuristic_memory_indirect_pointer_table", 4, 64
        raw = bytes(insn.bytes)
        extension = int.from_bytes(raw[2:4], "big")
        base_size = {1: 0, 2: 2, 3: 4}[(extension >> 4) & 3]
        outer_size = {1: 0, 2: 2, 3: 4}[extension & 7]
        displacement = int.from_bytes(raw[4:4 + base_size], "big", signed=True)
        outer = int.from_bytes(raw[4 + base_size:4 + base_size + outer_size], "big", signed=True)
        table = (0 if extension & 0x80 else insn.address + 2) + displacement
    else:
        return []
    cells = defaultdict(list)
    wanted = set(targets)
    for index in range(count):
        pc = table + index * step
        if not 0 <= pc <= len(rom) - step:
            break
        raw = rom[pc:pc + step]
        number = int.from_bytes(raw, "big", signed=step == 2)
        target = table + number if step == 2 else (number + outer) & 0xffffffff
        if target in wanted:
            cells[target].append({"pc": pc, "bytes": raw.hex(), "index": index, "encoded_value": number})
    rooted_producer = instructions.get(producer)
    return [{"target": target, "method": method, "table": table, "entry_bytes": step,
             "outer_displacement": outer, "matching_cells_in_bounded_probe": cells[target],
             "producer_pc": producer, "producer_bytes": producer_bytes.hex(),
             "producer_is_reached_instruction": rooted_producer is not None
                 and bytes(rooted_producer.bytes) == producer_bytes}
            for target in targets]


def trap_task_frame(rom, discovery, successors):
    """Prove this OS's TRAP #1 callback is a synthetic task RTE PC, not data.

    Recognition is deliberately narrow: a rooted short exception frame, the
    complete PC/SR/register frame constructor, publication into the task-SP
    array, and a rooted scheduler that restores that array via MOVEM/RTE.
    Other trap ABIs remain unfollowed pointer candidates.
    """
    handler = int.from_bytes(rom[0x84:0x88], "big")
    prologue = rooted_sequence(discovery.instructions, handler,
                               bytes.fromhex("007c070048e703c0"))
    if prologue is None:
        return None
    frame_raw = bytes.fromhex("4261232f0018333c200048e1003e232f000c"
                              "232f0008232f0004231748e1fc00")
    for pc in sorted(discovery.instructions):
        if not handler <= pc < handler + 0x100:
            continue
        constructor = rooted_sequence(discovery.instructions, pc, frame_raw)
        if constructor is None:
            continue
        publication_pc = pc + len(frame_raw)
        load = discovery.instructions.get(publication_pc)
        if load is None or bytes(load.bytes[:2]) != bytes.fromhex("41ed") or load.size != 4:
            continue
        displacement = bytes(load.bytes[2:4])
        publication = rooted_sequence(discovery.instructions, publication_pc,
                                      bytes.fromhex("41ed") + displacement + bytes.fromhex("21897000"))
        if publication is None:
            continue
        for scheduler_pc in sorted(discovery.instructions):
            prefix = rooted_sequence(discovery.instructions, scheduler_pc,
                                     bytes.fromhex("49ed") + displacement + bytes.fromhex("49f41000"))
            if prefix is None:
                continue
            publish = discovery.instructions.get(scheduler_pc + 8)
            switch = discovery.instructions.get(scheduler_pc + 12)
            if (publish is None or publish.size != 4
                    or bytes(publish.bytes[:2]) != bytes.fromhex("2b4c")
                    or switch is None or bytes(switch.bytes) != bytes.fromhex("2e54")):
                continue
            seen, pending = set(), [scheduler_pc + 14]
            while pending:
                restore_pc = pending.pop()
                if restore_pc in seen or not scheduler_pc <= restore_pc < scheduler_pc + 40:
                    continue
                seen.add(restore_pc)
                restore = rooted_sequence(discovery.instructions, restore_pc, bytes.fromhex("4cdf7fff4e73"))
                if restore is not None:
                    return {"handler": handler, "callback_stack_offset": 0,
                            "task_stack_array_displacement": int.from_bytes(displacement, "big", signed=True),
                            "scheduler_pc": scheduler_pc,
                            "evidence": [evidence(item) for item in
                                         prologue + constructor + publication + prefix + [publish, switch] + restore],
                            "certainty": "stored_task_pc_consumed_by_rooted_movem_rte_scheduler"}
                pending.extend(successors.get(restore_pc, []))
    return None


def function_callers(analyses, discovery, members):
    callers = defaultdict(list)
    for entry, (_, _, calls) in analyses.items():
        for call in calls:
            insn = discovery.instructions[call["pc"]]
            base = base_mnemonic(insn)
            kind = "call" if base in CALL_MNEMONICS else "aline_service" if base == "aline" else "tail_transfer"
            for target in discovery.branch_targets.get(call["pc"], []):
                if target in members:
                    callers[target].append({"function": entry, "pc": call["pc"], "state": call["state"],
                                            "kind": kind, "caller_frame": getattr(insn, "caller_frame", None)})
    return callers


def caller_registers(caller):
    state = dict(caller["state"])
    if caller["kind"] == "call":
        state["a7"] = add(state.get("a7"), -4, caller["pc"])
    elif caller["kind"] == "aline_service":
        frame = caller["caller_frame"]
        if frame is None:
            return {}  # No guessed frame layout for an unfamiliar dispatcher.
        stack = state.get("a7")
        state["a6"] = add(stack, frame["a6_from_caller_sp"], caller["pc"])
        state["a7"] = add(stack, frame["a7_from_caller_sp"], caller["pc"])
        state["d0"] = None  # Dispatcher consumes the opcode in D0.
        state["a0"] = const(caller["pc"] + 4, caller["pc"])
    return state


def bind_callers(entry, value, callers, depth, visited=frozenset()):
    if value is None:
        return []
    anchored = graphics_anchor(value)
    if value.kind in {"constant", "hardware_cursor"}:
        return [(value, [])]
    if anchored:
        return [(Value("hardware_cursor", name=anchored, origins=value.origins), [])]
    if depth == 0 or entry in visited:
        return []
    result = []
    for caller in callers.get(entry, []):
        rebound = resolve_value(value, caller_registers(caller))
        for resolved, chain in bind_callers(caller["function"], rebound, callers, depth - 1, visited | {entry}):
            result.append((resolved, [{"function": caller["function"], "pc": caller["pc"],
                                      "kind": caller["kind"]}] + chain))
    return result


def graphics_anchor(value):
    if value is None:
        return None
    if value.kind == "constant":
        return region(value.number)
    if value.kind == "hardware_cursor":
        return value.name
    if value.kind == "expression" and value.name in {"indexed_address", "add"} and value.args:
        return graphics_anchor(value.args[0])
    return None


def rooted_discovery(rom, original_config, rounds, caller_depth):
    config = deepcopy(original_config)
    cfg = config.setdefault("discovery", {})
    cfg.update(coverage="recursive", scan_task_traps=False, scan_callbacks=False,
               scan_jump_tables=False)
    # Existing actor_scripts inference scans all aligned words for pointer
    # stores. Keep explicit entry/jump-table metadata, but do not run that scan.
    cfg.pop("actor_scripts", None)
    origins, candidate_records = [], {}
    continuation_seeds = set()
    aline_sites = {}
    dispatcher_record = None
    exclusions = parse_exclusions(config, len(rom))
    md = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
    md.detail = True
    known = set()
    heuristic_tables = {}

    def pointer_candidate(target, record, value, result):
        # A value being stored/read or decoding coherently proves neither a
        # callback ABI nor an instruction start. Never feed it to entry_points.
        key = (record["kind"], record["source_pc"], record["function"], target,
               record.get("destination"), record.get("input_location"))
        if key in candidate_records:
            return
        excluded = exclusion_at(exclusions, target)
        record.update(entry=target, followed=False,
                      disposition="unfollowed_pointer_candidate",
                      source=value.json(), instruction=evidence(result.instructions[record["source_pc"]]),
                      pointer_evidence_pcs=list(value.origins),
                      pointer_instructions=[evidence(result.instructions[pc]) for pc in value.origins
                                            if pc in result.instructions],
                      coherent_code_sequence=_validate_code_sequence(rom, md, target),
                      target_bytes=rom[target:target + 16].hex(),
                      target_exclusion=({"start": excluded.start, "end": excluded.end,
                                         "reason": excluded.reason, "evidence": excluded.evidence}
                                        if excluded is not None else None))
        candidate_records[key] = record
        origins.append(record)
    def decode_platform(decoder, image, pc):
        if dispatcher_record is None:
            return None
        raw = int.from_bytes(image[pc:pc + 2], "big")
        if raw & 0xff80 == 0xa000 and image[pc + 2:pc + 4] == bytes.fromhex("4e71"):
            if pc not in aline_sites:
                aline_sites[pc] = raw & 0x7f
                origins.append({"entry": pc + 4, "source_pc": pc, "kind": "rooted_aline_dispatcher_continuation",
                                "service": raw & 0x7f, "certainty": "verified_dispatcher_four_byte_service_slot"})
            return ALineInstruction(decoder, image, pc, dispatcher_record["caller_frame"])
        return None

    for iteration in range(rounds + 1):
        result = discover(rom, config, instruction_decoder=decode_platform)
        result.functions.difference_update(continuation_seeds)
        for pc in aline_sites:
            result.instructions[pc] = ALineInstruction(md, rom, pc, dispatcher_record["caller_frame"])
        dispatcher = aline_dispatcher(rom, result)
        if dispatcher is not None:
            dispatcher_record = dispatcher
            for failure in result.report["unresolved_branches"]:
                if failure["reason"] != "decode_failure":
                    continue
                pc = int(failure["pc"], 16)
                raw = int.from_bytes(rom[pc:pc + 2], "big")
                if raw & 0xff80 == 0xa000 and rom[pc + 2:pc + 4] == bytes.fromhex("4e71"):
                    if pc not in aline_sites:
                        aline_sites[pc] = raw & 0x7f
                        origins.append({"entry": pc + 4, "source_pc": pc,
                                        "kind": "rooted_aline_dispatcher_continuation",
                                        "service": raw & 0x7f, "dispatcher": dispatcher,
                                        "certainty": "stacked_pc_update_proves_four_byte_service_slot"})
                    result.instructions[pc] = ALineInstruction(md, rom, pc, dispatcher["caller_frame"])
                    result.branch_targets[pc] = [dispatcher["targets"][raw & 0x7f]]
        for pc, service in aline_sites.items():
            if dispatcher_record:
                result.branch_targets[pc] = [dispatcher_record["targets"][service]]
        successors = graph(result, rom, config)
        members, owners = function_members(result, successors)
        analyses = {entry: analyze_function(entry, pcs, result.instructions, successors) for entry, pcs in members.items()}
        callers = function_callers(analyses, result, members)
        task_frame = trap_task_frame(rom, result, successors)
        if task_frame and not any(item.get("kind") == "rooted_trap_task_frame_abi" for item in origins):
            origins.append({"kind": "rooted_trap_task_frame_abi", **task_frame})
        new_entries, transfers = {}, defaultdict(set)
        expansion_records = []
        new_continuations = {pc + 4 for pc in aline_sites if pc + 4 not in result.instructions}
        if dispatcher_record and not any(item.get("kind") == "rooted_aline_service_table" for item in origins):
            transfers[dispatcher_record["call_pc"]].update(dispatcher_record["targets"])
            origins.append({"kind": "rooted_aline_service_table", **dispatcher_record})
        for entry, (states, accesses, _) in analyses.items():
            for access in accesses:
                target = address_value(access["source"])
                destination = address_value(access["address"])
                if (access["mode"] == "write" and access["width"] == 4 and destination is not None
                        and 0x400000 <= destination < 0x500000 and target is not None
                        and 0x400 <= target < len(rom) and not target & 1):
                    pointer_candidate(target, {"source_pc": access["pc"], "function": entry,
                                               "kind": "rooted_ram_code_pointer_candidate",
                                               "destination": destination,
                                               "certainty": "candidate_not_proven_callback"},
                                      access["source"], result)
            for pc, state in states.items():
                insn = result.instructions[pc]
                base = base_mnemonic(insn)
                if base in CALL_MNEMONICS | UNCOND_BRANCH_MNEMONICS and not result.branch_targets.get(pc):
                    op = insn.operands[-1] if insn.operands else None
                    if pc not in heuristic_tables:
                        heuristic_tables[pc] = heuristic_table_probe(rom, md, insn, result.instructions)
                    for probe in heuristic_tables[pc]:
                        pointer_candidate(probe["target"], {"source_pc": pc, "function": entry,
                                                           "kind": "rooted_jump_table_pointer_candidate",
                                                           "table_probe": probe,
                                                           "certainty": "heuristic_table_extent_not_proven_index_bound"},
                                          const(probe["target"]), result)
                    value = (state.get(reg_name(insn, op.reg)) if op and op.type == M68K_OP_REG
                             else operand_address(insn, op, state) if op and op.type == M68K_OP_MEM else None)
                    for resolved, chain in bind_callers(entry, value, callers, caller_depth):
                        target = address_value(resolved)
                        if target is not None and 0x400 <= target < len(rom) and not target & 1:
                            _require_included(exclusions, target, f"Tracked {insn.mnemonic} at {pc:#x}")
                            transfers[pc].add(target)
                            expansion_records.append({"entry": target, "source_pc": pc, "function": entry,
                                                      "kind": "tracked_indirect_transfer", "caller_chain": chain,
                                                      "instruction": evidence(insn),
                                                      "pointer_evidence_pcs": list(resolved.origins),
                                                      "certainty": "static_constant_target_on_documented_caller_path"})
                task_trap = base == "trap" and insn.operands and insn.operands[0].imm == 1
                if task_trap or base == "aline":
                    for name, value in state.items():
                        target = address_value(value)
                        if target is not None and 0x400 <= target < len(rom) and not target & 1:
                            pointer_candidate(target, {"source_pc": pc, "function": entry,
                                                       "kind": "rooted_exception_code_pointer_candidate",
                                                       "input_location": name, "exception": base,
                                                       "certainty": "candidate_task_abi_unproven"}, value, result)
                if task_trap and task_frame:
                    address = add(state.get("a7"), task_frame["callback_stack_offset"], pc)
                    key = stack_key(address)
                    callback = (state[key] if key in state else
                                operation("load", (address, const(4)), pc))
                    for resolved, chain in bind_callers(entry, callback, callers, caller_depth):
                        target = address_value(resolved)
                        if target is None or not 0x400 <= target < len(rom) or target & 1:
                            continue
                        _require_included(exclusions, target, f"Proved task PC from TRAP #1 at {pc:#x}")
                        if target in result.functions or target in known:
                            continue
                        new_entries[target] = {"entry": target, "source_pc": pc, "function": entry,
                                               "kind": "rooted_trap_task_target", "caller_chain": chain,
                                               "callback_stack_offset": task_frame["callback_stack_offset"],
                                               "pointer_evidence_pcs": list(resolved.origins),
                                               "instruction": evidence(insn), "task_frame": task_frame,
                                               "certainty": "callback_argument_proven_to_be_task_rte_pc"}
        can_expand = iteration < rounds
        for record in expansion_records + list(new_entries.values()):
            record.update(followed=can_expand,
                          disposition="followed_control_target" if can_expand else "root_expansion_bound_reached")
            origins.append(record)
        if dispatcher_record:
            for target in dispatcher_record["targets"]:
                _require_included(exclusions, target, f"Proved A-line service table at {dispatcher_record['call_pc']:#x}")
        for continuation in new_continuations:
            _require_included(exclusions, continuation, "Proved A-line continuation")
        if not can_expand or not new_entries and not transfers and not new_continuations:
            break
        known.update(new_entries)
        continuation_seeds.update(new_continuations)
        cfg.setdefault("entry_points", []).extend(sorted(new_entries.keys() | new_continuations))
        for pc, targets in transfers.items():
            cfg.setdefault("jump_tables", []).append({"address": pc, "targets": sorted(targets)})
    for record in origins:
        if record["kind"] == "rooted_aline_service_table":
            record["followed"] = set(record["targets"]) <= set(result.branch_targets.get(record["call_pc"], []))
            record["disposition"] = "followed_control_target" if record["followed"] else "root_expansion_bound_reached"
        elif record["kind"] == "rooted_aline_dispatcher_continuation":
            record["followed"] = record["entry"] in result.instructions
            record["disposition"] = "followed_continuation" if record["followed"] else "root_expansion_bound_reached"
    for record in candidate_records.values():
        record["target_is_independently_rooted"] = record["entry"] in result.instructions
    return result, successors, members, owners, analyses, origins


def normalized_function(pcs, instructions, rom_size, rename_registers, rom_pointer_pcs=frozenset()):
    names, rows = {}, []
    pc_indices = {pc: i for i, pc in enumerate(pcs)}
    relocations = {}
    def register(name):
        if name in {"a7", "sp", "pc", "sr", "ccr"} or not rename_registers:
            return name
        if name not in names:
            group = "a" if name.startswith("a") else "d" if name.startswith("d") else "r"
            names[name] = group + str(sum(value.startswith(group) for value in names.values()))
        return names[name]
    def address(number, role, force_rom=False):
        number &= 0xffffffff
        if region(number) or number >= rom_size:
            return number
        if force_rom or role == "memory":
            if number in pc_indices:
                return {"local_instruction": pc_indices[number]}
            key = (role, number)
            if key not in relocations:
                relocations[key] = len(relocations)
            return {"rom_reference": relocations[key], "role": role}
        # Fixed RAM/hardware addresses and all arithmetic immediates are retained.
        return number
    for pc in pcs:
        insn = instructions[pc]
        base = base_mnemonic(insn)
        row = {"instruction": insn.mnemonic, "operands": []}
        for op in insn.operands:
            item = {"type": op.type, "address_mode": op.address_mode}
            if base in CALL_MNEMONICS | COND_BRANCH_MNEMONICS | UNCOND_BRANCH_MNEMONICS:
                target = _resolve_target(insn, op)
                if target is not None:
                    item["target"] = address(target, "control_flow", True)
                    row["operands"].append(item)
                    continue
            if op.type == M68K_OP_REG:
                item["register"] = register(reg_name(insn, op.reg))
            elif op.type == M68K_OP_IMM:
                # Relocate only immediates whose producer feeds a proven ROM
                # effective address. Scale/flag/stride constants stay numeric.
                item["immediate"] = (address(op.imm, "pointer_immediate", True)
                                     if pc in rom_pointer_pcs and 0x400 <= op.imm < rom_size else op.imm)
            elif op.type == M68K_OP_MEM:
                if op.address_mode in {M68K_AM_ABSOLUTE_DATA_LONG, M68K_AM_ABSOLUTE_DATA_SHORT}:
                    item["address"] = address(op.imm, "memory")
                else:
                    item.update(base=register(reg_name(insn, op.mem.base_reg or op.reg)),
                                index=register(reg_name(insn, op.mem.index_reg)),
                                scale=op.mem.scale, index_size=op.mem.index_size)
                    full = full_base_extension(insn, op)
                    displacement = full["base_displacement"] if full else op.mem.disp
                    if full:
                        item["base_suppressed"] = full["base_suppressed"]
                        item["index_suppressed"] = full["index_suppressed"]
                        item["base_displacement_bytes"] = full["base_displacement_bytes"]
                    if full and full["base_suppressed"]:
                        item["address"] = address(displacement, "memory")
                    elif op.address_mode in PC_RELATIVE:
                        item["address"] = address(pc + 2 + displacement, "memory", True)
                    else:
                        item["displacement"] = displacement
                    # Do not reinsert a relocated base displacement through
                    # Capstone's raw inner-displacement field.
                    if op.address_mode in {M68K_AM_PC_MEMI_PRE_INDEX, M68K_AM_PC_MEMI_POST_INDEX}:
                        raw = bytes(insn.bytes)
                        extension = int.from_bytes(raw[2:4], "big")
                        count = {1: 0, 2: 2, 3: 4}.get((extension >> 4) & 3, 0)
                        displacement = int.from_bytes(raw[4:4 + count], "big", signed=True)
                        item.pop("displacement", None)
                        item["address"] = address((0 if extension & 0x80 else pc + 2) + displacement, "memory", True)
                        item["base_displacement_bytes"] = count
                        item["base_suppressed"] = bool(extension & 0x80)
                        item["index_suppressed"] = bool(extension & 0x40)
                    elif not full:
                        item["inner_displacement"] = op.mem.in_disp
                    item["outer_displacement"] = op.mem.out_disp
                    item["inner_base"] = register(reg_name(insn, op.mem.in_base_reg))
                    if op.mem.bitfield:
                        item["bitfield"] = {"width": op.mem.width, "offset": op.mem.offset}
            elif op.type == M68K_OP_REG_BITS:
                item["registers"] = [register(name) for i, name in enumerate(REGISTERS) if op.register_bits & (1 << i)]
            elif op.type == M68K_OP_REG_PAIR:
                item["registers"] = [register(reg_name(insn, op.reg_pair.reg_0)), register(reg_name(insn, op.reg_pair.reg_1))]
            else:
                item["raw"] = insn.op_str
            row["operands"].append(item)
        rows.append(row)
    encoded = json.dumps(rows, sort_keys=True, separators=(",", ":")).encode()
    return {"sha256": hashlib.sha256(encoded).hexdigest(), "instructions": rows,
            "register_map": names, "normalization": "register_renamed" if rename_registers else "abi_preserving"}


def serialize_access(access, instructions):
    return {**{key: value for key, value in access.items() if key not in {"address", "source"}},
            "address": access["address"].json() if access["address"] else None,
            "source": access["source"].json() if access["source"] else None,
            "source_semantics": "first_explicit_operand_not_rmw_result",
            "instruction": evidence(instructions[access["pc"]])}


def make_catalog(config_path, rom_root, root_rounds=6, caller_depth=4):
    """Public analysis API; loading validates the complete program chips."""
    import tomllib
    config_path, rom_root = Path(config_path), Path(rom_root)
    preview = tomllib.loads(config_path.read_text("utf-8"))
    game_id = preview["game"]["id"]
    # --rom-root is normally the containing directory; a single flat set is
    # also accepted when every configured lane physically exists there.
    candidates = [rom_root / game_id, rom_root]
    if rom_root.is_dir():
        candidates.extend(path for path in sorted(rom_root.iterdir()) if path.is_dir() and path not in candidates)
    config = preview
    rejected = []
    for rom_dir in candidates:
        if not all((rom_dir / lane["file"]).is_file() for lane in config["rom"]["lanes"]):
            continue
        try:
            rom = load_region(config, "rom", rom_dir)
        except ValueError as error:
            rejected.append({"directory": str(rom_dir), "reason": str(error)})
            continue
        break
    else:
        raise ValueError(f"No physical program-chip identity matches {config_path} under {rom_root}; rejected {rejected}")
    discovery, successors, members, owners, analyses, roots = rooted_discovery(rom, config, root_rounds, caller_depth)
    callers = function_callers(analyses, discovery, members)
    def bindings(entry, value, depth):
        return bind_callers(entry, value, callers, depth)
    routines = []
    for entry, pcs in members.items():
        states, accesses, _ = analyses[entry]
        associations, writers = [], []
        for access in accesses:
            value = access["address"]
            resolved = bindings(entry, value, caller_depth)
            for destination, chain in resolved:
                target_region = region(address_value(destination)) if destination.kind == "constant" else destination.name
                if not target_region:
                    continue
                item = serialize_access(access, discovery.instructions)
                item.update(region=target_region, resolved_address=destination.json(), caller_chain=chain,
                            certainty="proven_static_effective_address" if destination.kind == "constant" else "hardware_derived_cursor_range_unproven")
                associations.append(item)
                if access["mode"] in {"write", "read_write"}:
                    writers.append(item)
        constants = []
        for pc in pcs:
            insn = discovery.instructions[pc]
            for i, op in enumerate(insn.operands):
                number = op.imm if op.type == M68K_OP_IMM or op.address_mode in {M68K_AM_ABSOLUTE_DATA_LONG, M68K_AM_ABSOLUTE_DATA_SHORT} else None
                if region(number):
                    constants.append({"region": region(number), "operand": i, "value": number,
                                      "instruction": evidence(insn), "certainty": "constant_reference_not_a_write"})
        if not associations and not constants:
            # Include calibrated PCs only for actual Land Maker configs; this
            # is an assessment record, never a hardcoded graphics callback list.
            if not game_id.startswith("landmak") or not any(pc in pcs for pc in CALIBRATION):
                continue
        ranges = []
        for pc in pcs:
            end = pc + discovery.instructions[pc].size
            if ranges and ranges[-1][1] == pc:
                ranges[-1][1] = end
            else:
                ranges.append([pc, end])
        source_fields = []
        for access in accesses:
            if access["mode"] not in {"read", "read_write"}:
                continue
            address = access["address"]
            if address is None:
                continue
            if input_symbols(address) or (address.kind == "constant" and 0x400000 <= (address.number & 0xffffff) < 0x500000):
                source_fields.append({**serialize_access(access, discovery.instructions),
                                      "interpretation": "entry_stack_field" if address.kind == "input" and address.name == "a7" else
                                                        "entry_register_relative_field" if address.kind == "input" else
                                                        "entry_register_dynamic_field_offset_unproven" if input_symbols(address) else "absolute_work_ram_field",
                                      "semantic_role": "unknown_not_inferred"})
        mutations, saves = [], []
        for pc in pcs:
            insn = discovery.instructions[pc]
            base = base_mnemonic(insn)
            if base in {"addq", "subq", "adda", "suba", "lea"} or any(op.address_mode in {M68K_AM_REGI_ADDR_POST_INC, M68K_AM_REGI_ADDR_PRE_DEC} for op in insn.operands):
                mutations.append(evidence(insn))
            if base in {"movem", "link", "unlk", "pea"}:
                saves.append(evidence(insn))
        register_reads, abi_symbols, rom_pointer_pcs = [], set(), set()
        for access in accesses:
            rom_pointer_pcs.update(rom_pointer_origins(access["address"], len(rom)))
        for pc in pcs:
            insn = discovery.instructions[pc]
            modes = access_modes(insn)
            for i, op in enumerate(insn.operands):
                if op.type == M68K_OP_MEM:
                    value = operand_address(insn, op, states.get(pc, {}))
                elif op.type == M68K_OP_REG and modes[i] in {"read", "read_write", "address"}:
                    value = operand_value(insn, op, states.get(pc, {}))
                else:
                    continue
                symbols = input_symbols(value)
                abi_symbols.update(symbols)
                if symbols:
                    register_reads.append({"pc": pc, "operand": i, "entry_registers": sorted(symbols),
                                           "term": value.json(), "instruction": evidence(insn)})
        abi_inputs = sorted(abi_symbols)
        routines.append({"entry": entry, "status": "graphics_writer_candidate" if writers else "graphics_reference_candidate",
                         "driver_supported": False, "bounds": {"min_pc": min(pcs), "max_end": max(pc + discovery.instructions[pc].size for pc in pcs),
                         "ranges": ranges, "kind": "reachable_cfg_membership_not_symbol_extent"},
                         "instructions": [evidence(discovery.instructions[pc]) for pc in pcs],
                         "cfg": [{"pc": pc, "successors": successors[pc], "transfer_targets": discovery.branch_targets.get(pc, [])} for pc in pcs],
                         "callers": [{"function": caller["function"], "pc": caller["pc"], "kind": caller["kind"],
                                      "caller_frame": caller["caller_frame"],
                                      "instruction": evidence(discovery.instructions[caller["pc"]]),
                                      "register_arguments": {name: value.json() for name, value in caller["state"].items() if value is not None and name in REGISTERS},
                                      "stack_arguments": [{"caller_entry_sp_offset": int(name.split(":", 1)[1]), "value": value.json()}
                                                          for name, value in caller["state"].items() if value is not None and name.startswith("stack:")]}
                                     for caller in callers.get(entry, [])],
                         "constant_references": constants, "memory_associations": associations, "writers": writers,
                         "source_fields": source_fields, "abi": {"entry_symbols": abi_inputs, "entry_register_reads": register_reads, "stack_and_save_instructions": saves,
                         "call_clobber_policy": "all_general_registers_except_balanced_a7_unknown",
                         "argument_roles": "source_fields_prove_reads_not_semantic_descriptor_roles"},
                         "pointer_and_stride_evidence": mutations,
                         "signatures": {"relocation_abi": normalized_function(pcs, discovery.instructions, len(rom), False, rom_pointer_pcs),
                                        "relocation_register_renamed": normalized_function(pcs, discovery.instructions, len(rom), True, rom_pointer_pcs)}})
    calibration = []
    if game_id.startswith("landmak"):
        for pc, name in CALIBRATION.items():
            calibration.append({"pc": pc, "name": name, "decoded": pc in discovery.instructions,
                                "owning_entries": owners.get(pc, []),
                                "catalog_entries": [r["entry"] for r in routines if pc in members[r["entry"]]],
                                "instruction": evidence(discovery.instructions[pc]) if pc in discovery.instructions else None})
    return {"game": config["game"], "config": str(config_path), "rom_directory": str(rom_dir),
            "program_sha256": hashlib.sha256(rom).hexdigest(), "rom_size": len(rom),
            "program_chips": config["rom"]["lanes"],
            "rom_directory_selection": {"basis": "full_physical_program_chip_hash_identity", "rejected_candidates": rejected},
            "discovery_configuration": {"original": config.get("discovery", {}),
                                        "forced_mode": "recursive", "disabled_aligned_seed_scans": ["task_traps", "callbacks", "actor_scripts"],
                                        "heuristic_jump_table_policy": "unfollowed_candidates_not_code_roots"},
            "discovery": discovery.report,
            "root_expansion_evidence": roots, "routines": routines, "calibration": calibration,
            "limits": {"root_expansion_rounds": root_rounds, "caller_binding_depth": caller_depth,
                       "pointer_candidates_are_not_code_entries": True,
                       "unfollowed_pointer_candidates": sum(item.get("disposition") == "unfollowed_pointer_candidate" for item in roots),
                       "excluded_pointer_candidates": sum(item.get("target_exclusion") is not None for item in roots),
                       "unexpanded_proved_root_records": sum(item.get("disposition") == "root_expansion_bound_reached" for item in roots),
                       "unproved_task_abis_are_not_followed": True,
                       "unresolved_transfers_are_not_absence_of_graphics": True,
                       "descriptor_packing_and_semantics_require_instruction_review": True}}


def cross_game_signatures(catalogs):
    groups = defaultdict(list)
    for catalog in catalogs:
        for routine in catalog["routines"]:
            for mode, signature in routine["signatures"].items():
                groups[mode, signature["sha256"]].append({"game": catalog["game"]["id"], "entry": routine["entry"],
                                                        "regions": sorted({item["region"] for item in routine["memory_associations"] + routine["constant_references"]})})
    return [{"mode": mode, "sha256": digest, "members": members,
             "cross_game": len({member["game"] for member in members}) > 1,
             "interpretation": "instruction_family_only_not_descriptor_abi_compatibility"}
            for (mode, digest), members in sorted(groups.items()) if len(members) > 1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", action="append", required=True, type=Path,
                        help="Game TOML; repeat for cross-game analysis")
    parser.add_argument("--rom-root", required=True, type=Path, help="Containing set directory or one flat ROM directory")
    parser.add_argument("--output", type=Path, help="JSON output file; default stdout")
    parser.add_argument("--root-rounds", type=int, default=6, help="Bound proved control/task-target expansion; pointer candidates stay unfollowed")
    parser.add_argument("--caller-depth", type=int, default=4, help="Bound symbolic MMIO argument rebinding")
    args = parser.parse_args()
    if args.root_rounds < 0 or args.caller_depth < 0:
        parser.error("analysis bounds must be nonnegative")
    catalogs = [make_catalog(path, args.rom_root, args.root_rounds, args.caller_depth) for path in args.config]
    document = {"schema_version": SCHEMA_VERSION, "tool": "rooted_f3_graphics_discovery",
                "address_encoding": "integer_bytes", "hardware_regions": [{"name": name, "start": start, "end": end} for name, start, end in REGIONS],
                "catalogs": catalogs, "cross_game_signatures": cross_game_signatures(catalogs)}
    text = json.dumps(document, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
