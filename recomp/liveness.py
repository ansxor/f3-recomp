"""Backward CCR-flag liveness over the instruction graph the recompiler emits.

Condition codes are lazy (see ``include/f3rt/cpu_abi.h``), so a flag producer is
only worth emitting when some successor can observe one of the flags it writes.
For each instruction this module derives the CCR flags it *reads* and *must
write* from the 68020 semantics (and from what ``emitter.lower`` implements),
then solves

    live_out(i) = U live_in(s) for each successor s       (ALL at a boundary)
    live_in(i)  = reads(i) | (live_out(i) - writes(i))

to a fixpoint. Flags are the bits of ``cpu->sr`` (``CCR_*`` in the emitter).

Soundness rules the table below relies on:

* ``writes`` is a *must* set: flags the instruction sets on every path. A
  conditional write (e.g. X of a register-count shift) is a read instead, which
  keeps the flag live through the instruction.
* A flag an instruction preserves is not in ``writes``, so it stays live across
  the instruction without being listed in ``reads``.
* Anything that can raise an exception (privilege, CHK, divide, TRAPcc, illegal,
  line A/F...) reads every flag: the exception frame stacks SR.
* Anything unrecognised reads every flag and ends the analysed graph.
* Control leaving the graph - return, computed jump/call, a successor that is
  not an emitted instruction, hook PCs that flush for a sandbox - keeps every flag
  live. A known ``jsr``/``bsr`` flows only into its callee; the return point is
  reached through the callee's ``rts``, which is a boundary.
* Interrupts resume at the same PC and the runtime flushes at IRQ entry, so they
  add no liveness.
"""
from __future__ import annotations

from collections import deque
from typing import Iterable, Mapping, NamedTuple

import capstone.m68k as m68k
from capstone import CsInsn

from .emitter import (CCR_ALL, CCR_C, CCR_N, CCR_NZVC, CCR_V, CCR_X, CCR_Z, COND_MAP,
                      _get_base_mnemonic, static_flow)


class FlagEffects(NamedTuple):
    """CCR flags read / unconditionally written by one instruction.

    ``opaque`` marks instructions whose control flow is not described by
    ``static_flow`` (unknown mnemonic): they end the analysed graph.
    """
    reads: int
    writes: int
    opaque: bool = False


NO_FLAGS = FlagEffects(0, 0)
READS_ALL = FlagEffects(CCR_ALL, 0)
OPAQUE = FlagEffects(CCR_ALL, 0, True)
LOGIC = FlagEffects(0, CCR_NZVC)          # N,Z from result; V,C cleared; X kept
ARITH = FlagEffects(0, CCR_ALL)           # ADD/SUB/NEG: X,N,Z,V,C
EXTENDED = FlagEffects(CCR_X | CCR_Z, CCR_ALL)   # ADDX/SUBX/NEGX/xBCD: X in, Z sticky

# Flags each 4-bit condition tests, indexed like f3_eval_cond / COND_MAP.
_COND_FLAGS = (
    0, 0,                            # T, F
    CCR_C | CCR_Z, CCR_C | CCR_Z,    # HI, LS
    CCR_C, CCR_C,                    # CC, CS
    CCR_Z, CCR_Z,                    # NE, EQ
    CCR_V, CCR_V,                    # VC, VS
    CCR_N, CCR_N,                    # PL, MI
    CCR_N | CCR_V, CCR_N | CCR_V,    # GE, LT
    CCR_N | CCR_V | CCR_Z, CCR_N | CCR_V | CCR_Z,   # GT, LE
)

# Neither read nor write CCR. Returns/jumps are boundaries via static_flow.
_NO_FLAGS = frozenset((
    'nop', 'bra', 'bsr', 'jmp', 'jsr', 'rts', 'rtd', 'lea', 'pea', 'link', 'unlk',
    'movea', 'adda', 'suba', 'exg', 'movem', 'movep',
))
# Exception-capable or flag-snapshotting: stacked/stored SR exposes every flag.
_READS_ALL = frozenset((
    'reset', 'chk', 'chk2', 'cmp2', 'trapv', 'trap', 'moves', 'movec', 'rte', 'rtr',
    'stop', 'divu', 'divs', 'illegal',
))
# N,Z from the result, V and C cleared (multiply sets V for the 32-bit form),
# X preserved. CAS/CAS2/TAS flush and produce through the same lazy ops.
_LOGIC = frozenset((
    'moveq', 'ext', 'extb', 'swap', 'clr', 'tst', 'not', 'tas', 'mulu', 'muls',
    'cas', 'cas2', 'cmp', 'cmpa', 'cmpi', 'cmpm',
    'bftst', 'bfextu', 'bfchg', 'bfexts', 'bfclr', 'bfffo', 'bfset', 'bfins',
))
_BIT_OPS = frozenset(('btst', 'bset', 'bclr', 'bchg'))        # Z only
_ARITH_DST_AN = frozenset(('add', 'addi', 'addq', 'sub', 'subi', 'subq'))
_LOGIC_IMM = frozenset(('and', 'andi', 'or', 'ori', 'eor', 'eori'))
_SHIFT_NOX = frozenset(('asl', 'asr', 'lsl', 'lsr'))
_ROTATE = frozenset(('rol', 'ror'))
_ROTATE_X = frozenset(('roxl', 'roxr'))


def condition_flags(cond: int) -> int:
    """CCR flags tested by condition code ``cond`` (COND_MAP encoding)."""
    return _COND_FLAGS[cond & 15]


def _is_areg(op) -> bool:
    return op.type == m68k.M68K_OP_REG and m68k.M68K_REG_A0 <= op.reg <= m68k.M68K_REG_A7


def _shift_effects(mnem: str, ops) -> FlagEffects:
    # Memory shifts (one operand) always shift a word by one. A register shift
    # whose count is zero modulo 64 preserves X, so X is a conditional write.
    counted = len(ops) == 1 or (ops[0].type == m68k.M68K_OP_IMM and ops[0].imm & 63)
    if mnem in _ROTATE:
        return LOGIC                                  # X is never affected
    if mnem in _ROTATE_X:
        return FlagEffects(CCR_X, CCR_ALL if counted else CCR_NZVC)
    return ARITH if counted else FlagEffects(CCR_X, CCR_NZVC)


def _move_effects(ops) -> FlagEffects:
    if len(ops) < 2:
        return OPAQUE
    kinds = []
    for op in ops[:2]:
        if op.type == m68k.M68K_OP_REG:
            if m68k.M68K_REG_D0 <= op.reg <= m68k.M68K_REG_D7:
                kinds.append('d')
            elif m68k.M68K_REG_A0 <= op.reg <= m68k.M68K_REG_A7:
                kinds.append('a')
            elif op.reg == m68k.M68K_REG_CCR:
                kinds.append('ccr')
            elif op.reg == m68k.M68K_REG_SR:
                kinds.append('sr')
            elif op.reg == m68k.M68K_REG_USP:
                kinds.append('usp')
            else:
                return OPAQUE
        else:
            kinds.append('ea')
    source, destination = kinds
    if source in ('ccr', 'sr', 'usp') or destination in ('sr', 'usp'):
        return READS_ALL        # MOVE from CCR/SR; privileged forms raise an exception
    if destination == 'ccr':
        return FlagEffects(0, CCR_ALL)
    if destination == 'a':
        return NO_FLAGS
    return LOGIC


def _logic_effects(mnem: str, ops) -> FlagEffects:
    if len(ops) < 2:
        return OPAQUE
    destination = ops[1]
    if destination.type == m68k.M68K_OP_REG and destination.reg in (m68k.M68K_REG_CCR,
                                                                      m68k.M68K_REG_SR):
        if destination.reg == m68k.M68K_REG_SR:
            return READS_ALL    # privileged; also rewrites mode/mask bits
        imm = ops[0].imm & CCR_ALL
        if 'eor' in mnem:
            return FlagEffects(imm, 0)                 # toggles: depends on old value
        if 'and' in mnem:
            return FlagEffects(imm, ~imm & CCR_ALL)    # kept where imm=1, cleared where 0
        return FlagEffects(~imm & CCR_ALL, imm)        # ORI: set where imm=1, kept where 0
    return LOGIC


def _bcd_effects(opcode: int) -> FlagEffects:
    # Capstone labels memory UNPK as SBCD; the primary word is unambiguous.
    form = opcode & 0xf1f0
    if form in (0xc100, 0x8100):
        return EXTENDED
    if form in (0x8140, 0x8180):
        return NO_FLAGS          # PACK/UNPK leave the CCR alone
    return OPAQUE


def flag_effects(insn: CsInsn) -> FlagEffects:
    """CCR flags ``insn`` reads and unconditionally writes (see module doc)."""
    opcode = int.from_bytes(insn.bytes[:2], "big")
    if opcode >> 12 in (10, 15) or opcode == 0x4afc or opcode & 0xfff8 == 0x4848:
        return READS_ALL        # line A/F, ILLEGAL, BKPT: exception stacks SR
    if getattr(insn, 'id', 0) == 0:
        return OPAQUE
    try:
        ops = insn.operands
    except Exception:
        return OPAQUE
    mnem = _get_base_mnemonic(insn)

    if mnem in _NO_FLAGS:
        return NO_FLAGS
    if mnem in _READS_ALL or mnem == 'trapv' or (mnem.startswith('trap') and mnem[4:] in COND_MAP):
        return READS_ALL
    if mnem in _LOGIC:
        return LOGIC
    if mnem == 'move':
        return _move_effects(ops)
    if mnem in _ARITH_DST_AN:
        if len(ops) < 2:
            return OPAQUE
        return NO_FLAGS if _is_areg(ops[1]) else ARITH     # ADDQ/SUBQ to An: no flags
    if mnem == 'neg':
        return ARITH
    if mnem in ('negx', 'addx', 'subx', 'nbcd'):
        return EXTENDED
    if mnem in ('abcd', 'sbcd', 'pack', 'unpk'):
        return _bcd_effects(opcode)
    if mnem in _LOGIC_IMM:
        return _logic_effects(mnem, ops)
    if mnem in _BIT_OPS:
        return FlagEffects(0, CCR_Z)
    if mnem in _SHIFT_NOX or mnem in _ROTATE or mnem in _ROTATE_X:
        return _shift_effects(mnem, ops) if ops else OPAQUE
    # Branches, DBcc, Scc: exactly the flags the condition tests. DBcc/Scc use
    # their own COND_MAP names (st/sf and dbra name conditions T/F).
    if mnem.startswith('b') and mnem[1:] in COND_MAP:
        return FlagEffects(_COND_FLAGS[COND_MAP[mnem[1:]]], 0)
    if mnem.startswith('db') and mnem[2:] in COND_MAP:
        return FlagEffects(_COND_FLAGS[COND_MAP[mnem[2:]]], 0)
    if mnem in ('st', 'sf'):
        return NO_FLAGS
    if mnem.startswith('s') and mnem[1:] in COND_MAP:
        return FlagEffects(_COND_FLAGS[COND_MAP[mnem[1:]]], 0)
    return OPAQUE


class Liveness:
    """Per-instruction CCR liveness over a set of emitted instruction PCs.

    ``live_out[pc]`` is what ``lower(insn, live_out)`` needs. ``effects``,
    ``succs`` and ``preds`` expose the graph so an emitter pass can also ask
    which producers reach a consumer (``reaching_writers``).
    """

    def __init__(self, effects, succs, preds, live_in, live_out):
        self.effects: dict[int, FlagEffects] = effects
        self.succs: dict[int, tuple[int, ...]] = succs
        self.preds: dict[int, list[int]] = preds
        self.live_in: dict[int, int] = live_in
        self.live_out: dict[int, int] = live_out

    def reaching_writers(self, pc: int, flags: int) -> tuple[frozenset[int], bool]:
        """Instructions whose write of any of ``flags`` may reach ``pc``.

        Walks predecessors until an instruction that must-write ``flags`` is
        found on every path. The flag is *open* (second element True) if some
        path reaches an instruction with no known predecessor or one that does
        not write every flag asked for: the value then comes from a state this
        graph cannot see (entry from the dispatcher, a return, an unknown caller).
        """
        writers: set[int] = set()
        open_path = False
        seen = set()
        stack = [(pred, flags) for pred in self.preds.get(pc, ())]
        if not stack:
            return frozenset(), True
        while stack:
            node, wanted = stack.pop()
            if (node, wanted) in seen:
                continue
            seen.add((node, wanted))
            written = self.effects[node].writes & wanted
            if written:
                writers.add(node)
            wanted &= ~self.effects[node].writes
            if not wanted:
                continue
            preds = self.preds.get(node)
            if not preds:
                open_path = True
                continue
            stack.extend((pred, wanted) for pred in preds)
        return frozenset(writers), open_path


def analyse(instructions: Mapping[int, CsInsn], nodes: Iterable[int] | None = None,
            pinned: Iterable[int] = ()) -> Liveness:
    """Solve flag liveness for ``nodes`` (default: every decoded instruction).

    Successors outside ``nodes`` are not executed natively, so the instruction
    flowing into one keeps every flag live. ``pinned`` PCs read every flag
    (emit-unit hooks and fallback stubs observe a flushed sr).
    """
    node_set = set(instructions) if nodes is None else set(nodes)
    pinned = set(pinned)
    effects: dict[int, FlagEffects] = {}
    succs: dict[int, tuple[int, ...]] = {}
    preds: dict[int, list[int]] = {pc: [] for pc in node_set}
    base_out: dict[int, int] = {}
    for pc in node_set:
        insn = instructions[pc]
        effect = flag_effects(insn)
        if pc in pinned:
            effect = effect._replace(reads=CCR_ALL)
        effects[pc] = effect
        flow = static_flow(insn)
        boundary = flow.escapes or effect.opaque
        targets = []
        candidates = list(flow.targets)
        if flow.falls_through:
            candidates.append(pc + insn.size)
        for target in candidates:
            if target in node_set:
                if target not in targets:
                    targets.append(target)
            else:
                boundary = True
        succs[pc] = tuple(targets)
        base_out[pc] = CCR_ALL if boundary else 0
        for target in targets:
            preds[target].append(pc)

    live_in = {pc: effects[pc].reads for pc in node_set}
    live_out = dict(base_out)
    work = deque(sorted(node_set, reverse=True))
    queued = set(node_set)
    while work:
        pc = work.popleft()
        queued.discard(pc)
        out = base_out[pc]
        for target in succs[pc]:
            out |= live_in[target]
        live_out[pc] = out
        effect = effects[pc]
        new_in = effect.reads | (out & ~effect.writes)
        if new_in != live_in[pc]:
            live_in[pc] = new_in
            for pred in preds[pc]:
                if pred not in queued:
                    queued.add(pred)
                    work.append(pred)
    return Liveness(effects, succs, preds, live_in, live_out)
