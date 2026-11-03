"""Exact fast-forward of idle loops: loops that burn cycles until a device deadline.

A game spends most of a frame spinning in a few tiny loops that wait for the next
interrupt. Natively each iteration costs a handful of guarded instructions (and,
across subroutine calls, several dispatcher round trips), yet nothing observable
changes except time. This module recognises three loop shapes from decoded
instructions alone and plants an ``F3_IDLE_*`` call (recomp/cpu_ops.h) at the loop
head that skips whole iterations up to the dispatch deadline:

* spin:   ``bra *``
* poll:   ``tst.size <ram>`` closed by ``Bcc head`` or ``DBcc Dn,head``
* affine: ``jsr`` chain that advances one RAM longword by x -> m*x + a
          (an LCG random-number generator) and branches back to the head

Exactness rests on three properties that the shapes guarantee and cpu_ops.h
enforces at run time: the skipped iterations touch nothing the loop's next
iteration reads except what the helper advances itself (the poll counter, the
RNG word); every register, flag and lazy-flag field they write is rewritten by a
complete iteration from the RNG word or memory alone; and at least one complete
iteration is left to native execution, so those fields hold exactly what real
execution leaves and the deadline lands on the same instruction.

Every instruction on the loop's path must be emitted natively (retained, lowered,
no emit-unit hook), otherwise the loop is left alone.
"""

from __future__ import annotations

import re
from collections.abc import Callable, Mapping

import capstone.m68k as m68k
from capstone import CsInsn

from .emitter import COND_MAP, _ea_extension_bytes, _get_base_mnemonic, _get_size, _pc_base, static_flow
from .timing import instruction_cycles

_MASK = 0xFFFFFFFF
_MAX_PATH = 32
_RETURN_ADDRESS_BYTES = 4

# Memory operands whose address is a function of address registers only, so it
# cannot change while the loop runs (a loop may modify just one data register).
_INVARIANT_MODES = (
    m68k.M68K_AM_REGI_ADDR,
    m68k.M68K_AM_REGI_ADDR_DISP,
    m68k.M68K_AM_ABSOLUTE_DATA_SHORT,
    m68k.M68K_AM_ABSOLUTE_DATA_LONG,
)
_AFFINE_ADD = {"addq": 1, "addi": 1, "add": 1, "subq": -1, "subi": -1, "sub": -1}
_AFFINE_MUL = ("muls", "mulu")


def _cycles(insn: CsInsn) -> int:
    """Charge of one execution; a taken Bcc.b costs more than a fall-through."""
    text = instruction_cycles(insn, _pc_base, _ea_extension_bytes)
    conditional = re.fullmatch(r"\(cpu->pc == 0x[0-9a-f]+u \? (\d+)u : (\d+)u\)", text)
    return int(conditional.group(2) if conditional else re.fullmatch(r"(\d+)u", text).group(1))


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
    return [f"F3_IDLE_SPIN(cpu, {_cycles(insn)}u);"]


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
        condition, loop_when_true = COND_MAP[mnemonic[2:]], 0
        counter = _data_register(closing.operands[0])
    elif mnemonic.startswith("b") and mnemonic[1:] in COND_MAP and mnemonic not in ("bra", "bsr"):
        condition, loop_when_true, counter = COND_MAP[mnemonic[1:]], 1, -1
    else:
        return None
    cycles = _cycles(insn) + _cycles(closing)
    return [f"F3_IDLE_POLL(cpu, {address}, {size}, {condition}, {loop_when_true}, {counter}, {cycles}u);"]


def _call_path(instructions: Mapping[int, CsInsn], head: int) -> tuple[list[CsInsn], int] | None:
    """Straight-line execution from head back to head through direct calls and
    returns, and the deepest call nesting reached."""
    path, stack, depth, pc = [], [], 0, head
    while len(path) < _MAX_PATH:
        insn = instructions.get(pc)
        if insn is None:
            return None
        path.append(insn)
        mnemonic = _get_base_mnemonic(insn)
        flow = static_flow(insn)
        if mnemonic == "jsr" and len(flow.targets) == 1:
            stack.append(insn.address + insn.size)
            depth = max(depth, len(stack))
            pc = flow.targets[0]
        elif mnemonic == "rts" and stack:
            pc = stack.pop()
        elif mnemonic == "bra":
            return (path, depth) if _target(insn) == head and not stack else None
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
    path, depth = called
    body = [insn for insn in path if _get_base_mnemonic(insn) not in ("jsr", "rts", "bra")]
    # move.l ea,Dn ; (mul/add/sub of an immediate on Dn)+ ; move.l Dn,ea ; (swap Dn)*
    load = body[0] if body else None
    if load is None or _get_base_mnemonic(load) != "move" or _get_size(load) != 4 or len(load.operands) != 2:
        return None
    register, address = _data_register(load.operands[1]), _address(load.operands[0], allow_stack=False)
    if register is None or address is None:
        return None
    multiplier, addend, index = 1, 0, 1
    while index < len(body):
        insn = body[index]
        mnemonic, operands = _get_base_mnemonic(insn), insn.operands
        if _get_size(insn) != 4 or len(operands) < 2 or _data_register(operands[1]) != register:
            break
        if operands[0].type != m68k.M68K_OP_IMM:
            break
        immediate = operands[0].imm & _MASK
        if mnemonic in _AFFINE_ADD:
            addend = (addend + _AFFINE_ADD[mnemonic] * immediate) & _MASK
        elif mnemonic in _AFFINE_MUL and not int.from_bytes(insn.bytes[2:4], "big") & 0x0400:
            # 32-bit result form only: the low word of the product is the same
            # for signed and unsigned operands.
            multiplier, addend = multiplier * immediate & _MASK, addend * immediate & _MASK
        else:
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
    cycles = sum(_cycles(insn) for insn in path)
    stack = depth * _RETURN_ADDRESS_BYTES
    return path, [f"F3_IDLE_AFFINE_CALL(cpu, {address}, {stack}u, {multiplier}u, {addend}u, {cycles}u);"]


def find_idle_loops(
    instructions: Mapping[int, CsInsn], native: Callable[[int], bool]
) -> dict[int, tuple[str, list[str]]]:
    """Loop-head pc -> (shape, statements to plant at the head label).

    ``native(pc)`` tells whether an instruction is emitted as plain native code
    (retained, lowered and without hooks); a loop that touches any other
    instruction is not fast-forwarded.
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
