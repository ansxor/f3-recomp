"""Idle-loop yields: loops that only wait for an interrupt end the scheduling slice.

A game spends most of a frame spinning in a few tiny loops that wait for the next
interrupt. Nothing those loops do is observable except that they keep waiting, so
the scheduler need not run them at all: the CPU is modelled as infinitely fast,
and a waiting CPU skips straight to the next device event. This module recognises
three loop shapes from decoded instructions alone and plants an ``F3_IDLE*`` call
(recomp/cpu_ops.h) at the loop head that calls f3_idle when the loop would wait:

* spin:   ``bra *``
* poll:   ``tst.size <ram>`` closed by ``Bcc head`` or ``DBcc Dn,head``
* affine: ``jsr`` chain that advances one RAM longword by x -> m*x + a
          (an LCG random-number generator) and branches back to the head

Only an interrupt handler can change what such a loop reads, so the yield never
changes the result of the wait. The loop still runs one native iteration per
yield: a DBcc counter or an RNG word advances once per device event rather than
once per iteration of real hardware.

Every instruction on the loop's path must be emitted natively (retained, lowered,
no emit-unit hook), otherwise the loop is left alone.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping

import capstone.m68k as m68k
from capstone import CsInsn

from .emitter import COND_MAP, _get_base_mnemonic, _get_size, static_flow

_MASK = 0xFFFFFFFF
_MAX_PATH = 32

_INVARIANT_MODES = (
    m68k.M68K_AM_REGI_ADDR,
    m68k.M68K_AM_REGI_ADDR_DISP,
    m68k.M68K_AM_ABSOLUTE_DATA_SHORT,
    m68k.M68K_AM_ABSOLUTE_DATA_LONG,
)
_AFFINE_ADD = ("addq", "addi", "add", "subq", "subi", "sub")
_AFFINE_MUL = ("muls", "mulu")


def _address(op, allow_stack: bool = True) -> str | None:
    """C expression of an invariant memory operand's address, or None."""
    mode = op.address_mode
    if mode not in _INVARIANT_MODES:
        return None
    if mode == m68k.M68K_AM_ABSOLUTE_DATA_SHORT:
        value = op.imm & 0xFFFF
        return f"0x{(value - 0x10000 if value & 0x8000 else value) & _MASK:08x}u"
    if mode == m68k.M68K_AM_ABSOLUTE_DATA_LONG:
        return f"0x{op.imm & _MASK:08x}u"
    register = (op.reg if mode == m68k.M68K_AM_REGI_ADDR else op.mem.base_reg) - m68k.M68K_REG_A0
    if not allow_stack and register == 7:
        return None
    if mode == m68k.M68K_AM_REGI_ADDR:
        return f"cpu->a[{register}]"
    return f"(uint32_t)(cpu->a[{register}] + (int32_t)({op.mem.disp}))"


def _target(insn: CsInsn) -> int | None:
    """The single static destination of a direct branch, call or jump."""
    flow = static_flow(insn)
    return flow.targets[0] if len(flow.targets) == 1 and not flow.escapes else None


def _spin(insn: CsInsn) -> list[str] | None:
    if _get_base_mnemonic(insn) != "bra" or _target(insn) != insn.address:
        return None
    return ["F3_IDLE(cpu);"]


def _poll(instructions: Mapping[int, CsInsn], insn: CsInsn) -> list[str] | None:
    if _get_base_mnemonic(insn) != "tst" or len(insn.operands) != 1:
        return None
    size = _get_size(insn)
    address = _address(insn.operands[0])
    closing = instructions.get(insn.address + insn.size)
    if address is None or closing is None or _target(closing) != insn.address:
        return None
    mnemonic = _get_base_mnemonic(closing)
    if mnemonic.startswith("db") and mnemonic[2:] in COND_MAP and _data_register(closing.operands[0]) is not None:
        counter = _data_register(closing.operands[0])
        return [f"F3_IDLE_POLL_DBCC(cpu, {address}, {size}, {COND_MAP[mnemonic[2:]]}, {counter});"]
    if mnemonic.startswith("b") and mnemonic[1:] in COND_MAP and mnemonic not in ("bra", "bsr"):
        return [f"F3_IDLE_POLL_BCC(cpu, {address}, {size}, {COND_MAP[mnemonic[1:]]});"]
    return None


def _call_path(instructions: Mapping[int, CsInsn], head: int) -> list[CsInsn] | None:
    """Straight-line execution from head back to head through direct calls and returns."""
    path, stack, pc = [], [], head
    while len(path) < _MAX_PATH:
        insn = instructions.get(pc)
        if insn is None:
            return None
        path.append(insn)
        mnemonic = _get_base_mnemonic(insn)
        flow = static_flow(insn)
        if mnemonic == "jsr" and len(flow.targets) == 1:
            stack.append(insn.address + insn.size)
            pc = flow.targets[0]
        elif mnemonic == "rts" and stack:
            pc = stack.pop()
        elif mnemonic == "bra":
            return path if _target(insn) == head and not stack else None
        elif flow.targets or flow.escapes:
            return None
        else:
            pc = insn.address + insn.size
    return None


def _data_register(op) -> int | None:
    if op.type == m68k.M68K_OP_REG and m68k.M68K_REG_D0 <= op.reg <= m68k.M68K_REG_D7:
        return op.reg - m68k.M68K_REG_D0
    return None


def _affine(instructions: Mapping[int, CsInsn], head: int) -> tuple[list[CsInsn], list[str]] | None:
    first = instructions[head]
    if _get_base_mnemonic(first) != "jsr":
        return None
    called = _call_path(instructions, head)
    if called is None:
        return None
    path = called
    body = [insn for insn in path if _get_base_mnemonic(insn) not in ("jsr", "rts", "bra")]
    load = body[0] if body else None
    if load is None or _get_base_mnemonic(load) != "move" or _get_size(load) != 4 or len(load.operands) != 2:
        return None
    register, address = _data_register(load.operands[1]), _address(load.operands[0], allow_stack=False)
    if register is None or address is None:
        return None
    index = 1
    while index < len(body):
        insn = body[index]
        mnemonic, operands = _get_base_mnemonic(insn), insn.operands
        if _get_size(insn) != 4 or len(operands) < 2 or _data_register(operands[1]) != register:
            break
        if operands[0].type != m68k.M68K_OP_IMM:
            break
        multiply = mnemonic in _AFFINE_MUL and not int.from_bytes(insn.bytes[2:4], "big") & 0x0400
        if mnemonic not in _AFFINE_ADD and not multiply:
            break
        index += 1
    if index == 1 or index >= len(body):
        return None
    store = body[index]
    if (
        _get_base_mnemonic(store) != "move"
        or _get_size(store) != 4
        or len(store.operands) != 2
        or _data_register(store.operands[0]) != register
        or _address(store.operands[1], allow_stack=False) != address
    ):
        return None
    if any(
        _get_base_mnemonic(insn) != "swap" or _data_register(insn.operands[0]) != register for insn in body[index + 1 :]
    ):
        return None
    return path, ["F3_IDLE(cpu);"]


def find_idle_loops(
    instructions: Mapping[int, CsInsn], native: Callable[[int], bool]
) -> dict[int, tuple[str, list[str]]]:
    """Loop-head pc -> (shape, statements to plant at the head label).

    ``native(pc)`` tells whether an instruction is emitted as plain native code
    (retained, lowered and without hooks); a loop that touches any other
    instruction never yields.
    """
    loops = {}
    for pc, insn in instructions.items():
        if (statements := _spin(insn)) is not None:
            shape, path = "spin", [insn]
        elif (statements := _poll(instructions, insn)) is not None:
            shape, path = "poll", [insn, instructions[pc + insn.size]]
        elif (found := _affine(instructions, pc)) is not None:
            shape, (path, statements) = "affine", found
        else:
            continue
        if all(native(step.address) for step in path):
            loops[pc] = (shape, statements)
    return loops
