"""f3a command line: one subcommand per question a reverse engineer asks.

Every command prints a short header, then fixed-column rows (addresses always 0x + 6 hex
digits), and accepts --json for one JSON object per row. Output is bounded by --limit.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict, deque
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zlib

from .game import (DISPATCH, LOG_LAYER, REGIONS, ROOT, VECTOR_NAMES, VIDEO_REGIONS, Game, access_size,
                   base_mnemonic, base_register, canonical, h, parse_address, region_of)
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from recomp.block_profile import BlockProfile, read_profile

from capstone import m68k_const as M

from .relations import compile_expr, field_name, search
from .sprites import control_text, decode

ROLE_MARK = {"read": "r", "write": "w", "rmw": "rw", "addr": "&"}
ADDRESS_KEYS = {"pc", "site", "address", "entry"}


# ---------------------------------------------------------------- output
class Out:
    def __init__(self, args):
        self.json = getattr(args, "json", False)
        self.limit = getattr(args, "limit", 200)
        self.sink = getattr(args, "_sink", None)  # library calls collect rows with their raw fields
        self.shown = 0
        self.hidden = 0

    def note(self, text: str = "") -> None:
        if not self.json:
            print(text)

    @property
    def full(self) -> bool:
        return bool(self.limit) and self.shown >= self.limit

    def row(self, line: str, **fields) -> None:
        if self.limit and self.shown >= self.limit:
            self.hidden += 1
            return
        self.shown += 1
        if self.sink is not None:
            self.sink(line, fields)
        elif self.json:
            print(json.dumps({k: h(v) if k in ADDRESS_KEYS and isinstance(v, int) and v >= 0 else v
                              for k, v in fields.items()}, default=str))
        else:
            print(line)

    def done(self) -> None:
        if self.hidden and not self.json:
            print(f"… {self.hidden} more rows (raise --limit, 0 = unlimited)")


DEFAULT_DIR = ROOT / "build" / "f3a"


def terminal() -> str | None:
    """Name of the controlling terminal (ttys003), the one key that survives between commands of one
    interactive shell; harnesses that run each command in a fresh non-terminal shell have none."""
    for fd in (0, 1, 2):
        try:
            return os.path.basename(os.ttyname(fd))
        except OSError:
            continue
    return None


def session_default() -> Path | None:
    """`f3a use` file for this terminal: parallel terminals (and agents) keep their own default."""
    tty = terminal()
    return DEFAULT_DIR / f"game.{tty}" if tty else None


def load(args, run_dir: str | Path | None = None) -> Game:
    """The selected game: --game, F3A_GAME, the `game` file `f3a run` leaves in its --out directory
    (when the command reads one), then `f3a use`. Loaded once per command."""
    if getattr(args, "_game", None) is not None:
        return args._game
    game, source = args.game or getattr(args, "global_game", None), ""
    if not game and os.environ.get("F3A_GAME"):
        game, source = os.environ["F3A_GAME"], "F3A_GAME"
    if not game and run_dir is not None:
        for marker in (Path(run_dir) / "game", Path(run_dir).parent / "game"):
            if marker.is_file():
                game, source = marker.read_text().strip(), str(marker)
                break
    path = session_default()
    if not game and path and path.is_file():
        game, source = path.read_text().strip(), f"`f3a use` on {terminal()}"
    if not game:
        raise SystemExit("f3a: pass --game ID, set F3A_GAME, or run `tools/f3a use ID` in this terminal")
    if source:
        print(f"f3a: game={game} (from {source})", file=sys.stderr)
    args._game = Game(game, args.rom_dir, args.profile, args.symbols, parse_address(args.a5) if args.a5 else None)
    return args._game


def resolve_target(g: Game, text: str) -> int:
    """Address, a5 offset (a5-0x7cd7), vector name (reset, irq2, trap5, vector:irq2), symbol or sub_xxxxxx."""
    lowered = text.strip().lower()
    match = re.fullmatch(r"a5\s*([+-])\s*\$?(?:0x)?([0-9a-f]+)", lowered)
    if match:
        if g.a5 is None:
            raise SystemExit("f3a: a5 base unknown for this game; pass --a5")
        offset = int(match.group(2), 16)
        return canonical(g.a5 + (offset if match.group(1) == "+" else -offset))
    vector = lowered.removeprefix("vector:")
    for number, target in g.vectors.items():
        if vector in (VECTOR_NAMES.get(number, ""), f"v{number}"):
            return target
    for entry, name in g.symbols_code.items():
        if name.lower() == lowered:
            return entry
    for address, (name, _) in g.symbols_data.items():
        if name.lower() == lowered:
            return address
    match = re.fullmatch(r"(?:sub|loc)_([0-9a-f]+)", lowered)
    if match:
        return int(match.group(1), 16)
    try:
        value = parse_address(text)
    except ValueError:
        raise SystemExit(f"f3a: cannot read {text!r} as an address, a5±offset, vector name "
                         "(reset, irq2, trap5), symbol or sub_xxxxxx") from None
    return canonical(value) if value >= 0x200000 else value


def routine_for(g: Game, target: int) -> int:
    """The routine at or containing a PC; a clear error instead of the nearest unrelated routine."""
    if target in g.routines:
        return target
    if g.owners.get(target):
        return g.routine_of(target)
    blank = target < len(g.rom) and g.rom[target:target + 4] == b"\xff" * 4
    raise SystemExit(f"f3a: {h(target)} is not inside any discovered routine of {g.id}"
                     + (" (blank ROM there: wrong game?)" if blank else
                        f" ({g.evidence(target) if target in g.code else 'not decoded as code'}); "
                        f"try `f3a dis {h(target)} --count 20` or `f3a xref {h(target)}`"))


def kinds_text(kinds: set) -> str:
    vectors = sorted(k.split(":", 1)[1] for k in kinds if k.startswith("vector:"))
    rest = sorted(k for k in kinds if not k.startswith("vector:"))
    if len(vectors) > 3:
        vectors = [f"{len(vectors)} vectors"]
    return ",".join([f"vector:{v}" for v in vectors] + rest) or "-"


def insn_text(insn) -> str:
    """Capstone names the PC base `a16` in indexed modes and prints a suppressed index as `invalid.w`."""
    text = f"{insn.mnemonic} {insn.op_str}".strip()
    return re.sub(r",\s*invalid\.[wl](\s*\*\s*\d)?", "", text.replace("(a16,", "(pc,"))


# ---------------------------------------------------------------- annotations
def annotate(g: Game, pc: int, insn, state: dict | None = None) -> list[str]:
    notes = []
    kind, targets = g._transfer_targets(pc, insn)
    if base_mnemonic(insn) == "trap" and insn.operands:
        num = insn.operands[0].imm & 15
        if num in g.yield_traps:
            res_pc = pc + insn.size
            notes.append(f"yield (trap{num} task switch): resumes at {h(res_pc)}")
    if kind in ("call", "branch", "cond"):
        for t in targets:
            if t in g.routines:
                notes.append(f"→ {g.label(t)}")
            elif kind == "call":
                notes.append(f"→ {h(t)}")
    elif kind in ("table-call", "table-jump"):
        source = "config table" if pc in g.config_tables else "scanned table"
        names = ", ".join(g.label(t) if t in g.routines else h(t) for t in targets[:8])
        more = f" +{len(targets) - 8}" if len(targets) > 8 else ""
        notes.append(f"{source} → [{names}{more}]")
    elif kind in ("unresolved-call", "unresolved-jump"):
        entry = g.routine_of(pc)
        resolved, how = g.register_target(entry, pc) if entry in g.routines else ([], "")
        if resolved:
            names = ", ".join(g.label(t) if t in g.routines else h(t) for t in resolved[:8])
            notes.append(f"{how} → [{names}{' +' + str(len(resolved) - 8) if len(resolved) > 8 else ''}]")
        else:
            notes.append("UNRESOLVED computed transfer → ?dispatch" + (f"; {how}" if how else ""))
    for t in g.refs.get(pc, []):
        notes.append(f"code ref → {g.label(t)}")
    entry = g.routine_of(pc)
    transfer = base_mnemonic(insn) in ("jmp", "jsr")
    for operand in g.operands(insn):
        if transfer and operand.mode == "areg":
            continue  # (aN) targets are explained by the transfer note above
        mark = ROLE_MARK[operand.role]
        _, where, region = resolve_store(g, entry, operand, state)
        if region != "stack" and not where.endswith("(base unknown)"):
            notes.append(f"{mark} {where}" + (" (memory-indirect)" if operand.indirect else ""))
        if operand.index_reg and operand.scale > 1 and f"*{operand.scale}" not in insn.op_str.replace(" ", ""):
            notes.append(f"index scale *{operand.scale} (Capstone text omits it)")
    return notes


def resolve_store(g: Game, entry: int | None, operand, state: dict | None) -> tuple[list[int], str, str]:
    """Where a memory operand points: (addresses, description, region). region is a region name,
    'mixed' (argument pointers from callers disagree), 'stack', or '?' (not statically known)."""
    idx = f" + {operand.index_reg}*{operand.scale}" if operand.index_reg else ""
    if operand.mode in ("abs", "pc", "a5") and operand.address is not None:
        return [operand.address], g.describe_address(operand.address), region_of(operand.address)
    value = state.get(operand.reg) if state and operand.reg else None
    if operand.reg in ("a7", "sp") or (value is not None and value.kind in ("sp", "frame")):
        return [], "stack", "stack"
    if operand.address is not None:  # indexed from a known pc/abs/a5 base
        return [operand.address], f"indexed from {g.describe_address(operand.address)}{idx}", region_of(operand.address)
    if value is None:
        return [], f"{operand.reg or '?'}{operand.disp:+#x}{idx} (base unknown)", "?"
    if value.kind == "const":
        bases = [value.value] + list(value.alts)
        addresses = [canonical(b + operand.disp) for b in bases]
        step = f", stepped {value.step:+d}" if value.step else ""
        drift = ", plus a register offset" if value.drift else ""
        regions = {region_of(a) for a in addresses}
        if len(bases) > 1:
            return addresses, (f"{operand.reg}∈{{{', '.join(h(b) for b in bases)}}}{operand.disp:+#x}{idx} → "
                               f"{' or '.join(g.describe_address(a) for a in addresses)} (set on alternative "
                               f"branches, last at {h(value.set_at)}{step}{drift})"), \
                (regions.pop() if len(regions) == 1 else "mixed")
        return addresses, (f"{operand.reg}≈{h(value.value)}{operand.disp:+#x}{idx} → {g.describe_address(addresses[0])} "
                           f"(set {h(value.set_at)}{step}{drift})"), region_of(addresses[0])
    if value.kind in ("field", "table"):
        what = (f"long loaded from {value.reg}{value.value:+#x}" if value.kind == "field"
                else f"long loaded from table {h(value.value)}")
        return [], f"{operand.reg}={what} at {h(value.set_at)}{operand.disp:+#x}{idx} (pointer value not static)", "?"
    # Pointer from a RAM cell, a stack argument or the caller's register: resolve through the
    # cell's constant stores / every static call site (bounded depth).
    addresses, resolved, tried = g.bases(entry, value) if entry is not None else ([], 0, 0)
    addresses = sorted({canonical(a + operand.disp) for a in addresses})
    regions = Counter(region_of(a) for a in addresses)
    region = next(iter(regions)) if len(regions) == 1 else "mixed" if regions else "?"
    sample = ", ".join(f"{r} {h(min(a for a in addresses if region_of(a) == r))}"
                       + (f"..{h(max(a for a in addresses if region_of(a) == r))}" if c > 1 else "")
                       for r, c in regions.most_common(3))
    if value.kind == "arg" and not addresses and entry is not None:
        callers = g.callers.get(entry, [])
        if not callers or all(c.kind == "dispatch" for c in callers):
            reg = operand.reg or "arg"
            return [], f"via {reg} = arg +{value.value} ({g.label(entry)} has no static callers)", "stack-arg"
    if value.kind == "ptr":
        origin = f"*[{g.describe_address(value.value)}] (pointer cell"
        coverage = "; cell gets constant stores" if addresses else "; no constant stores to the cell"
    elif value.kind == "arg":
        origin = f"stack arg +{value.value:#x} (set {h(value.set_at)}"
        coverage = f"; {resolved}/{tried} call paths resolve"
    else:
        origin = f"caller's {value.reg} (incoming"
        coverage = f"; {resolved}/{tried} call paths resolve"
    extra = (f", stepped {value.step:+d}" if value.step else "") + (", plus a register offset" if value.drift else "")
    return addresses, (f"{operand.reg}={origin}{coverage}{extra}){operand.disp:+#x}{idx}"
                       + (f" → {sample}" if sample else "")), region


# ---------------------------------------------------------------- dis
def cmd_dis(args) -> None:
    g = load(args)
    out = Out(args)
    text = args.target
    if args.data:
        dump_data(g, out, resolve_target(g, text.split("..", 1)[0]), args.data,
                  args.count or (64 if args.data == "offsets" else 16))
        return
    if ".." in text:
        start, end = (resolve_target(g, part) for part in text.split("..", 1))
        pcs, pc = [], start
        while pc <= end:
            insn = g.insn_at(pc)
            pcs.append(pc)
            pc += insn.size if insn is not None else 2
        entry = g.routine_of(start)
        header = f"range {h(start)}..{h(end)} (linear decode; 'decoded?' rows are not proven code)"
    else:
        target = resolve_target(g, text)
        entry = target if target in g.routines else g.routine_of(target)
        if args.count or entry is None or (target not in g.code):
            count = args.count or 40
            pcs, pc = [], target
            for _ in range(count):
                insn = g.insn_at(pc)
                pcs.append(pc)
                pc += insn.size if insn is not None else 2
            header = f"{count} instructions from {h(target)} (linear decode)"
            if g.rom[target:target + 8] == b"\xff" * 8:
                header += f"  NOTE: blank ROM (0xff) at {h(target)} in {g.id}: wrong game or past the program?"
            elif g.owners.get(target) and target not in g.routines:
                header += f"; inside {g.owner_label(target)} (`f3a dis {g.label(g.routine_of(target))}` for the routine)"
        else:
            pcs = g.routines[entry].pcs
            header = routine_header(g, entry)
            if target != entry:
                owners = g.owners.get(target, [])
                header += (f"\n  {h(target)} is not a routine entry: it is code inside {g.label(entry)}"
                           + (f", shared by {len(owners)} routines ({', '.join(g.label(o) for o in owners[:4])}"
                              + (" …" if len(owners) > 4 else "") + ")" if len(owners) > 1 else "")
                           + f"; `f3a dis {h(target)} --count N` reads just that code")
    if args.pc_from is not None or args.pc_to is not None:
        lo = resolve_target(g, args.pc_from) if args.pc_from is not None else 0
        hi = resolve_target(g, args.pc_to) if args.pc_to is not None else 1 << 32
        pcs = [pc for pc in pcs if lo <= pc <= hi]
        header += f"\n  only {h(lo) if lo else 'start'}..{h(hi) if hi < 1 << 32 else 'end'} ({len(pcs)} instructions)"
    out.note(header)
    states: dict[int, dict] = {}
    if entry in g.routines:
        states = g.states(entry)
    previous_end = None
    for index, pc in enumerate(pcs):
        if out.limit and out.shown >= out.limit:
            out.hidden += len(pcs) - index
            break
        insn = g.insn_at(pc)
        if previous_end is not None and pc != previous_end:
            shared = [g.label(o) for o in g.owners.get(pc, []) if o != entry]
            out.note(f"{'':10}… gap {h(previous_end)}..{h(pc)}"
                     + (f"  (shared tail, also in {', '.join(shared[:3])})" if shared else ""))
        if insn is None:
            out.row(f"  {h(pc)}  {'data?':>9}  dc.w ${int.from_bytes(g.rom[pc:pc + 2], 'big'):04x}", pc=pc, text="(undecodable)")
            previous_end = pc + 2
            continue
        mark = ">" if pc in g.routines else " "
        raw = f"{bytes(insn.bytes).hex():<20} " if args.bytes else ""
        notes = annotate(g, pc, insn, states.get(pc))
        line = f"{mark} {h(pc)}  {g.evidence(pc):>9}  {raw}{insn_text(insn):<40}"
        if pc in g.routines and pc != pcs[0]:
            out.note(f"{'':10}--- {g.label(pc)} [{kinds_text(g.routines[pc].kinds)}]")
        out.row(line + ("  ; " + "; ".join(notes) if notes else ""), pc=pc, evidence=g.evidence(pc),
                text=insn_text(insn), bytes=bytes(insn.bytes).hex(), notes=notes)
        previous_end = pc + insn.size
    out.done()


def dump_data(g: Game, out: Out, start: int, unit: str, rows: int) -> None:
    """ROM tables as data: 16 bytes, 8 words or 4 longs per row; longs naming routines are labelled.
    offsets: signed words added to the table address (`jmp table(pc,d0.w)`), one entry per row."""
    if unit == "offsets":
        out.note(f"offset table at {g.describe_address(start)}: entry index, word, target = table + word")
        for index in range(rows):
            at = start + 2 * index
            if index and (at in g.routines or (at in g.code and start not in g.code)):
                out.note(f"  table ends at {h(at)}: code of {g.owner_label(at)} starts there ({index} entries)")
                break
            word = int.from_bytes(g.rom[at:at + 2], "big", signed=True)
            target = start + word
            where = (g.label(target) if target in g.routines else f"in {g.owner_label(target)}") \
                if target in g.code else "not code"
            out.row(f"  [{index:3}] {h(at)}  {word & 0xffff:04x}  → {h(target)} {g.evidence(target):>9}  {where}",
                    index=index, address=at, value=word, target=target, evidence=g.evidence(target), where=where)
        out.done()
        return
    size = {"bytes": 1, "words": 2, "longs": 4}[unit]
    per_row = {1: 16, 2: 8, 4: 4}[size]
    out.note(f"{unit} at {g.describe_address(start)}" + ("  (inside code: check the table bounds)" if start in g.code else ""))
    for row in range(rows):
        base = start + row * per_row * size
        if base + per_row * size > len(g.rom):
            break
        stop = next((a for a in range(base, base + per_row * size)
                     if a != start and (a in g.routines or (a in g.code and start not in g.code))), None)
        end = stop if stop is not None else base + per_row * size
        values = [int.from_bytes(g.rom[a:a + size], "big") for a in range(base, end - size + 1, size)]
        text = " ".join(f"{v:0{size * 2}x}" for v in values)
        notes = ""
        if size == 4:
            labels = [g.label(v) if v in g.routines else "" for v in values]
            if any(labels):
                notes = "  ; " + " ".join(l or "-" for l in labels)
        if values:
            out.row(f"  {h(base)}  {text}{notes}", address=base, values=values)
        if stop is not None:
            out.note(f"  table ends at {h(stop)}: code of {g.owner_label(stop)} starts there "
                     f"({(stop - start) // size} {unit} from {h(start)})")
            break
    out.done()


def routine_header(g: Game, entry: int) -> str:
    routine = g.routines[entry]
    hits = max((g.hits.get(g.block_of.get(pc, pc), 0) for pc in routine.pcs), default=0)
    entry_hits = g.hits.get(g.block_of.get(entry, entry), 0)
    callers = {e.site for e in g.callers.get(entry, []) if e.kind != "fall"}
    hit_text = f"entry_hits={entry_hits or '-'} max_block_hits={hits or '-'}" if g.hits else "no profile"
    lines = [f"{g.label(entry)} @ {h(entry)}  kinds={kinds_text(routine.kinds)}  insns={len(routine.pcs)}  "
             f"span={h(routine.pcs[0])}..{h(routine.pcs[-1])}  caller_sites={len(callers)}  {hit_text}"]
    shared = Counter(o for pc in routine.pcs for o in g.owners.get(pc, []) if o != entry)
    if shared:
        lines.append("  shares code with " + ", ".join(f"{g.label(o)} ({n} insns)" for o, n in shared.most_common(4))
                     + " (branches into common tails)")
    if entry in g.dispatch_hint:
        lines.append(f"  uncertain origin: {g.dispatch_hint[entry]}")
        near = sorted((abs(e.site - entry), e.site, e.src) for e in g.edges if e.dst == DISPATCH and abs(e.site - entry) < 0x200)
        if near:
            lines.append("  nearest unresolved computed transfers: " + ", ".join(
                f"{h(site)} in {g.label(src)}" for _, site, src in near[:3]))
    return "\n".join(lines)


# ---------------------------------------------------------------- xref
def cmd_xref(args) -> None:
    g = load(args)
    out = Out(args)
    if args.field is not None:
        field_xrefs(g, out, args)
        out.done()
        return
    if not args.targets:
        raise SystemExit("f3a: xref needs targets or --field OFF")
    for text in args.targets:
        target = resolve_target(g, text)
        if target < len(g.rom) and target in g.code and not args.data:
            code_xrefs(g, out, target, args.sort)
        else:
            data_xrefs(g, out, canonical(target), args)
    out.done()


BIT_OPS = {"btst": "test", "bset": "set", "bclr": "clear", "bchg": "flip"}
MASK_OPS = {"andi": "clear", "ori": "set", "eori": "flip"}


def mask_bits(insn, base: str) -> tuple[str, list[int]] | None:
    """`ori.b #4,$1(a0)` → ('set', [2]): bits of the byte at the operand's displacement that an
    immediate mask op changes (word/long ops: that byte is the high one)."""
    if base not in MASK_OPS or insn.operands[0].type != M.M68K_OP_IMM:
        return None
    size = access_size(insn)
    imm = insn.operands[0].imm & ((1 << (8 * size)) - 1)
    byte = (imm >> (8 * (size - 1))) & 0xff
    if base == "andi":
        byte = ~byte & 0xff
    return MASK_OPS[base], [b for b in range(8) if byte >> b & 1]


def code_value(g: Game, insn, state: dict):
    """The code address a store writes (int), None if the value is not static, False if it is a
    static non-code value."""
    if len(insn.operands) != 2 or insn.operands[1].type != M.M68K_OP_MEM:
        return False
    src = insn.operands[0]
    if src.type == M.M68K_OP_IMM:
        value = src.imm & 0xffffffff
    elif src.type == M.M68K_OP_REG and insn.reg_name(src.reg) in state:
        held = state[insn.reg_name(src.reg)]
        if held.kind != "const":
            return None
        value = (held.value + held.step) & 0xffffffff
    else:
        return None
    return value if value in g.code else False


def field_xrefs(g: Game, out: Out, args) -> None:
    """Reads, writes and bit tests at displacement OFF through any address register (structure field)."""
    text = args.field
    off_text, _, bit_text = text.partition(".")
    offset, bit = int(off_text, 0), (int(bit_text, 0) if bit_text else None)
    if offset < 0:
        out.note(f"negative field offset: if -{-offset:#x} is an a5 global, `f3a xref a5-{-offset:#x}` lists its "
                 "accesses (--field excludes a5)")
    within = None
    if args.within:
        lo_text, _, hi_text = re.split(r"(-|\.\.)", args.within, maxsplit=1)
        within = (canonical(parse_address(lo_text)), canonical(parse_address(hi_text)))
    out.note(f"accesses to field +{offset:#x}" + (
        f" bit {bit} (bit ops, andi/ori/eori masks" + (", and immediate byte/word/long stores that write the byte)"
                                                       if within else "; immediate stores like move.w #$cdc0,(a1) "
                                                       "also set it: add --within START-END to list them)")
        if bit is not None else "")
             + " through an address register (a5/a7 excluded); base = what the register tracker knows"
             + (f"; role={args.role}" if args.role else "")
             + (f"; only bases resolved inside {h(within[0])}..{h(within[1])}" if within else "")
             + ("; only stores of code addresses (or of longs whose value is not static)" if args.code else ""))
    rows = []
    for entry in g.entry_list:
        states = g.states(entry)
        for pc in g.routines[entry].pcs:
            insn = g.code[pc]
            base = base_mnemonic(insn)
            for operand in g.operands(insn):
                if operand.mode not in ("areg", "areg-idx", "memi") or operand.reg in ("a5", "a7", "sp", ""):
                    continue
                size = access_size(insn)
                covering = (bit is not None and within is not None and operand.role == "write" and base == "move"
                            and insn.operands[0].type == M.M68K_OP_IMM
                            and operand.disp < offset < operand.disp + size)
                if operand.disp != offset and not covering:
                    continue
                role = args.role
                if role and operand.role != role and not (role == "write" and operand.role == "rmw"):
                    continue
                tested, what = None, None
                if base in BIT_OPS and insn.operands[0].type == M.M68K_OP_IMM:
                    tested = insn.operands[0].imm & 7
                    what = f"{BIT_OPS[base]} {tested}"
                masked = mask_bits(insn, base) if operand.role == "rmw" else None
                if masked:
                    verb, bits = masked
                    what = f"{verb} " + ",".join(map(str, bits)) if bits else f"{verb} -"
                stored = None
                if bit is not None and within is not None and base == "move" and operand.role == "write" \
                        and insn.operands[0].type == M.M68K_OP_IMM:
                    shift = 8 * (operand.disp + size - 1 - offset)  # big-endian: byte OFF of the stored value
                    stored = (insn.operands[0].imm >> shift) >> bit & 1
                    what = f"store {stored}"
                if bit is not None and tested != bit and not (masked and bit in masked[1]) and stored is None:
                    continue
                state = states.get(pc, {})
                addresses, where, region = resolve_store(g, entry, operand, state)
                if within and not any(within[0] <= canonical(a) <= within[1] for a in addresses):
                    continue
                if args.code:
                    value = code_value(g, insn, state)
                    if value is False or (value is None and size != 4):
                        continue
                    where = (f"value {g.label(value) if value in g.routines else h(value)}; " if value else
                             "value not static; ") + where
                rows.append((pc, entry, operand.role, tested, what, where, insn))
    seen = set()
    for pc, entry, role, tested, what, where, insn in sorted(rows, key=lambda r: (-hits_of(g, r[0]), r[0])
                                                               if args.sort == "hits" else r[0]):
        if pc in seen:
            continue  # shared tails list the instruction once
        seen.add(pc)
        what = what or ROLE_MARK[role]
        out.row(f"  {h(pc)}  {what:<9} {g.owner_label(pc):<24} {g.evidence(pc):>9}  {insn_text(insn):<34} {where}",
                pc=pc, role=role, bit=tested, routine=g.owner_label(pc), evidence=g.evidence(pc),
                text=insn_text(insn), where=where)


def hits_of(g: Game, pc: int) -> int:
    return g.hits.get(g.block_of.get(pc, pc), 0)


def code_xrefs(g: Game, out: Out, target: int, sort: str = "pc") -> None:
    entry = target if target in g.routines else None
    out.note(routine_header(g, entry) if entry is not None else
             f"{h(target)} is inside {g.owner_label(target)} (not a routine entry): direct transfers to it")
    rows: dict[tuple[int, str], list[str]] = defaultdict(list)  # (site, kind) -> routines sharing it
    if entry is not None:
        for edge in g.callers.get(entry, []):
            rows[(edge.site, edge.kind)].append(g.label(edge.src))
    for pc in g.sorted_pcs:  # transfers into the middle of a routine and data refs
        insn = g.code[pc]
        kind, targets = g._transfer_targets(pc, insn)
        if target in targets and (entry is None or g.routine_of(pc) == entry):
            rows[(pc, kind if entry is None else "loop/branch")].append(g.owner_label(pc))
        if entry is None and target in g.refs.get(pc, []):
            rows[(pc, "ref")].append(g.owner_label(pc))
    for address in g.rom_pointers().get(target, []):
        rows[(address, "rom-data")].append("(long in ROM data)")
    order = (lambda r: (r[0] < 0, -hits_of(g, r[0]), r[0])) if sort == "hits" else (lambda r: (r[0] < 0, r[0]))
    if sort == "routine":
        grouped = Counter(who for names in rows.values() for who in names)
        for who, count in grouped.most_common():
            out.row(f"  {count:4}  {who}", routine=who, sites=count)
        return
    for (site, kind), names in sorted(rows.items(), key=lambda kv: order(kv[0])):
        who = names[0] + (f" +{len(names) - 1} sharing this code" if len(names) > 1 else "")
        if site < 0:
            out.row(f"  {'-':8}  {kind:<12} {who:<22} profile-hit entry with no static predecessor",
                    site=None, kind=kind, routine=who)
            continue
        if kind == "rom-data":
            out.row(f"  {h(site)}  {kind:<12} {'':<22} {'':>9}  dc.l ${target:x}  (script, callback list or table)",
                    site=site, kind=kind, routine="", text=f"dc.l ${target:x}")
            continue
        insn = g.code.get(site)
        out.row(f"  {h(site)}  {kind:<12} {who:<22} {g.evidence(site):>9}  {insn_text(insn) if insn else ''}",
                site=site, kind=kind, routine=who, routines=names, evidence=g.evidence(site),
                text=insn_text(insn) if insn else "")


def data_xrefs(g: Game, out: Out, target: int, args) -> None:
    length, role = args.len, args.role
    out.note(f"accesses to {g.describe_address(target)} (+{length} bytes); a5={h(g.a5) if g.a5 is not None else '?'}")
    hits = []
    pcs = list(g.sorted_pcs)
    if args.all:
        pcs += [pc for pc in range(0, len(g.rom), 2) if pc not in g.code]
    for pc in pcs:
        insn = g.code.get(pc) or g.insn_at(pc)
        if insn is None:
            continue
        operands = g.operands(insn)
        if not operands:
            continue
        entry = g.routine_of(pc) if pc in g.code else None
        state = g.states(entry).get(pc) if entry in g.routines else None
        for operand in operands:
            if role and operand.role != role and not (role == "write" and operand.role == "rmw"):
                continue
            size = access_size(insn) if operand.role != "addr" else 1
            if operand.mode in ("abs", "pc", "a5") and operand.address is not None:
                if not (operand.address < target + length and target < operand.address + size):
                    continue
                how = operand.mode
            elif operand.address is not None and operand.index_reg:
                if not (args.indexed and operand.address <= target < operand.address + 0x400):
                    continue
                how = f"indexed+{target - operand.address:#x}?"
            else:
                # Register-based: match when the static base (constant, pointer cell, stack or register
                # argument from callers) puts the operand on the target.
                if state is None or operand.index_reg:
                    continue
                addresses, _, region = resolve_store(g, entry, operand, state)
                if region == "stack" or not any(a < target + length and target < a + size for a in addresses):
                    continue
                value = state.get(operand.reg)
                how = f"via {operand.reg}" + ("" if value is None or value.kind == "const" else f" ({value.kind})")
            hits.append((pc, operand, size, how, insn))
    by_role = Counter(operand.role for _, operand, _, _, _ in hits)
    by_routine = Counter(g.owner_label(pc) for pc, *_ in hits)
    out.note(f"  {len(hits)} sites: " + ", ".join(f"{k}={v}" for k, v in sorted(by_role.items()))
             + f" in {len(by_routine)} routines" + ("" if args.indexed else "  (indexed accesses excluded; --indexed)"))
    if args.sort == "routine":
        for who, count in by_routine.most_common():
            out.row(f"  {count:4}  {who}", routine=who, sites=count)
        return
    if args.sort == "hits":
        hits.sort(key=lambda r: -hits_of(g, r[0]))
    for pc, operand, size, how, insn in hits:
        evidence = g.evidence(pc)
        out.row(f"  {h(pc)}  {ROLE_MARK[operand.role]:<2} {size}  {how:<14} {g.owner_label(pc):<22} {evidence:>9}  "
                f"{insn_text(insn)}", pc=pc, role=operand.role, size=size, mode=how, routine=g.owner_label(pc),
                evidence=evidence, text=insn_text(insn), address=operand.address)


# ---------------------------------------------------------------- flow
def cmd_flow(args) -> None:
    g = load(args)
    out = Out(args)
    target = resolve_target(g, args.target) if args.target != "?dispatch" else DISPATCH
    entry = DISPATCH if target == DISPATCH else routine_for(g, target)
    if args.path:
        paths_to(g, out, entry, args.path)
    if args.tree:
        tree(g, out, entry, args.tree)
    if not args.path and not args.tree:
        if entry == DISPATCH:
            dispatch_summary(g, out, getattr(args, "observed", None))
        else:
            summary(g, out, entry)
    out.done()


def summary(g: Game, out: Out, entry: int) -> None:
    out.note(routine_header(g, entry))
    out.note("callers:")
    for edge in sorted(g.callers.get(entry, []), key=lambda e: e.site):
        site = h(edge.site) if edge.site >= 0 else "-"
        out.row(f"  {site}  {edge.kind:<10} {g.label(edge.src)}", direction="caller", site=edge.site,
                kind=edge.kind, routine=g.label(edge.src))
    if not g.callers.get(entry):
        out.note("  (none: root" + (" vector" if any(k.startswith("vector") for k in g.routines[entry].kinds) else "") + ")")
    out.note("callees:")
    for edge in sorted(g.callees.get(entry, []), key=lambda e: e.site):
        out.row(f"  {h(edge.site)}  {edge.kind:<10} {g.label(edge.dst)}", direction="callee", site=edge.site,
                kind=edge.kind, routine=g.label(edge.dst))
    stores, reads = memory_summary(g, entry)
    out.note("memory (static operands; areg rows use the register tracker):")
    for region, count in sorted(stores.items(), key=lambda kv: -kv[1]):
        out.row(f"  writes {region:<10} {count}", direction="writes", region=region, count=count)
    for address, count in reads.most_common(12):
        out.row(f"  reads  {g.describe_address(address):<40} {count}", direction="reads", address=address, count=count)


def memory_summary(g: Game, entry: int):
    stores: Counter = Counter()
    reads: Counter = Counter()
    states = g.states(entry)
    for pc in g.routines[entry].pcs:
        for operand in g.operands(g.code[pc]):
            if operand.role in ("write", "rmw"):
                _, _, region = resolve_store(g, entry, operand, states.get(pc))
                if region != "stack":
                    stores[region if region != "?" else f"?[{operand.reg or 'idx'}]"] += 1
            elif operand.role == "read" and operand.address is not None and operand.mode in ("abs", "a5"):
                reads[operand.address] += 1
    return stores, reads


def passed_targets(g: Game, entry: int, pc: int) -> list[int]:
    """jmp/jsr (aN) where aN is the caller's register or a stack argument: the code addresses the
    static callers pass (a continuation or callback argument)."""
    insn = g.code[pc]
    op = insn.operands[-1] if insn.operands else None
    if op is None or op.address_mode != M.M68K_AM_REGI_ADDR:
        return []
    value = g.states(entry).get(pc, {}).get(base_register(insn, op))
    if value is None or value.kind not in ("in", "arg", "ptr"):
        return []
    addresses, _, _ = g.bases(entry, value)
    return [a for a in addresses if a in g.code]


def dispatch_summary(g: Game, out: Out, observed_arg: str | None = None) -> None:
    obs_file = None
    if observed_arg:
        p = Path(observed_arg)
        if p.is_dir():
            if (p / "indirect.log").is_file():
                obs_file = p / "indirect.log"
            elif (p / f"{g.id}.indirect").is_file():
                obs_file = p / f"{g.id}.indirect"
            else:
                for candidate in p.glob("*.indirect"):
                    obs_file = candidate
                    break
        elif p.is_file():
            obs_file = p
        if obs_file is None:
            raise SystemExit(f"f3a: no indirect target log found at {observed_arg}")
    else:
        default_indirect = ROOT / "profiles" / f"{g.id}.indirect"
        if default_indirect.is_file():
            obs_file = default_indirect

    observed_by_site: dict[int, dict[int, int]] = defaultdict(dict)
    if obs_file:
        for (site, target), count in read_indirect_log(obs_file).items():
            observed_by_site[site][target] = count

    sites: dict[int, list[int]] = defaultdict(list)
    for edge in g.edges:
        if edge.dst == DISPATCH:
            sites[edge.site].append(edge.src)
    for site in observed_by_site:
        if site not in sites:
            owner = g.routine_of(site)
            sites[site].append(owner if owner is not None else site)
    entries = [e for e in g.edges if e.src == DISPATCH]
    out.note(f"?dispatch: {len(sites)} unresolved computed transfer sites, {len(entries)} profile-hit entries "
             "with no static predecessor")
    if obs_file:
        out.note(f"observed targets from {obs_file}: covers only what the recorded runs executed")
    out.note("entries reached only through dispatch, grouped by the instruction before them:")
    groups = defaultdict(list)
    for edge in entries:
        hint = g.dispatch_hint.get(edge.dst, "")
        # Group by the kind of evidence, not the exact addresses ("at 0x.., 0x.. +3" → "at …").
        key = re.sub(r"(at|after) 0x[0-9a-f]+(, 0x[0-9a-f]+)*( \+\d+)?", r"\1 …", hint)
        key = re.sub(r"\((jmp|bra) 0x[0-9a-f]+ at …\)", r"(\1 0x… )", key) if "tail jump" not in key else \
            re.sub(r"at …\)", ")", key)
        groups[key].append(edge.dst)
    for key, members in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        shown = members if out.limit == 0 else members[:6]
        sample = ", ".join(g.label(m) for m in shown) + (f" +{len(members) - len(shown)}" if len(members) > len(shown) else "")
        out.row(f"  {len(members):4}  {key}: {sample}", direction="entries", why=key,
                routines=[g.label(m) for m in members])
    out.note("unresolved sites (where control leaves static knowledge), most executed first; 'slot +d' = the target "
             "is a code pointer read from offset d of a structure, with the routines the program stores there:")
    order = sorted(sites, key=lambda pc: (-g.hits.get(g.block_of.get(pc, pc), 0), -sum(observed_by_site.get(pc, {}).values())))
    listed: dict[frozenset, int] = {}  # candidate set -> first site that printed it
    for pc in order:
        insn = g.code.get(pc)
        owners = sites[pc]
        owner = owners[0]
        who = (g.label(owner) if owner in g.routines else g.owner_label(owner)) + (f" +{len(owners) - 1} sharing routines" if len(owners) > 1 else "")
        where, candidates = g.slot_targets(owner, pc) if (owner in g.routines and pc in g.code) else ("", {})
        if not where and owner in g.routines and pc in g.code:
            passed = passed_targets(g, owner, pc)
            if passed:
                where, candidates = "target passed in by the callers", {t: [] for t in passed}
        if not where and owner in g.routines and pc in g.code:
            where = g.register_target(owner, pc)[1]
        shown = sorted(candidates, key=lambda t: -g.hits.get(t, 0))
        name = lambda t: g.label(t) if t in g.routines else g.owner_label(t)
        hit = lambda t: f" x{g.hits[t]}" if g.hits.get(t) else ""
        cand_set = set(candidates.keys() if isinstance(candidates, dict) else candidates)
        if not cand_set and owner in g.routines and pc in g.code:
            resolved_regs = g.register_target(owner, pc)[0]
            if resolved_regs:
                cand_set.update(resolved_regs)
        key = frozenset(candidates)
        if len(candidates) > 4 and key in listed:
            detail = f"{where}: the same {len(candidates)} candidates as {h(listed[key])}"
        else:
            if len(candidates) > 4:
                listed[key] = pc
            listing = ", ".join(name(t) + hit(t) for t in (shown if out.limit == 0 else shown[:4])) \
                + (f" +{len(shown) - 4}" if out.limit and len(shown) > 4 else "")
            detail = f"{where}: {len(candidates)} candidate{'s' * (len(candidates) != 1)} ({listing})" if candidates else where
        def obs_label(t: int) -> str:
            if t in g.routines:
                return g.label(t)
            t_owner = g.routine_of(t)
            if t_owner is not None and t_owner != owner and t_owner not in owners:
                return f"0x{t:06x} in {g.label(t_owner)}"
            return f"0x{t:06x}"

        site_obs = observed_by_site.get(pc, {})
        sorted_obs = sorted(site_obs.items(), key=lambda kv: (-kv[1], kv[0]))
        observed_field = {obs_label(t): count for t, count in sorted_obs}
        if sorted_obs:
            obs_items = []
            for t, count in sorted_obs:
                t_owner = g.routine_of(t)
                is_local = (t == owner) or (t_owner is not None and (t_owner == owner or t_owner in owners))
                if is_local:
                    mark = " (local)"
                else:
                    is_static = t in cand_set or (t_owner is not None and t_owner in cand_set)
                    mark = "" if is_static else " (not a static candidate)"
                obs_items.append(f"{obs_label(t)} x{count}{mark}")
            obs_text = "observed: " + ", ".join(obs_items)
            detail = f"{detail}; {obs_text}" if detail else obs_text
        text = insn_text(insn) if insn else f"0x{pc:06x}"
        out.row(f"  {h(pc)}  {g.evidence(pc):>9}  {text:<28} in {who}" + (f"  — {detail}" if detail else ""),
                direction="site", site=pc, routines=[g.label(o) for o in owners], text=text,
                source=where, candidates=[name(t) for t in shown],
                candidate_hits={name(t): g.hits.get(t, 0) for t in shown},
                observed=observed_field)

def roots_of(g: Game, entry: int) -> bool:
    kinds = g.routines[entry].kinds if entry in g.routines else set()
    return entry == DISPATCH or any(k.startswith("vector") or k in ("entry", "seed") for k in kinds) \
        or not [e for e in g.callers.get(entry, []) if e.kind != "fall"]


def tree(g: Game, out: Out, entry: int, depth: int) -> None:
    """Call tree, depth-first in call-site order. ?dispatch is a leaf (unless it is the root) and
    over-approximated slot candidates of one site collapse into one line. With a profile, xN is how
    often that call site ran (the root: entry count); `not run` marks sites the profile never reached."""
    seen = set()
    if g.hits:
        out.note("xN = executions of the call site (profile); root: entry count; `not run` = site never executed")

    def count_text(node: int, site: int | None) -> tuple[str, int]:
        if not g.hits:
            return "", 0
        count = g.hits.get(node, 0) if site is None or site < 0 else hits_of(g, site)
        return (f" x{count}" if count else " not run"), count

    def walk(node: int, level: int, via: str, site: int | None) -> None:
        label = g.label(node)
        repeat = node in seen
        text, count = count_text(node, site)
        task_mark = " [task]" if node in g.routines and "task" in g.routines[node].kinds else ""
        out.row(f"{'  ' * level}{via}{label}{task_mark}{text}" + (" (above)" if repeat and level else ""), depth=level,
                routine=label, via=via.strip(), hits=count)
        if repeat or level >= depth or (node == DISPATCH and level):
            return
        seen.add(node)
        if node in g.routines and "task" in g.routines[node].kinds:
            resumes = [res for res, task in g.resume_points.items() if task == node]
            resumes.sort(key=lambda r: (-g.hits.get(r, 0), r) if g.hits else r)
            for res_pc in resumes:
                text_res, count_res = count_text(res_pc, None)
                out.row(f"{'  ' * (level + 1)}[resume] {h(res_pc)}{text_res}", depth=level + 1,
                        routine=label, via="resume", pc=h(res_pc), hits=count_res)
        by_site: dict[tuple[int, str], list[Edge]] = defaultdict(list)
        for edge in g.callees.get(node, []):
            if edge.kind == "resume":
                continue  # already listed above
            by_site[(edge.site, edge.kind)].append(edge)
        target_sets: dict[frozenset, int] = {}  # table-call targets already listed under this routine
        for (site, kind), edges in sorted(by_site.items()):
            if kind == "slot" and len(edges) > 3:
                names = ", ".join(g.label(e.dst) for e in sorted(edges, key=lambda e: -g.hits.get(e.dst, 0))[:4])
                out.row(f"{'  ' * (level + 1)}[slot @{h(site)}] {len(edges)} candidates (any routine stored at that "
                        f"structure offset): {names} …", depth=level + 1, via=f"slot @{h(site)}",
                        candidates=[g.label(e.dst) for e in edges])
                continue
            targets = frozenset(e.dst for e in edges)
            if kind in ("table-call", "table") and len(edges) > 1:
                if targets in target_sets:
                    out.row(f"{'  ' * (level + 1)}[{kind} @{h(site)}] the same {len(edges)} targets as "
                            f"@{h(target_sets[targets])}{count_text(edges[0].dst, site)[0]}", depth=level + 1,
                            via=f"{kind} @{h(site)}", candidates=[g.label(e.dst) for e in edges])
                    continue
                target_sets[targets] = site
            for edge in edges:
                walk(edge.dst, level + 1, f"[{edge.kind} @{h(edge.site)}] " if edge.site >= 0 else f"[{edge.kind}] ",
                     edge.site)
    walk(entry, 0, "", None)


def paths_to(g: Game, out: Out, entry: int, limit: int) -> None:
    out.note(f"shortest caller chains from roots (vectors, config entries, task seeds, ?dispatch) to {g.label(entry)}")
    if roots_of(g, entry):
        kinds = kinds_text(g.routines[entry].kinds) if entry in g.routines else "dispatch"
        out.note(f"  {g.label(entry)} is itself a root ({kinds})")
    found = 0
    queue = deque([(entry, [])])
    seen = {entry}
    while queue and found < limit:
        node, chain = queue.popleft()
        if node != entry and roots_of(g, node):
            kinds = kinds_text(g.routines[node].kinds) if node in g.routines else "unresolved computed transfers"
            steps = " → ".join(f"[{e.kind} @{h(e.site) if e.site >= 0 else '-'}] {g.label(e.dst)}" for e in chain)
            out.row(f"  {g.label(node)} ({kinds}) {steps}", root=g.label(node), chain=[
                {"kind": e.kind, "site": e.site, "to": g.label(e.dst)} for e in chain])
            found += 1
            continue
        for edge in g.callers.get(node, []):
            if edge.src not in seen:
                seen.add(edge.src)
                queue.append((edge.src, [edge] + chain))


# ---------------------------------------------------------------- writes
def loops_of(g: Game, entry: int) -> list[tuple[int, int, str]]:
    loops = []
    pcs = set(g.routines[entry].pcs)
    for pc in g.routines[entry].pcs:
        insn = g.code[pc]
        kind, targets = g._transfer_targets(pc, insn)
        for t in targets:
            if kind in ("branch", "cond") and t <= pc and t in pcs:
                loops.append((t, pc, insn.mnemonic))
    return loops


def cmd_writes(args) -> None:
    g = load(args)
    out = Out(args)
    regions, spans = set(), []
    names = {name for _, _, name in REGIONS} | set(LOG_LAYER.values())
    for text in args.region or []:
        match = re.fullmatch(r"(\S+?)(?:-|\.\.)(\S+)", text)
        if text not in names and match:
            spans.append((canonical(parse_address(match.group(1))), canonical(parse_address(match.group(2)))))
            regions.add("span")
        elif text in names:
            regions.add(text)
        else:
            raise SystemExit(f"f3a: unknown region {text!r}; regions: {', '.join(sorted(names))}, or START-END")
    if args.video:
        regions |= VIDEO_REGIONS
    # Discovery-log layer names cover two regions each.
    regions |= {name for name, layer in LOG_LAYER.items() if layer in regions}
    if args.targets:
        entries = []
        for text in args.targets:
            entry = routine_for(g, resolve_target(g, text))
            if entry not in entries:  # two PCs of one routine: list it once
                entries.append(entry)
    else:
        if not regions and args.field is None:
            raise SystemExit("f3a: writes needs routine targets, --region, --video or --field")
        entries = g.entry_list
    ranges: dict[tuple[str, int], list[int]] = defaultdict(list)
    unplaced: list[tuple[int, list]] = []
    stack_args: list[tuple[int, list]] = []

    def show(entry: int, rows: list) -> None:
        if out.full:
            out.hidden += len(rows)  # no header without rows
            return
        out.note(routine_header(g, entry))
        for pc, operand, region, hit, where, loop, step, insn, *rest in rows:
            arg_offset = rest[0] if rest else None
            extra = {"arg_offset": arg_offset} if arg_offset is not None else {}
            loop_text = f"loop {h(loop[0])}..{h(loop[1])} ({loop[2]})" if loop else ""
            out.row(f"  {h(pc)}  {ROLE_MARK[operand.role]:<2} {access_size(insn)}  {region:<9} {g.evidence(pc):>9}  "
                    f"{insn_text(insn):<34} {where}" + (f"  [{', '.join(p for p in (loop_text, step) if p)}]"
                                                        if loop_text or step else ""),
                    routine=g.label(entry), pc=pc, role=operand.role, size=access_size(insn), region=region,
                    regions=sorted(hit), where=where, loop=loop_text, step=step, evidence=g.evidence(pc),
                    text=insn_text(insn), **extra)

    for entry in entries:
        loops = loops_of(g, entry)
        states = g.states(entry)
        rows = []
        for pc in g.routines[entry].pcs:
            insn = g.code[pc]
            state = states.get(pc, {})
            for operand in g.operands(insn):
                if operand.role not in ("write", "rmw"):
                    continue
                if args.field is not None and (operand.mode not in ("areg", "areg-idx", "memi")
                                               or operand.disp != args.field or operand.reg == "a5"):
                    continue
                addresses, where, region = resolve_store(g, entry, operand, state)
                if region == "stack":
                    continue  # pushes and locals are not data stores
                arg_offset = None
                if region == "stack-arg":
                    val = state.get(operand.reg) if operand.reg else None
                    if val and val.kind == "arg":
                        arg_offset = val.value
                hit = {region_of(a) for a in addresses} or {region}
                if spans and any(lo <= a <= hi for a in addresses for lo, hi in spans):
                    hit.add("span")
                inside = [l for l in loops if l[0] <= pc <= l[1]]
                loop = min(inside, key=lambda l: l[1] - l[0]) if inside else None
                step = ""
                if operand.postinc:
                    step = f"{operand.postinc:+d}/access"
                elif operand.reg in state and state[operand.reg].step:
                    step = f"{state[operand.reg].step:+d} so far"
                rows.append((pc, operand, region, hit, where, loop, step, insn, arg_offset))
        if regions:
            # Keep unknown-destination stores only in routines that provably store to a wanted region:
            # the same loop usually writes through a pointer the tracker could not follow.
            if not any(r[3] & regions for r in rows):
                sa = [r for r in rows if r[2] == "stack-arg"]
                unk = [r for r in rows if r[2] == "?"]
                if sa:
                    stack_args.append((entry, sa))
                if unk:
                    unplaced.append((entry, unk))
                continue
            rows = [r for r in rows if r[3] & regions or r[2] in ("?", "stack-arg")]
        if not rows:
            continue
        for pc, operand, region, hit, where, loop, step, insn, *rest in rows:
            quality = "stack-arg" if region == "stack-arg" else "unknown" if region == "?" else "mixed" if region == "mixed" or len(hit) > 1 else "placed"
            for layer in {("?" if r == "stack-arg" else LOG_LAYER.get(r, r)) for r in hit if r in VIDEO_REGIONS - {"palette"} or r in ("?", "stack-arg")}:
                ranges[(layer, entry)].append((pc, quality))
        if args.ranges:
            continue
        show(entry, rows)
    if stack_args:
        stack_args.sort(key=lambda item: -max(g.hits.get(g.block_of.get(r[0], r[0]), 0) for r in item[1]))
    if unplaced:
        unplaced.sort(key=lambda item: -max(g.hits.get(g.block_of.get(r[0], r[0]), 0) for r in item[1]))
    if stack_args or unplaced:
        stores = sum(len(rows) for _, rows in unplaced)
        if args.unresolved and not args.ranges:
            if stack_args:
                sa_shown = stack_args if out.limit == 0 else stack_args[:20]
                out.note(f"-- {len(stack_args)} routines store through stack arguments (no static callers):")
                for entry, rows in sa_shown:
                    show(entry, rows)
            if unplaced:
                shown = unplaced if out.limit == 0 else unplaced[:20]
                out.note(f"-- {len(unplaced)} more routines store only through pointers the static pass cannot place "
                         f"(any of them may write {', '.join(sorted(regions))}); {len(shown)} most executed"
                         + (" (--limit 0 for all)" if len(shown) < len(unplaced) else "") + ":")
                for entry, rows in shown:
                    show(entry, rows)
        else:
            sa_stores = sum(len(rows) for _, rows in stack_args)
            if sa_stores:
                out.note(f"-- not listed: {sa_stores} stores in {len(stack_args)} routines go through stack arguments "
                         f"(no static callers; --unresolved lists them)")
            if unplaced:
                hot = ", ".join(g.label(e) for e, _ in unplaced[:5])
                out.note(f"-- not listed: {stores} stores in {len(unplaced)} routines go through pointers the static pass "
                         f"cannot place (they may hit {', '.join(sorted(regions))}); most executed: {hot}. "
                         + ("`f3a writes --region R --unresolved` (without --ranges) lists them; " if args.ranges
                            else "--unresolved lists them; ")
                         + "`f3a run --all-writers` + `f3a discover LOG --ranges` observe the real writers")
    if args.ranges:
        out.note("// Store-PC ranges per discovery-log layer (games/<id>/video/video.cpp style), static only. "
                 "{first, last} store PC per routine, not a tight set.")
        out.note("// placed = destination resolved to this layer; mixed = candidates span layers/RAM (weak); "
                 "unknown = pointer not followed. Observed writers: f3a run --all-writers, f3a discover LOG --ranges.")
        wanted = sorted({LOG_LAYER.get(r, r) for r in regions if r in VIDEO_REGIONS - {"palette"}}
                        | {layer for layer, _ in ranges})
        for layer in wanted:
            members: dict[tuple[int, int], list] = {}
            for (lay, entry), stores in ranges.items():
                if lay != layer or (args.placed_only and not any(q == "placed" for _, q in stores)):
                    continue
                pcs = sorted({pc for pc, _ in stores})
                members.setdefault((pcs[0], pcs[-1]), []).append((entry, stores))  # shared tails print once
            if layer == "?" and args.placed_only:
                continue
            title = "? (stores whose layer is unknown, in routines that also write video)" if layer == "?" else layer
            out.note(f"// {title}: " + (f"{len(members)} ranges" if members else "none found statically"))
            for (start, end), owners in sorted(members.items()):
                entry, stores = owners[0]
                quality = Counter(q for _, q in {s for s in stores})
                tags = ", ".join(f"{quality[q]} {q}" for q in ("placed", "mixed", "stack-arg", "unknown") if quality[q])
                also = f"; same code in {', '.join(g.label(e) for e, _ in owners[1:4])}" if len(owners) > 1 else ""
                out.row(f"{{0x{start:06x}, 0x{end:06x}}}, // {g.label(entry)}: {tags}{also}",
                        layer=layer, routine=g.label(entry), start=h(start), end=h(end),
                        placed=quality["placed"], mixed=quality["mixed"],
                        stack_arg=quality["stack-arg"], unknown=quality["unknown"],
                        also=[g.label(e) for e, _ in owners[1:]],
                        pcs={h(pc): q for pc, q in sorted(set(stores))},
                        entry_hits=g.hits.get(entry, 0) if g.hits else None)
    if not out.shown and not out.hidden and not args.ranges:
        out.note("no stores found" + (f" into {', '.join(sorted(args.region or []))}" if args.region else ""))
    out.done()


def cmd_struct(args) -> None:
    from .structs import cmd_struct as _cmd_struct
    _cmd_struct(args)


# ---------------------------------------------------------------- run / discover
def game_executable(g: Game, explicit: str | None, instrumented: bool = False) -> Path:
    """The game's built executable; `instrumented` wants an F3_PROFILE_INSTRUMENT=ON build (watch runs), else
    a plain build is preferred (instrumentation slows the game down)."""
    if explicit:
        return Path(explicit)
    found: list[tuple[bool, Path]] = []
    for cache in sorted(ROOT.glob("build*/CMakeCache.txt")):
        text = cache.read_text(errors="replace")
        if re.search(rf"^F3_GAME:STRING={re.escape(g.id)}$", text, re.M):
            for name in (g.id, g.id.rstrip("j")):
                exe = cache.parent / name
                if exe.is_file() and os.access(exe, os.X_OK):
                    found.append((bool(re.search(r"^F3_PROFILE_INSTRUMENT:BOOL=ON$", text, re.M)), exe))
                    break
    for want in ([True] if instrumented else [False, True]):
        for has, exe in found:
            if has == want:
                return exe
    if instrumented:
        rom = f"-DF3_ROM_DIR={g.rom_dir} " if g.rom_dir else ""
        raise SystemExit(f"f3a: --watch needs an instrumented {g.id} build; configure and build one once:\n"
                         f"  cmake -S . -B build-{g.id}-instrument -G Ninja -DCMAKE_BUILD_TYPE=Release -DF3_GAME={g.id} "
                         f"{rom}-DF3_PROFILE_INSTRUMENT=ON\n  cmake --build build-{g.id}-instrument --target "
                         f"{g.id.rstrip('j')}")
    raise SystemExit(f"f3a: no built executable for {g.id}; build it or pass --exe")


def run_game(exe: Path, g: Game, frames: int, extra: list[str]) -> tuple[str, int]:
    command = [str(exe), "--rom-dir", str(g.rom_dir), "--headless", "--no-audio", "--unthrottled",
               "--frames", str(frames)] + extra
    result = subprocess.run(command, capture_output=True, text=True)
    text = result.stdout + result.stderr
    match = re.search(r"Unknown argument: (\S+)", text)
    if match:
        raise SystemExit(f"f3a: {exe} predates {match.group(1)}; rebuild it first: "
                         f"cmake --build {exe.parent} --target {exe.name}")
    return text, result.returncode


SUMMARY_KEYS = ("frame_crc", "cycles", "native_blocks", "fallback_instructions", "pc")


def summary_fields(text: str) -> dict:
    fields = {}
    for line in text.splitlines():
        for key, value in re.findall(r"(\w+)=(\S+)", line):
            if key in SUMMARY_KEYS:
                fields[key] = value
    return fields


def default_out(g: Game, what: str) -> Path:
    """Per-shell-session default output directory, so parallel shells do not overwrite each other."""
    return ROOT / "build" / "f3a" / f"{g.id}-{what}-s{os.getsid(0)}"


def bmp_to_png(source: Path, target: Path) -> None:
    """The dump's uncompressed 24/32-bit BMP as an RGB PNG (image viewers and agent tools read PNG)."""
    data = source.read_bytes()
    offset, width, height, bits = (int.from_bytes(data[10:14], "little"), int.from_bytes(data[18:22], "little"),
                                   int.from_bytes(data[22:26], "little", signed=True),
                                   int.from_bytes(data[28:30], "little"))
    if len(data) < 30 or data[:2] != b"BM" or bits not in (24, 32):
        raise SystemExit(f"f3a: {source} is not an uncompressed 24/32-bit BMP")
    step = bits // 8
    stride = (width * step + 3) & ~3
    if len(data) < offset + stride * abs(height):
        return  # the last frame of a run --until stopped can be cut off mid-write
    rows = range(abs(height)) if height < 0 else range(height - 1, -1, -1)  # positive height: bottom-up
    raw = bytearray()
    for y in rows:
        line = data[offset + y * stride: offset + y * stride + width * step]
        pixels = bytearray(width * 3)
        pixels[0::3], pixels[1::3], pixels[2::3] = line[2::step], line[1::step], line[0::step]  # BGR(A) → RGB
        raw += b"\0" + pixels

    def chunk(kind: bytes, body: bytes) -> bytes:
        return len(body).to_bytes(4, "big") + kind + body + zlib.crc32(kind + body).to_bytes(4, "big")
    header = width.to_bytes(4, "big") + abs(height).to_bytes(4, "big") + bytes([8, 2, 0, 0, 0])
    target.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
                       + chunk(b"IEND", b""))


ENTRY_LINE = re.compile(r"^ENTRY pc=0x([0-9a-f]+) frame=(\d+) hits=(\d+) total=(\d+)$")
INDIRECT_LINE = re.compile(r"^INDIRECT\s+site=0x([0-9a-fA-F]+)\s+target=0x([0-9a-fA-F]+)\s+count=(\d+)$")


def read_indirect_log(path: Path) -> dict[tuple[int, int], int]:
    counts: dict[tuple[int, int], int] = defaultdict(int)
    if not path.is_file():
        return counts
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = INDIRECT_LINE.match(line)
        if m:
            site = int(m.group(1), 16)
            target = int(m.group(2), 16)
            counts[(site, target)] += int(m.group(3))
    return counts


def write_indirect_log(path: Path, counts: dict[tuple[int, int], int]) -> None:
    lines = ["# f3rt indirect targets v1\n"]
    for (site, target), count in sorted(counts.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        lines.append(f"INDIRECT site=0x{site:06x} target=0x{target:06x} count={count}\n")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(lines))


class LogTail:
    """New complete lines of a log the game flushes line by line while it runs."""

    def __init__(self, path: Path):
        self.path, self.offset = path, 0

    def lines(self) -> list[str]:
        if not self.path.is_file():
            return []
        with self.path.open("rb") as handle:
            handle.seek(self.offset)
            chunk = handle.read()
        complete = chunk[:chunk.rfind(b"\n") + 1]  # keep a partial last line for the next poll
        self.offset += len(complete)
        return complete.decode(errors="replace").splitlines()


def run_game_until(exe: Path, g: Game, frames: int, extra: list[str], transcript: Path, discovery: Path,
                   entries: Path, store_targets: set[int], entry_targets: set[int]) -> tuple[str, int | None, dict]:
    """Run the game, tailing its logs; stop once every target ran. Routines in `store_targets` count when they
    store to video/control RAM (discovery log), PCs in `entry_targets` when they execute (entry watch log).
    Returns (output, exit code, {target: {frame, pc, via}}); a run stopped early has exit code None."""
    command = [str(exe), "--rom-dir", str(g.rom_dir), "--headless", "--no-audio", "--unthrottled",
               "--frames", str(frames)] + extra
    reached: dict[int, dict] = {}
    discovery_tail, entries_tail = LogTail(discovery), LogTail(entries)
    with transcript.open("w") as sink:
        process = subprocess.Popen(command, stdout=sink, stderr=subprocess.STDOUT)
        while True:
            code = process.poll()
            for line in discovery_tail.lines() if store_targets else []:
                match = LOG_LINE.match(line)
                if not match or match.group(1) != "NEW" or match.group(2) == "video-fallback":
                    continue
                fields = dict(re.findall(r"(\w+)=(\S+)", match.group(3)))
                pc = int(fields["pc"], 16)
                for owner in g.owners.get(pc) or [g.routine_of(pc)]:
                    if owner in store_targets and owner not in reached:
                        reached[owner] = {"frame": int(fields["frame"]), "pc": pc,
                                          "via": f"first store pc 0x{pc:06x} into {fields.get('layer', 'sprites')}"}
            for line in entries_tail.lines() if entry_targets else []:
                match = ENTRY_LINE.match(line)
                pc = int(match.group(1), 16) if match else None
                if pc in entry_targets and pc not in reached:
                    reached[pc] = {"frame": int(match.group(2)), "pc": pc, "via": "executed"}
            if code is not None:
                break
            if len(reached) == len(store_targets | entry_targets):
                process.terminate()
                process.wait()
                code = None
                break
            time.sleep(0.2)
    text = transcript.read_text(errors="replace")
    match = re.search(r"Unknown argument: (\S+)", text)
    if match:
        raise SystemExit(f"f3a: {exe} predates {match.group(1)}; rebuild it first: "
                         f"cmake --build {exe.parent} --target {exe.name}")
    return text, code, reached


def code_targets(g: Game, items: list[str] | None, option: str, entries_only: bool) -> dict[int, str]:
    targets = {}
    for text in (part.strip() for item in items or [] for part in item.split(",") if part.strip()):
        pc = resolve_target(g, text)
        owner = g.routine_of(pc)
        where = g.label(owner) if owner is not None else "no routine"
        if entries_only and pc not in g.routines:
            raise SystemExit(f"f3a: {option} {text}: 0x{pc:06x} is not a routine entry (it is in {where}); "
                             "without --watch, --until sees routines through their video stores. Add --watch to "
                             "stop on any instruction")
        if pc not in g.code:
            raise SystemExit(f"f3a: {option} {text}: 0x{pc:06x} is not a decoded instruction start")
        targets[pc] = text
    return targets

def target_label(g: Game, pc: int) -> str:
    """A routine's name at its entry; elsewhere the instruction address and its routine."""
    if pc in g.routines:
        return g.label(pc)
    owner = g.routine_of(pc)
    return f"0x{pc:06x} in {g.label(owner)}" if owner is not None else f"0x{pc:06x}"


def cmd_run(args) -> None:
    g = load(args)
    watch = code_targets(g, args.watch, "--watch", entries_only=False)
    until = code_targets(g, args.until, "--until", entries_only=not watch)
    watch.update(until if watch else {})  # an instrumented run watches its --until targets by execution
    exe = game_executable(g, args.exe, instrumented=bool(watch or getattr(args, "indirect", False)))
    outdir = Path(args.out) if args.out else default_out(g, str(args.frames))
    outdir.mkdir(parents=True, exist_ok=True)
    extra = list(args.extra)
    log, entries = outdir / "discovery.log", outdir / "entries.log"
    if until and args.check_inert:
        raise SystemExit("f3a: --check-inert compares whole runs; --until cuts the run short")
    all_writers = args.all_writers or bool(until and not watch)  # known writers are only logged with --discovery-all
    if args.discovery or all_writers:
        log.unlink(missing_ok=True)
        extra += ["--discovery-log", str(log)] + (["--discovery-all"] if all_writers else [])
    if watch:
        entries.unlink(missing_ok=True)
        (outdir / "run.profile").unlink(missing_ok=True)
        extra += ["--profile-out", str(outdir / "run.profile"), "--watch-log", str(entries),
                  "--watch-entries", ",".join(f"{pc:x}" for pc in watch)]
    if getattr(args, "indirect", False):
        indirect_log = outdir / "indirect.log"
        indirect_log.unlink(missing_ok=True)
        if not watch:
            (outdir / "run.profile").unlink(missing_ok=True)
            extra += ["--profile-out", str(outdir / "run.profile")]
        extra += ["--indirect-log", str(indirect_log)]
    if args.inputs:
        script = Path(args.inputs)
        if not script.is_file():
            raise SystemExit(f"f3a: no input script at {script}")
        if script.resolve() != (outdir / "inputs.txt").resolve():
            shutil.copyfile(script, outdir / "inputs.txt")  # the run directory records what was pressed
        extra += ["--inputs", str((outdir / "inputs.txt").resolve())]
    if args.dump_every:
        extra += ["--dump-dir", str(outdir / "dumps"), "--dump-start", str(args.dump_start),
                  "--dump-every", str(args.dump_every)]
    print(f"$ [{g.id}] {exe} --frames {args.frames} {' '.join(extra)}")
    (outdir / "game").write_text(g.id + "\n")
    reached: dict = {}
    if until:
        text, code, reached = run_game_until(exe, g, args.frames, extra, outdir / "run.txt", log, entries,
                                             set() if watch else set(until), set(until) if watch else set())
    else:
        text, code = run_game(exe, g, args.frames, extra)
        (outdir / "run.txt").write_text(text)
    fields = summary_fields(text)
    if code is None:
        last = max(r["frame"] for r in reached.values())
        print(f"stopped at frame ~{last}: every --until target ran (the run was cut short, so there is no "
              f"end-of-run summary; dumps and logs are complete up to that frame)")
    else:
        print(f"exit={code} " + " ".join(f"{k}={v}" for k, v in fields.items()))
    if until or watch:
        out = Out(args)
        for pc, name in until.items():
            hit = reached.get(pc)
            label = target_label(g, pc)
            if hit:
                out.row(f"  reached     {label:<22} frame {hit['frame']:<6} {hit['via']}",
                        target=label, reached=True, **hit)
            else:
                out.row(f"  NOT reached {label:<22} (" + ("never executed" if watch else
                        "no store to video/control RAM") + f" in {args.frames} frames)",
                        target=label, reached=False, frame=None, pc=h(pc), via=None)
        if len(reached) < len(until) and not watch:
            out.note("--until without --watch only sees routines that store to video/control RAM themselves; "
                     "add --watch (instrumented build) to stop on any routine or branch target")
        if watch:
            seen: dict[int, list[tuple[int, int]]] = defaultdict(list)
            for line in entries.read_text().splitlines() if entries.is_file() else []:
                match = ENTRY_LINE.match(line)
                if match:
                    seen[int(match.group(1), 16)].append((int(match.group(2)), int(match.group(3))))
            out.note(f"executions per watched instruction ({entries}: one ENTRY line per frame it ran in)")
            for pc in watch:
                runs = seen.get(pc, [])
                label = target_label(g, pc)
                if runs:
                    out.row(f"  {label:<22} frames {runs[0][0]}..{runs[-1][0]}: ran in {len(runs)} frame"
                            f"{'s' * (len(runs) != 1)}, {sum(n for _, n in runs)} times", target=label, pc=h(pc),
                            first=runs[0][0],
                            last=runs[-1][0], frames=len(runs), executions=sum(n for _, n in runs))
                else:
                    out.row(f"  {label:<22} never executed", target=label, pc=h(pc), first=None, last=None,
                            frames=0, executions=0)
                if pc in g.routines and len(runs) <= 1:
                    r_kinds = g.routines[pc].kinds
                    if "task" in r_kinds or "seed" in r_kinds:
                        resumes = [res for res, task in g.resume_points.items() if task == pc]
                        if resumes:
                            resumes.sort(key=lambda p: (-g.hits.get(p, 0), p) if g.hits else p)
                            suggest = ", ".join(h(p) for p in resumes[:3])
                            out.note(f"{label} is a task entry that ran in at most 1 frame; consider watching a resume point: {suggest}")
        out.done()
        code = code or 0
    for line in text.splitlines():  # game diagnostics worth seeing without opening run.txt
        if re.match(r"^(flicker_shadows|DISCOVERY|warning|error)\b", line, re.I):
            print(f"  {line}")
    if code:
        errors = [line for line in text.splitlines() if re.search(r"cannot|error|fail|missing", line, re.I)]
        raise SystemExit(f"f3a: {exe.name} exited {code}: " + ("; ".join(errors[:3]) or text[-600:])
                         + f" (full output: {outdir / 'run.txt'})")
    if args.discovery or args.all_writers:
        print(f"discovery log: {log}; next: f3a discover {log}" + (" --ranges" if args.all_writers else ""))
    if args.dump_every:
        dumps = outdir / "dumps"
        if not any(dumps.glob("frame_*")):
            raise SystemExit(f"f3a: no dumps were written to {dumps} (frames < --dump-start {args.dump_start}?)")
        if args.keep:
            keep = {name.strip() for item in args.keep for name in item.split(",") if name.strip()}
            if "graphics.bin" in keep:
                keep.add("sprite_writers.bin")
            for path in dumps.glob("frame_*/*"):
                if path.name == "rendered.bmp" and "rendered.png" in keep:
                    bmp_to_png(path, path.with_suffix(".png"))
                if path.name not in keep and path.name != "cpu.json":
                    path.unlink()
        print(f"dumps: {dumps} (frame_NNNN/{', '.join(sorted(p.name for p in next(dumps.glob('frame_*')).iterdir()))})"
              f"; dumps are taken at the end of each emulated frame. next: f3a records {outdir} --scan ram")
    if args.check_inert:
        plain, _ = run_game(exe, g, args.frames, list(args.extra))
        base = summary_fields(plain)
        same = all(base.get(k) == fields.get(k) for k in SUMMARY_KEYS if k in base)
        print(("INERT" if same else "NOT INERT") + ": instrumented vs plain " +
              " ".join(f"{k}={base.get(k)}/{fields.get(k)}" for k in SUMMARY_KEYS if k in base))


LOG_LINE = re.compile(r"^(NEW|REPEAT|SUM) (sprite-stray|video-write|video-fallback) (.*)$")


def parse_log(path: Path) -> list[dict]:
    entries = []
    for line in path.read_text().splitlines():
        match = LOG_LINE.match(line)
        if not match:
            continue
        fields = dict(re.findall(r'(\w+)=("[^"]*"|\S+)', match.group(3)))
        fields = {k: v.strip('"') for k, v in fields.items()}
        entries.append({"tag": match.group(1), "category": match.group(2), **fields})
    return entries


def cmd_discover(args) -> None:
    g = load(args, Path(args.log).parent if args.log else None)
    out = Out(args)
    if args.log:
        log = Path(args.log)
        if log.is_dir():
            log = log / "discovery.log"  # the `f3a run --out` directory
        if not log.is_file():
            raise SystemExit(f"f3a: no discovery log at {log} (f3a run --discovery or --all-writers writes one)")
    else:
        exe = game_executable(g, args.exe)
        outdir = Path(args.out) if args.out else default_out(g, f"discover{args.frames}")
        outdir.mkdir(parents=True, exist_ok=True)
        log = outdir / "discovery.log"
        out.note(f"[{g.id}] running {exe.name} headless for {args.frames} frames (attract mode: no input) → {log}")
        text, code = run_game(exe, g, args.frames, ["--discovery-log", str(log)] + (["--discovery-all"] if args.all else []))
        if code:
            raise SystemExit(f"f3a: game exited {code}:\n{text[-2000:]}")
    entries = parse_log(log)
    final = [e for e in entries if e["tag"] == "SUM"] or [e for e in entries if e["tag"] == "NEW"]
    text = log.read_text()
    header = text.split("\n", 1)[0]
    observed_all = "# ALL observed writers" in text
    units = "emit_units=yes" in header
    if observed_all:
        out.note(f"{log}: every observed video/control writer (known=1: already listed in games/{g.id}/video/)")
    else:
        out.note(f"{log}: only writers NOT already listed in games/{g.id}/video/ (known writers suppressed"
                 + ("; sprite RAM is accounted by emit units: see sprite-stray" if units else "")
                 + "). `--all` logs every observed writer")
    groups: dict[tuple[str, int | None], list[dict]] = defaultdict(list)
    for entry in final:
        if entry["category"] in ("video-write", "sprite-stray"):
            pc = int(entry["pc"], 16)
            owners = g.owners.get(pc) or [g.routine_of(pc)]
            groups[(entry.get("layer", "sprites" if entry["category"] == "sprite-stray" else "?") +
                    ("" if entry["category"] == "video-write" else " (stray)"), min(owners))].append(entry)
    out.note(f"{len(final)} entries in {len(groups)} (layer, routine) groups; frames are 1-based emulated frames")
    for (layer, entry), items in sorted(groups.items(), key=lambda kv: (kv[0][0], kv[0][1] or 0)):
        pcs = sorted({int(i["pc"], 16) for i in items})
        count = sum(int(i.get("count", 0)) for i in items)
        first = min(int(i.get("frame", 0)) for i in items)
        known_pcs = sorted({int(i["pc"], 16) for i in items if i.get("known") == "1"})
        unknown_pcs = sorted({int(i["pc"], 16) for i in items if i.get("known") != "1"})
        known = len(known_pcs)
        if getattr(args, "unknown_only", False) and observed_all and not unknown_pcs:
            continue
        sharers = sorted(set().union(*(g.owners.get(p, []) for p in pcs)) - {entry})
        label = g.label(entry) if entry is not None else "?"
        also = f" (shared code: also {', '.join(g.label(s) for s in sharers[:3])})" if sharers else ""
        known_pcs_h = [h(p) for p in known_pcs]
        unknown_pcs_h = [h(p) for p in unknown_pcs]
        if args.ranges:
            status = ""
            if observed_all:
                if known_pcs and not unknown_pcs:
                    status = ", known"
                elif unknown_pcs and not known_pcs:
                    pass
                elif unknown_pcs:
                    status = f", unknown: {', '.join(unknown_pcs_h)}"
            out.row(f"{{0x{pcs[0]:06x}, 0x{pcs[-1]:06x}}}, // {layer}: {label}{also}, {len(pcs)} PCs, "
                    f"{count} bytes, first frame {first}{status}",
                    layer=layer, routine=label, start=h(pcs[0]), end=h(pcs[-1]), pcs=[h(p) for p in pcs],
                    known_pcs=known_pcs_h, unknown_pcs=unknown_pcs_h,
                    count=count, first_frame=first, known=known)
            continue
        pcs_text = ", ".join(h(p) for p in pcs[:6]) + (f" +{len(pcs) - 6}" if len(pcs) > 6 else "")
        known_text = ""
        if observed_all:
            if known_pcs and not unknown_pcs:
                known_text = f"known={known}/{len(pcs)} "
            elif not known_pcs and unknown_pcs:
                known_text = "unknown "
            else:
                known_text = f"known={known}/{len(pcs)} unknown: {', '.join(unknown_pcs_h)} "
        out.row(f"  {layer:<16} {label:<22} first_frame={first:<6} bytes={count:<9} "
                + f"{known_text}pcs: {pcs_text}{also}",
                layer=layer, routine=label, first_frame=first, count=count, pcs=[h(p) for p in pcs],
                known_pcs=known_pcs_h, unknown_pcs=unknown_pcs_h, known=known)
    for entry in final:
        if entry["category"] == "video-fallback":
            out.row(f"  fallback {entry.get('component')}: {entry.get('reason')} (frame {entry.get('frame')}, "
                    f"count {entry.get('count', '?')})", **entry)
    if groups and not args.ranges:
        out.note("next: f3a discover LOG --ranges (observed ranges per layer), f3a writes <routine> (static view)")
    out.done()


# ---------------------------------------------------------------- records
DUMP_FILES = {"ram": ("mainram.bin", 0x400000, 0x1ffff), "palette": ("palette.bin", 0x440000, None),
              "control": ("control.bin", 0x660000, None), "shared": ("shared.bin", 0xc00000, None)}


def dump_location(address: int) -> tuple[str, int]:
    region = region_of(address)
    if region in DUMP_FILES:
        name, base, mask = DUMP_FILES[region]
        offset = address - base
        return name, (offset & mask) if mask else offset
    if 0x600000 <= address < 0x640000:
        return "graphics.bin", address - 0x600000
    raise SystemExit(f"f3a: {h(address)} ({region}) is not in a dump file")


def load_frames(root: Path, frames: str | None) -> list[tuple[int, Path]]:
    if not any(root.glob("frame_*")) and (root / "dumps").is_dir():
        root = root / "dumps"  # accept the `f3a run --out` directory
    dirs = sorted((int(p.name.split("_")[1]), p) for p in root.glob("frame_*") if p.is_dir())
    if frames:
        lo, _, hi = frames.partition(":")
        dirs = [(n, p) for n, p in dirs if n >= int(lo or 0) and n <= int(hi or 1 << 60)]
    if not dirs:
        raise SystemExit(f"f3a: no frame_NNNN dumps under {root} (make them with `f3a run --dump-every N`)")
    return dirs


def series_class(values: list[int]) -> str:
    """const | periodN (repeats every N frames, N<=8) | counter±[/Nf] (steps every N frames, measured
    between its changes, so a counter that starts late is not averaged with its idle stretch;
    /~Nf = irregular steps) | varies."""
    if len(set(values)) == 1:
        return "const"
    n = len(values)
    for period in range(2, 9):
        if n >= 2 * period and all(values[i] == values[i + period] for i in range(n - period)):
            return f"period{period}"
    diffs = [(b - a) & 0xff for a, b in zip(values, values[1:])]
    steps = {d for d in diffs if d}
    changes = [i for i, d in enumerate(diffs) if d]
    if len(steps) == 1 and len(changes) >= 3:
        sign = "+" if steps.pop() < 0x80 else "-"
        gaps = {b - a for a, b in zip(changes, changes[1:])}
        if len(gaps) == 1:
            gap = gaps.pop()
            return f"counter{sign}" if gap == 1 else f"counter{sign}/{gap}f"
        return f"counter{sign}/~{round((changes[-1] - changes[0]) / (len(changes) - 1))}f"
    return "varies"


def periodic_bits(values: list[int]) -> list[str]:
    """Bits that repeat with a short period, e.g. ['b0:p4'] for bit 0 of a half-rate frame counter."""
    found = []
    for bit in range(8):
        kind = series_class([(v >> bit) & 1 for v in values])
        if kind.startswith("period"):
            found.append(f"b{bit}:p{kind[6:]}")
    return found


def cmd_records(args) -> None:
    root = Path(args.dumps)
    frames = load_frames(root, args.frames)
    numbers = [n for n, _ in frames]
    gaps = {b - a for a, b in zip(numbers, numbers[1:])}
    dense = gaps == {1}
    out = Out(args)
    record_options = [f"--{name}" for name in ("base", "stride", "show", "check", "where", "rel")
                      if getattr(args, name, None) is not None]
    if (args.watch or args.scan) and record_options:
        raise SystemExit(f"f3a: {' '.join(record_options)} belong to record mode (--base/--stride) and do nothing "
                         f"with {'--watch' if args.watch else '--scan'}; run them separately")
    out.note(f"{len(frames)} frames {numbers[0]}..{numbers[-1]} (step {','.join(map(str, sorted(gaps))) or '-'})"
             + ("" if dense or len(frames) == 1 else "  NOTE: not consecutive; toggle/counter detection needs --dump-every 1"))
    cache: dict[tuple[int, str], bytes] = {}

    def read(n: int, path: Path, name: str) -> bytes:
        key = (n, name)
        if key not in cache:
            cache[key] = (path / name).read_bytes()
        return cache[key]

    def address_of(text: str) -> int:
        if text.strip().lower().startswith("a5"):
            return resolve_target(load(args, root), text)
        return canonical(parse_address(text))

    if args.watch:
        specs = []
        for text in args.watch:
            addr_text, _, size_text = text.partition(":")
            specs.append((address_of(addr_text), int(size_text or "1", 0)))
        out.note("frame  " + "  ".join(f"{h(a)}/{s}" for a, s in specs))
        columns: list[list[str]] = [[] for _ in specs]
        previous = None
        for n, path in frames:
            cells = []
            for column, (address, size) in zip(columns, specs):
                name, offset = dump_location(address)
                data = read(n, path, name)
                cells.append(data[offset:offset + size].hex())
                column.append(cells[-1])
            if not args.changes or cells != previous:
                out.row(f"{n:5}  " + "  ".join(f"{c:>{len(h(a)) + 2}}" for c, (a, _) in zip(cells, specs)),
                        frame=n, values=cells)
            previous = cells
        out.done()
        numbers_seen = [n for n, _ in frames]
        for (address, size), column in zip(specs, columns):
            change_frames = [numbers_seen[i + 1] for i, (a, b) in enumerate(zip(column, column[1:])) if a != b]
            values = sorted(set(column), key=lambda v: int(v, 16))
            first = f", first change at frame {change_frames[0]}" if change_frames else ""
            out.note(f"  {h(address)}/{size}: {len(change_frames)} changes{first}, {len(values)} distinct, "
                     f"min {values[0]} max {values[-1]}, {series_class([int(v, 16) for v in column])}")
        return
    elif args.scan:
        region = args.scan
        start, end = next(((s, e) for s, e, name in REGIONS if name == region), (None, None))
        if start is None:
            raise SystemExit(f"f3a: unknown region {region}")
        if region == "ram":
            end = 0x420000
        name, base = dump_location(start)
        series = [read(n, p, name) for n, p in frames]
        size = min(len(s) for s in series)
        out.note(f"bytes in {region} that repeat with a period of 2-8 frames or count every 1-8 frames "
                 f"(periodN = repeats every N frames; counter+/Nf = steps once every N frames); "
                 f"writers = static stores to that exact address (absolute/a5 operands only)")
        try:
            writers = load(args, root).static_writers()
        except SystemExit:
            writers = {}  # no game selected: no writer column
        hits = 0
        for offset in range(base, min(size, base + (end - start))):
            values = [s[offset] for s in series]
            kind = series_class(values)
            if kind in ("const", "varies"):
                continue
            if not args.all and "/" in kind and int(kind.split("/")[1].strip("~f")) > 8:
                continue  # slow counters drown the frame-rate ones; --all shows them
            bits = periodic_bits(values)
            address = start + offset - base
            changes = sum(1 for a, b in zip(values, values[1:]) if a != b)
            distinct = sorted(set(values))
            pcs = writers.get(address, [])
            who = ", ".join(f"{h(pc)}" for pc in pcs[:3]) + (f" +{len(pcs) - 3}" if len(pcs) > 3 else "") if pcs else "-"
            hits += 1
            out.row(f"  {h(address)}  {kind:<11} {' '.join(bits) or '-':<20} {changes:4} changes  "
                    f"{len(distinct):3} distinct {distinct[0]:02x}..{distinct[-1]:02x}  writers: {who:<22} first: "
                    + " ".join(f"{v:02x}" for v in values[:8]), address=address, kind=kind, periodic_bits=bits,
                    changes=changes, distinct=len(distinct), min=distinct[0], max=distinct[-1], values=values[:32],
                    writers=[h(pc) for pc in pcs])
        if not hits:
            out.note("  no periodic or counting bytes in these frames (const/varies only; --all includes slow counters)")
    else:
        if args.base is None or args.stride is None:
            raise SystemExit("f3a: records needs --watch, --scan or --base/--stride")
        base = address_of(args.base)
        stride = int(args.stride, 0)
        length = int(args.size, 0) if args.size else stride
        name, offset0 = dump_location(base)
        data = [(n, read(n, p, name)) for n, p in frames]
        active_off, active_bit = (int(part, 0) for part in args.active.split(".")) if args.active else (None, None)

        def active(blob: bytes, k: int) -> bool:
            record = blob[offset0 + k * stride: offset0 + k * stride + length]
            if record == b"\xff" * len(record):
                return False  # uninitialised (RAM test fill)
            if active_off is None:
                return any(record)
            return bool(record[active_off] >> active_bit & 1)

        if args.show is not None:
            shown = [(n, b) for n, b in data if args.show in (n, -1)]
            if not shown:
                raise SystemExit(f"f3a: frame {args.show} is not dumped; frames {data[0][0]}..{data[-1][0]} "
                                 f"(step {','.join(map(str, sorted(gaps))) or '-'})")
            n, blob = shown[0]
            out.note(f"frame {n}: {args.count} records of {length:#x} bytes at {h(base)} stride {stride:#x}"
                     + ("" if args.all else " (inactive/all-zero records hidden; --all)"))
            for k in range(args.count):
                if not args.all and not active(blob, k):
                    continue
                rec = blob[offset0 + k * stride: offset0 + k * stride + length]
                out.row(f"  [{k:3}] {h(base + k * stride)}  " + " ".join(rec[i:i + 2].hex() for i in range(0, len(rec), 2)),
                        record=k, address=base + k * stride, hex=rec.hex())
            out.done()
            return
        rom = load(args, root).rom if args.rel or args.check or args.where else b""
        where = compile_expr(args.where, rom) if args.where else None
        samples = [(n, k, b[offset0 + k * stride: offset0 + k * stride + length])
                   for n, b in data for k in range(args.count) if args.all or active(b, k)]
        if where is not None:
            samples = [s for s in samples if where(s[2])]
        scope = f"{len(data)} frames × {args.count} slots" + (f", where {args.where}" if where else "")
        if args.check:
            check = compile_expr(args.check, rom)
            failed = [s for s in samples if not check(s[2])]
            records = len({k for _, k, _ in samples})
            out.note(f"{args.check}: holds for {len(samples) - len(failed)}/{len(samples)} active records "
                     f"({100 * (len(samples) - len(failed)) / max(1, len(samples)):.1f}%; {records} slots, {scope})")
            if failed and check.sides:
                lhs_code, rhs_code = check.sides
                diffs = Counter()
                for _, _, row in failed:
                    diffs[check(row, lhs_code) - check(row, rhs_code)] += 1
                out.note("  left - right on failures: " + ", ".join(f"{d:+#x}×{c}" for d, c in diffs.most_common(8)))
            for n, k, row in failed:
                values = f"  left {check(row, check.sides[0]):#x} right {check(row, check.sides[1]):#x}" if check.sides else ""
                out.row(f"  frame {n:5} [{k:3}] {h(base + k * stride)}{values}  {row[:16].hex(' ', 2)}",
                        frame=n, record=k, address=base + k * stride, hex=row.hex())
            out.done()
            return
        if args.rel:
            rel_off_text, _, rel_size_text = args.rel.partition(":")
            target = (int(rel_off_text, 0), int(rel_size_text or "1", 0))
            if target[1] not in (1, 2, 4) or target[0] + target[1] > length:
                raise SystemExit("f3a: --rel OFF[:SIZE] needs SIZE 1, 2 or 4 inside the record")
            relations, info = search([row for _, _, row in samples], length, target, rom)
            out.note(f"T = {field_name(target)} over {info['samples']} distinct active records "
                     f"({scope}), {info['distinct']} distinct values")
            if info["distinct"] < 2:
                out.note("  T is constant: nothing to relate")
            elif not relations:
                out.note("  no field or field pair determines T exactly. It may depend on state outside the record, "
                         "or several record types mix (narrow with --where), or T changes after it is derived; "
                         "test a hypothesis from the code with --check")
            for r in relations:
                parts = [field_name(f) + (f"&{mask:#x}" if mask != (1 << 8 * f[1]) - 1 else "")
                         for f, mask in zip(r.fields[1:] if r.kind == "residual" else r.fields,
                                            r.bits or (None,) * 3) if mask is not None] \
                    or [field_name(f) for f in (r.fields[1:] if r.kind == "residual" else r.fields)]
                what = (f"T - {field_name(r.fields[0])} = f({', '.join(parts)})" if r.kind == "residual"
                        else f"T = f({', '.join(parts)})")
                fmt = (lambda k: "(" + ", ".join(f"{x:#x}" for x in k) + ")") if isinstance(next(iter(r.mapping)), tuple) \
                    else (lambda k: f"{k:#x}")
                preview = ", ".join(f"{fmt(k)}→{v:#x}" for k, v in sorted(r.mapping.items())[:8]) \
                    + (f" … ({len(r.mapping)} keys)" if len(r.mapping) > 8 else "")
                out.row(f"  {r.kind:<8} {what:<44} keys {r.keys:3}  {r.samples / r.keys:5.1f}/key  {r.form}\n"
                        f"           {preview}",
                        kind=r.kind, relation=what, keys=r.keys, samples=r.samples, form=r.form,
                        mapping={fmt(k): v for k, v in r.mapping.items()})
            out.note("  &mask = bits of that field that change the result; f() without a form is a lookup "
                     "the record data supports (check it against the code that writes T: f3a writes --field)")
            out.done()
            return
        records = [k for k in range(args.count) if args.all or any(active(b, k) for _, b in data)]
        out.note(f"{len(records)}/{args.count} records × {length:#x} bytes at {h(base)} stride {stride:#x} "
                 f"(records never active hidden): per-offset behaviour over frames")
        out.note("  off   const periodic counter varies  periodic bits (records)        sample values")
        for off in range(length):
            classes = Counter()
            bit_hits = Counter()
            samples = Counter()
            for k in records:
                values = [b[offset0 + k * stride + off] for _, b in data if active_off is None or active(b, k)]
                if not values or (not any(values) and not args.all):
                    classes["zero"] += 1
                    continue
                kind = series_class(values)
                classes["counter" if kind.startswith("counter") else "periodic" if kind.startswith("period") else kind] += 1
                samples.update(values)
                bit_hits.update(periodic_bits(values))
            if classes.get("zero", 0) == len(records):
                continue
            if classes.keys() <= {"const", "zero"} and not args.all and len(samples) <= 1:
                continue
            bits = " ".join(f"{bit}×{count}" for bit, count in sorted(bit_hits.items()))
            out.row(f"  ${off:02x}   {classes['const']:5} {classes['periodic']:8} {classes['counter']:7} {classes['varies']:6}"
                    f"  {bits or '-':<30}  " + " ".join(f"{v:02x}" for v, _ in samples.most_common(6)),
                    offset=off, **dict(classes), periodic_bits=dict(bit_hits),
                    samples=[v for v, _ in samples.most_common(8)])
    out.done()


# ---------------------------------------------------------------- sprites
def cmd_sprites(args) -> None:
    out = Out(args)
    frames = load_frames(Path(args.dumps), args.frames)
    root = Path(args.dumps)
    g = None
    try:
        g = load(args, root)
    except SystemExit:
        pass

    filter_pc: int | None = None
    filter_routine: str | None = None
    if getattr(args, "writer", None) is not None:
        w_raw = args.writer.strip()
        try:
            filter_pc = int(w_raw, 16)
        except ValueError:
            filter_routine = w_raw.lower()

    warned_missing_writers = False
    for n, path in frames if args.frames else frames[-1:]:
        graphics = (path / "graphics.bin").read_bytes()
        entries, summary = decode(graphics[:0x10000], args.bank)
        sw_file = path / "sprite_writers.bin"
        has_writers = sw_file.is_file() and sw_file.stat().st_size == 16384
        writers: list[int] = []
        if has_writers:
            sw_bytes = sw_file.read_bytes()
            writers = [int.from_bytes(sw_bytes[i:i + 4], "little") for i in range(0, len(sw_bytes), 4)]
        elif not warned_missing_writers:
            out.note("  NOTE: sprite_writers.bin missing in dump (predates writer tracking or was dropped by --keep)")
            warned_missing_writers = True

        if summary["end"] is not None:
            end = f"list end (jump to itself) at slot {summary['end']:#05x}"
        elif summary["last_used"]:
            end = (f"no jump-to-itself end: the walk covers all 0x400 slots; last non-empty entry is bank "
                   f"{summary['last_used'][0]} slot {summary['last_used'][1]:#05x} (zero fill after it)")
        else:
            end = "every entry is empty"
        switches = "; ".join(f"slot {s:#05x} of bank {b} switches the walk to bank {t}" for b, s, t in summary["switches"])
        out.note(f"frame {n}: walk starts in bank {args.bank} ({h(0x600000 + 0x8000 * args.bank)}); "
                 f"entries read from bank {'+'.join(map(str, summary['banks_read']))}"
                 + (f" ({switches})" if switches else "") + f"; {summary['visited']} entries walked, "
                 f"{summary['drawn']} drawn, {summary['jumps']} jumps, {summary['commands']} commands; {end}")
        out.note("  slot  address    tile    col pri  x       y       scale x/y  control          words / notes")
        for e in entries:
            empty = not any(e.words)
            if not args.all and (empty and not e.notes):
                continue
            if args.tile is not None and e.tile != args.tile:
                continue

            writer_pc = None
            writer_routine = None
            owner_entry = None
            if has_writers:
                offset = (e.address - 0x600000) & 0xffff
                pc = writers[offset >> 4]
                if pc:
                    writer_pc = pc
                    if g is not None:
                        owner_entry = g.routine_of(pc)
                        writer_routine = g.label(owner_entry) if owner_entry is not None else None

            if filter_pc is not None or filter_routine is not None:
                if writer_pc is None:
                    continue
                if filter_pc is not None and writer_pc != filter_pc:
                    continue
                if filter_routine is not None:
                    matches = False
                    if writer_routine and writer_routine.lower() == filter_routine:
                        matches = True
                    if owner_entry is not None and f"sub_{owner_entry:06x}" == filter_routine:
                        matches = True
                    if not matches:
                        continue

            writer_text = ""
            if writer_pc is not None:
                routine_part = f" ({writer_routine})" if writer_routine else ""
                writer_text = f"  ← {h(writer_pc)}{routine_part}"

            notes = list(e.notes)
            if not e.drawn and not empty and not e.notes:
                notes.append("tile 0: not drawn; sets position/colour/scale state for the entries after it")
            notes_text = "; ".join(notes)
            out.row(f"  {e.slot:#05x} {h(e.address)}  {e.tile:#07x} {e.color:#04x} {e.color >> 6}  "
                    f"{e.x:7.1f} {e.y:7.1f}  {e.scale_x:#05x}/{e.scale_y:#05x}  {control_text(e.control):<16} "
                    + " ".join(f"{w:04x}" for w in e.words) + (f"  {notes_text}" if notes_text else "")
                    + writer_text,
                    frame=n, slot=e.slot, address=e.address, tile=e.tile, color=e.color, x=e.x, y=e.y,
                    scale_x=e.scale_x, scale_y=e.scale_y, control=control_text(e.control), drawn=e.drawn,
                    words=[f"{w:04x}" for w in e.words], notes=notes,
                    writer=writer_pc, writer_routine=writer_routine)
    out.done()
    out.note("  all-zero entries hidden (--all shows every walked entry). Who wrote an entry: "
             "f3a run --all-writers + f3a discover LOG, f3a writes --region sprites, or f3a mametap (landmakrj)")


# ---------------------------------------------------------------- mametap
LUA_TEMPLATE = r"""-- generated by tools/f3a mametap: write/read taps logging the executing instruction (CURPC)
if _G.f3a_started then return end
_G.f3a_started = true
local OUT = "@OUT@"
local FRAMES = @FRAMES@
local PC_LO, PC_HI = @PCLO@, @PCHI@
local SPECS = { @SPECS@ }
local cpu = manager.machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local screen = manager.machine.screens[":screen"]
local f = io.open(OUT, "w")
f:write("frame,pc,kind,address,data,mask\n")
local last = nil
local function logger(kind)
    return function(offset, data, mask)
        local pc = cpu.state["CURPC"].value  -- CURPC = start of the executing instruction; PC is post-fetch
        if pc < PC_LO or pc > PC_HI then return nil end
        local row = string.format("%d,0x%06x,%s,0x%06x,0x%08x,0x%08x", screen:frame_number(), pc, kind, offset, data, mask)
        if row ~= last then f:write(row, "\n") end  -- drop adjacent duplicate events
        last = row
        return nil
    end
end
_G.f3a_taps = {}
local function install()
    if #_G.f3a_taps > 0 then
        for _, tap in ipairs(_G.f3a_taps) do tap:reinstall() end  -- reset remaps the space and drops taps
        return
    end
    for i, s in ipairs(SPECS) do
        local tap
        if s[3] == "w" then tap = space:install_write_tap(s[1], s[2], "f3a_w" .. i, logger("w"))
        else tap = space:install_read_tap(s[1], s[2], "f3a_r" .. i, logger("r")) end
        _G.f3a_taps[#_G.f3a_taps + 1] = tap  -- keep handles global or the taps are collected
    end
end
install()
_G.f3a_reset = emu.add_machine_reset_notifier(install)
_G.f3a_frame = emu.register_frame_done(function()
    if screen:frame_number() >= FRAMES then
        f:close()
        manager.machine:exit()
    end
end)
"""


def cmd_mametap(args) -> None:
    g = load(args)
    specs = []
    for kind, ranges in (("w", args.write or []), ("r", args.read or [])):
        for text in ranges:
            lo_text, _, hi_text = text.partition(":")
            lo = resolve_target(g, lo_text)
            hi = resolve_target(g, hi_text) if hi_text else lo + 1
            specs.append(f"{{0x{lo & ~3:x}, 0x{hi | 3:x}, \"{kind}\"}}")
    if not specs:
        raise SystemExit("f3a: mametap needs --write and/or --read ranges (ADDR or LO:HI)")
    pc_lo, pc_hi = (0, 0xffffffff)
    if args.pc:
        lo_text, _, hi_text = args.pc.partition(":")
        pc_lo, pc_hi = parse_address(lo_text), parse_address(hi_text or lo_text)
    outdir = Path(args.out) if args.out else default_out(g, "mame")
    outdir.mkdir(parents=True, exist_ok=True)
    csv = outdir / "taps.csv"
    lua = (LUA_TEMPLATE.replace("@OUT@", str(csv)).replace("@FRAMES@", str(args.frames))
           .replace("@PCLO@", hex(pc_lo)).replace("@PCHI@", hex(pc_hi)).replace("@SPECS@", ", ".join(specs)))
    script = outdir / "taps.lua"
    script.write_text(lua)
    mame = Path(args.mame or ROOT.parent / "tools" / "mame-baseline" / "f3")
    rompath = stage_mame_roms(g, outdir)
    command = [str(mame), g.id, "-rompath", rompath, "-video", "none", "-sound", "none", "-nothrottle",
               "-skip_gameinfo", "-autoboot_delay", "0", "-autoboot_script", str(script),
               "-seconds_to_run", str(args.frames // 50 + 10),
               "-nvram_directory", str(outdir / "nvram"), "-cfg_directory", str(outdir / "cfg")]
    print(f"lua: {script}\ncsv: {csv}\n$ {' '.join(command)}")
    if not args.run:
        return
    if not mame.is_file():
        raise SystemExit(f"f3a: MAME not found at {mame}; pass --mame")
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode or not csv.is_file():
        raise SystemExit(f"f3a: MAME exited {result.returncode}:\n{(result.stdout + result.stderr)[-2000:]}")
    rows = csv.read_text().splitlines()[1:]
    by_pc = Counter((r.split(",")[1], r.split(",")[2]) for r in rows)
    print(f"{len(rows)} events; top writer/reader PCs:")
    for (pc, kind), count in by_pc.most_common(args.limit or 20):
        address = int(pc, 16)
        print(f"  {pc}  {kind}  {count:7}  {g.owner_label(address)}")


def stage_mame_roms(g: Game, outdir: Path) -> str:
    """MAME wants <rompath>/<set>/ with MAME file names; link the game's ROM dir under the set name.
    Land Maker's short sound dumps need padding, which tools/mame/stage_roms.py already does."""
    if g.id == "landmakrj":
        staged = ROOT / "tools" / "mame" / "staged_roms"
        if not (staged / "landmakrj.zip").is_file():
            subprocess.run([sys.executable, str(ROOT / "tools" / "mame" / "stage_roms.py"), "--out-zip",
                            str(staged / "landmakrj.zip")], check=True)
        return str(staged)
    link_root = outdir / "rompath"
    link_root.mkdir(exist_ok=True)
    link = link_root / g.id
    if not link.exists():
        link.symlink_to(g.rom_dir.resolve())
    return str(link_root)


# ---------------------------------------------------------------- graph
def cmd_use(args) -> None:
    if not (ROOT / "games" / args.game / "config.toml").is_file():
        raise SystemExit(f"f3a: no games/{args.game}/config.toml")
    path = session_default()
    if path is None:
        raise SystemExit("f3a: use needs a terminal to remember the game, and this shell has none (each command "
                         f"runs in a fresh shell); pass --game {args.game} or export F3A_GAME={args.game}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(args.game + "\n")
    print(f"default game on {terminal()}: {args.game} ({path}); --game or F3A_GAME override it. "
          f"Other terminals: `export F3A_GAME={args.game}`")


def cmd_vectors(args) -> None:
    g = load(args)
    out = Out(args)
    targets = Counter(g.vectors.values())
    default = targets.most_common(1)[0][0]
    out.note(f"{g.id} 68k vectors (a5={h(g.a5) if g.a5 is not None else '?'}); the most common target {h(default)} "
             f"({targets[default]} vectors) is treated as the default handler"
             + ("" if args.all else "; it and vectors that point at no code are hidden (--all shows them)"))
    for number, target in sorted(g.vectors.items()):
        if not args.all and (target == default or target not in g.code):
            continue
        name = VECTOR_NAMES.get(number, f"v{number}")
        if target in g.routines:
            routine = g.routines[target]
            what = f"{routine.name:<14} insns={len(routine.pcs):<4} entry_hits={hits_of(g, target) or '-'}"
            first = g.code[target]
            if base_mnemonic(first) in ("bra", "jmp") or (len(routine.pcs) <= 2 and base_mnemonic(first) != "rte"):
                what += f"  first: {insn_text(first)}"
        elif target in g.code:
            what = f"inside {g.owner_label(target)}"
        else:
            what = "not code (unused or data)"
        out.row(f"  {number:3} {name:<14} {h(target)}  {what}", vector=number, name=name, address=target, routine=what)
    out.done()


def cmd_graph(args) -> None:
    from .graph import write_graph
    g = load(args)
    path = Path(args.out or ROOT / "build" / "f3a" / f"{g.id}-callgraph.html")
    path.parent.mkdir(parents=True, exist_ok=True)
    write_graph(g, path, with_disassembly=not args.no_disassembly)
    print(f"wrote {path} ({path.stat().st_size // 1024} KiB): open it in a browser")


def build_instrumented(g: Game) -> Path:
    build_dir = ROOT / f"build-{g.id}-instrument"
    target = "landmakr" if g.id in ("landmakrj", "landmakr") else g.id  # CMakeLists.txt F3_GAME_EXECUTABLE
    if not g.rom_dir or not Path(g.rom_dir).is_dir():
        raise SystemExit(f"f3a: ROM directory unknown or not found: {g.rom_dir}; pass --rom-dir or set F3_ROM_DIR")
    print(f"configuring {build_dir.name} ({g.id}, instrumented)...")
    config_cmd = [
        "cmake", "-S", str(ROOT), "-B", str(build_dir), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", f"-DF3_GAME={g.id}",
        f"-DF3_ROM_DIR={g.rom_dir}", "-DF3_PROFILE_INSTRUMENT=ON",
    ]
    res = subprocess.run(config_cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise SystemExit(f"f3a: cmake configure failed:\n{res.stdout}\n{res.stderr}")
    print(f"building {target} in {build_dir.name}...")
    build_cmd = ["cmake", "--build", str(build_dir), "--target", target]
    process = subprocess.Popen(build_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in iter(process.stdout.readline, ""):
        line_clean = line.strip()
        if line_clean:
            print(f"  {line_clean}")
    process.wait()
    if process.returncode != 0:
        raise SystemExit(f"f3a: cmake build failed (exit {process.returncode})")
    exe = build_dir / target
    if not (exe.is_file() and os.access(exe, os.X_OK)):
        raise SystemExit(f"f3a: built executable not found: {exe}")
    return exe


def cmd_profile(args) -> None:
    g = load(args)
    out_path = Path(args.out) if args.out else ROOT / "profiles" / f"{g.id}.profile"
    indirect_out_path = Path(f"{args.out}.indirect") if args.out else ROOT / "profiles" / f"{g.id}.indirect"
    frozen_profile = (ROOT / "profiles" / "landmakrj.profile").resolve()
    frozen_indirect = (ROOT / "profiles" / "landmakrj.indirect").resolve()
    if out_path.resolve() == frozen_profile:
        raise SystemExit(f"f3a: refusing to overwrite {out_path}: profiles/landmakrj.profile is frozen "
                         "(it feeds code generation); pass --out FILE to write elsewhere")
    if indirect_out_path.resolve() == frozen_indirect:
        raise SystemExit(f"f3a: refusing to overwrite {indirect_out_path}: profiles/landmakrj.indirect is frozen "
                         "(it feeds code generation); pass --out FILE to write elsewhere")

    try:
        exe = game_executable(g, getattr(args, "exe", None), instrumented=True)
    except SystemExit:
        if not args.build:
            raise SystemExit(f"f3a: no instrumented executable for {g.id}; pass --build to configure and build one")
        exe = build_instrumented(g)

    input_scripts: list[Path] = []
    for script_str in args.inputs or []:
        script_path = Path(script_str)
        if not script_path.is_file():
            raise SystemExit(f"f3a: no input script at {script_path}")
        input_scripts.append(script_path.resolve())

    with tempfile.TemporaryDirectory(prefix=f"f3a-profile-{g.id}-") as tmp_dir:
        tmp_path = Path(tmp_dir)
        runs: list[tuple[str, Path | None, Path, Path]] = [("attract mode", None, tmp_path / "attract.profile", tmp_path / "attract.indirect")]
        for index, script in enumerate(input_scripts, 1):
            runs.append((f"script {script.name}", script, tmp_path / f"input_{index}.profile", tmp_path / f"input_{index}.indirect"))

        for label, script, prof_file, indir_file in runs:
            extra = ["--profile-out", str(prof_file), "--indirect-log", str(indir_file)]
            if script is not None:
                extra += ["--inputs", str(script)]
            print(f"running {label} ({args.frames} frames)...")
            text, code = run_game(exe, g, args.frames, extra)
            if code != 0:
                errors = [line for line in text.splitlines() if re.search(r"cannot|error|fail|missing", line, re.I)]
                raise SystemExit(f"f3a: {exe.name} exited {code}: " + ("; ".join(errors[:3]) or text[-600:]))
            if not prof_file.is_file():
                raise SystemExit(f"f3a: run produced no profile at {prof_file}")

        merged = BlockProfile()
        merged_indirect = {}
        kept = out_path.is_file() and not args.replace
        if kept:
            merged.merge(read_profile(out_path))  # earlier runs (other scripts) stay counted
        if indirect_out_path.is_file() and not args.replace:
            merged_indirect = read_indirect_log(indirect_out_path)
        for label, script, prof_file, indir_file in runs:
            merged.merge(read_profile(prof_file))
            for k, v in read_indirect_log(indir_file).items():
                merged_indirect[k] = merged_indirect.get(k, 0) + v

        out_path.parent.mkdir(parents=True, exist_ok=True)
        merged.write(out_path)
        write_indirect_log(indirect_out_path, merged_indirect)
        alt_indirect = Path(args.out).with_suffix(".indirect") if args.out else None
        if alt_indirect and alt_indirect != indirect_out_path:
            write_indirect_log(alt_indirect, merged_indirect)

    total_runs = len(runs)
    total_frames = args.frames * total_runs
    total_insn_exec = sum(merged.hits.values())
    main_hit_addresses = {addr for (region, crc, addr), count in merged.hits.items() if region == "main" and count > 0}
    executed_routines = {entry for entry, r in g.routines.items()
                         if any(pc in main_hit_addresses or g.block_of.get(pc, pc) in main_hit_addresses for pc in r.pcs)}
    prior_routines_with_hits = {entry for entry, r in g.routines.items()
                                if any(pc in g.hits or g.block_of.get(pc, pc) in g.hits for pc in r.pcs)}
    gained_routines = executed_routines - prior_routines_with_hits

    run_desc = ("attract mode" if not input_scripts else f"attract mode + {len(input_scripts)} script"
                f"{'s' if len(input_scripts) != 1 else ''}") + (f", merged into the existing {out_path.name}" if kept else "")

    out = Out(args)
    out.row(
        f"profile written: {out_path}",
        path=str(out_path),
        runs=total_runs,
        frames=total_frames,
        instructions_executed=total_insn_exec,
        unique_instructions=len(main_hit_addresses),
        routines_executed=len(executed_routines),
        routines_gained=len(gained_routines),
        total_routines=len(g.routines),
    )
    out.note(f"  runs: {total_runs} ({run_desc})")
    out.note(f"  frames: {total_frames}" + (f" ({args.frames} per run)" if total_runs > 1 else ""))
    out.note(f"  instructions executed: {total_insn_exec:,} ({len(main_hit_addresses):,} unique addresses)")
    out.note(f"  routines executed: {len(executed_routines):,} of {len(g.routines):,} ({len(gained_routines):,} gained hits)")
    default_path = ROOT / "profiles" / f"{g.id}.profile"
    out.note(f"next: f3a now uses {out_path.name} automatically (e.g. `tools/f3a flow irq2 --tree 2 --game {g.id}`)"
             if out_path.resolve() == default_path.resolve() else
             f"next: pass `--profile {out_path}` to f3a commands to use it (only profiles/<game>.profile loads by default)")
    out.done()


# ---------------------------------------------------------------- main
def build_parser() -> argparse.ArgumentParser:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--game", help="game id under games/ (or env F3A_GAME, or `f3a use ID`)")
    common.add_argument("--rom-dir", help="ROM directory (default: env F3_ROM_DIR, else the build cache for this game)")
    common.add_argument("--profile", help="block profile (default profiles/<game>.profile if present)")
    common.add_argument("--symbols", help="symbols TOML (default games/<game>/analysis/symbols.toml if present)")
    common.add_argument("--a5", help="override the detected global a5 base")
    common.add_argument("--json", action="store_true", help="one JSON object per row")
    common.add_argument("--limit", type=int, default=200, help="max rows (0 = unlimited)")

    parser = argparse.ArgumentParser(prog="f3a", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=EPILOG)
    parser.add_argument("--game", dest="global_game", help="game id (also accepted after the command)")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("use", help="remember a default game for this terminal (agents: --game or F3A_GAME)")
    p.add_argument("game")
    p.set_defaults(func=cmd_use)

    p = sub.add_parser("vectors", parents=[common], help="68k vector table: reset, interrupts, traps and their routines")
    p.add_argument("--all", action="store_true", help="include unused vectors (pointing at the default handler)")
    p.set_defaults(func=cmd_vectors)

    p = sub.add_parser("dis", parents=[common], help="disassemble a routine (default) or a range, annotated")
    p.add_argument("target", help="PC, name, vector (irq2), a5-0x7cd7, or START..END for a linear range")
    p.add_argument("--count", type=int, help="linear decode of N instructions (or N data rows with --data)")
    p.add_argument("--from", dest="pc_from", help="only instructions at or after this PC (page a long routine)")
    p.add_argument("--to", dest="pc_to", help="only instructions at or before this PC")
    p.add_argument("--bytes", action="store_true", help="show raw instruction bytes")
    p.add_argument("--data", choices=["bytes", "words", "longs", "offsets"],
                   help="dump ROM as a data table instead of code; offsets = `jmp table(pc,dN)` word offset table "
                        "(each entry → table + word); tables stop where discovered code starts")
    p.set_defaults(func=cmd_dis)

    p = sub.add_parser("xref", parents=[common], help="who calls a routine / who reads or writes an address")
    p.add_argument("targets", nargs="*", help="PC, routine name, data address, a5-0x7cd7")
    p.add_argument("--field", metavar="OFF[.BIT]", help="every access to structure offset OFF through an address "
                   "register (reads, writes, bit tests), e.g. 0x1.7 for tests of bit 7 of byte +1")
    p.add_argument("--len", type=lambda v: int(v, 0), default=1, help="data target length in bytes")
    p.add_argument("--role", choices=["read", "write", "rmw", "addr"], help="only this access role (write includes rmw)")
    p.add_argument("--within", help="--field: only accesses whose resolved base lies in START-END (one structure "
                                    "array, e.g. 0x40b420-0x40fc20); unresolved bases are dropped")
    p.add_argument("--code", action="store_true", help="--field: only stores of code addresses (hook/callback "
                                                       "slots), plus long stores whose value is not static")
    p.add_argument("--data", action="store_true", help="treat a ROM address as data (tables), not code")
    p.add_argument("--all", action="store_true", help="also scan undiscovered ROM decodes (slow; may be data)")
    p.add_argument("--indexed", action="store_true", help="include indexed accesses whose base is within 0x400 below")
    p.add_argument("--sort", choices=["pc", "hits", "routine"], default="pc",
                   help="row order; routine = one row per routine with its site count")
    p.set_defaults(func=cmd_xref)

    p = sub.add_parser("flow", parents=[common], help="callers/callees/memory of a routine, root paths, call trees, ?dispatch")
    p.add_argument("target", help="PC, name or vector inside a routine, or ?dispatch for everything unresolved")
    p.add_argument("--path", type=int, nargs="?", const=5, help="show up to N caller chains from roots")
    p.add_argument("--tree", type=int, nargs="?", const=2, help="callee tree to depth N")
    p.add_argument("--observed", help="path to indirect.log, run dir, or profiles/<game>.indirect (default: profiles/<game>.indirect if present)")
    p.set_defaults(func=cmd_flow)

    p = sub.add_parser("writes", parents=[common], help="enumerate stores of routines (or every routine touching a region)")
    p.add_argument("targets", nargs="*", help="routine PCs or names")
    p.add_argument("--region", action="append", help="only stores into this region (repeatable): " +
                   ", ".join(name for _, _, name in REGIONS) + ", or an address range START-END (0x660000-0x66001f)")
    p.add_argument("--video", action="store_true", help="only stores into video regions")
    p.add_argument("--field", type=lambda v: int(v, 0), help="only stores at this displacement through an address "
                   "register, e.g. 0x1 for byte +1 of every record (includes unknown bases)")
    p.add_argument("--ranges", action="store_true", help="print only store-PC ranges per discovery-log layer")
    p.add_argument("--unresolved", action="store_true", help="with --region/--video: also list routines whose stores "
                   "all go through pointers the static pass cannot place (most executed first)")
    p.add_argument("--placed-only", action="store_true", help="with --ranges: drop ranges with no store resolved "
                   "to that layer (only mixed/unknown candidates)")
    p.set_defaults(func=cmd_writes)

    p = sub.add_parser("struct", parents=[common], help="list fields accessed through one address register as a structure")
    p.add_argument("routine", help="routine name or PC")
    p.add_argument("reg", help="address register (a0..a6)")
    p.add_argument("--depth", type=int, default=2, help="callee follow depth (default 2)")
    p.set_defaults(func=cmd_struct)

    p = sub.add_parser("run", parents=[common], help="run the game headless with discovery log and/or RAM dumps")
    p.add_argument("--frames", type=int, default=1200)
    p.add_argument("--discovery", action="store_true", help="write discovery.log (video writers missing from games/<id>/video/)")
    p.add_argument("--all-writers", action="store_true", help="discovery.log of EVERY observed video/control writer, "
                   "known ones tagged known=1, sprite RAM included (f3a discover LOG --ranges summarises it)")
    p.add_argument("--dump-start", type=int, default=1)
    p.add_argument("--dump-every", type=int, default=0, help="dump machine state every N frames (1 = dense)")
    p.add_argument("--check-inert", action="store_true", help="rerun without instrumentation and compare frame_crc/cycles")
    p.add_argument("--out", help="output directory (default build/f3a/<game>-<frames>-s<shell session>)")
    p.add_argument("--exe", help="game executable (default: found via build*/CMakeCache.txt)")
    p.add_argument("--keep", action="append", help="dump files to keep, comma-separated or repeated (e.g. "
                   "mainram.bin,graphics.bin); the rest are deleted after the run to save space (~1 MB/frame "
                   "otherwise). rendered.png keeps a PNG of the screen (rendered.bmp converted)")
    p.add_argument("--inputs", help="input script for the game's --inputs (coin, start, held controls, seeded "
                   "mashing, main-RAM pokes per frame range); copied into the run directory as inputs.txt. Format: "
                   "docs/developer/WORKFLOWS.md (Scripted input)")
    p.add_argument("--until", action="append", metavar="ROUTINE[,ROUTINE]",
                   help="stop as soon as every listed routine has run and report the frame each first did. Without "
                   "--watch a routine counts when it stores to video/control RAM (implies --all-writers); with "
                   "--watch, when it executes (any instruction address)")
    p.add_argument("--watch", action="append", metavar="PC[,PC]",
                   help="log the frames in which these instructions run (routine names or any instruction "
                   "address, e.g. a branch target, to tell 'never called' from 'called but the branch went the "
                   "other way'); needs an F3_PROFILE_INSTRUMENT=ON build of the game (f3a prints the cmake "
                   "commands if there is none). Log: entries.log in the run directory")
    p.add_argument("extra", nargs="*", help="extra arguments for the game after --")
    p.add_argument("--indirect", action="store_true", help="log observed targets of computed jmp/jsr sites (implies instrumented build, writes indirect.log in run dir)")
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("discover", parents=[common], help="group discovery-log writers by routine (observed, not static)")
    p.add_argument("log", nargs="?", help="existing discovery.log (default: run the game first)")
    p.add_argument("--frames", type=int, default=3000)
    p.add_argument("--exe")
    p.add_argument("--out", help="output directory for the run (default build/f3a/<game>-discoverN-s<shell session>)")
    p.add_argument("--all", action="store_true", help="log every observed writer, not only those missing from "
                   "games/<id>/video/ (needs a build with --discovery-all)")
    p.add_argument("--ranges", action="store_true", help="print {first, last} store PCs per (layer, routine), "
                   "video.cpp style")
    p.add_argument("--unknown-only", action="store_true", help="show only groups with at least one unknown PC")
    p.set_defaults(func=cmd_discover)

    p = sub.add_parser("records", parents=[common], help="analyse RAM dumps: watch values, find toggles/counters, decode records")
    p.add_argument("dumps", help="dump directory (or the `f3a run --out` directory containing dumps/)")
    p.add_argument("--frames", help="LO:HI frame filter")
    p.add_argument("--watch", nargs="+", help="ADDR[:SIZE] (or a5-0x7ebe:2) values per frame, with a change summary")
    p.add_argument("--changes", action="store_true", help="with --watch: print only frames where a value changed")
    p.add_argument("--scan", metavar="REGION", help="find periodic/counting bytes in a region (ram, sprites, …)")
    p.add_argument("--base", help="record array base address (hex, or a5±offset)")
    p.add_argument("--stride", help="record stride (0x80 hex or 128 decimal)")
    p.add_argument("--size", help="bytes per record to analyse (default stride; 0x.. hex or decimal)")
    p.add_argument("--count", type=lambda v: int(v, 0), default=16, help="number of records")
    p.add_argument("--active", help="OFFSET.BIT marking a live record, e.g. 0.7; only live records are analysed")
    p.add_argument("--show", type=int, nargs="?", const=-1, help="hex-dump the live records at frame N (default first)")
    p.add_argument("--rel", metavar="OFF[:SIZE]", help="which other fields of the record determine this field "
                   "(equal, offset, linear, ROM table lookup, field pairs, base field + f(others))")
    p.add_argument("--check", metavar="EXPR", help="test a hypothesis on every live record and list counterexamples, "
                   "e.g. '+0x10.w == +0x12.w + 2 * rom.b(0x1000 + +0x4.b)'; fields +OFF.b/.w/.l, rom.b/w/l(addr), "
                   "Python operators; 'FIELD == expr' compares modulo the field width")
    p.add_argument("--where", metavar="EXPR", help="only records where EXPR is true (same syntax as --check), "
                   "for --rel and --check, e.g. '+0x0.b & 0x80'")
    p.add_argument("--all", action="store_true", help="include all-zero records/offsets and slow counters")
    p.set_defaults(func=cmd_records)

    p = sub.add_parser("sprites", parents=[common], help="decode the hardware sprite list in RAM dumps (graphics.bin)")
    p.add_argument("dumps", help="dump directory (frame_NNNN/ or a `f3a run --out` directory)")
    p.add_argument("--frames", help="FIRST:LAST frames to decode (default: the last dump)")
    p.add_argument("--bank", type=int, choices=(0, 1), default=0, help="bank the walk starts in (commands switch it)")
    p.add_argument("--tile", type=lambda v: int(v, 0), help="only entries drawing this tile")
    p.add_argument("--all", action="store_true", help="also undrawn entries")
    p.add_argument("--writer", help="only entries last written by this routine (sub_xxxxxx, name) or PC (hex)")
    p.set_defaults(func=cmd_sprites)

    p = sub.add_parser("mametap", parents=[common], help="generate (and optionally run) a MAME Lua tap script")
    p.add_argument("--write", nargs="+", help="ADDR or LO:HI ranges to log writes for")
    p.add_argument("--read", nargs="+", help="ADDR or LO:HI ranges to log reads for")
    p.add_argument("--pc", help="only log accesses from PCs in LO:HI")
    p.add_argument("--frames", type=int, default=600)
    p.add_argument("--run", action="store_true", help="run MAME now and summarise the CSV")
    p.add_argument("--mame", help="MAME binary (default ../tools/mame-baseline/f3)")
    p.add_argument("--out")
    p.set_defaults(func=cmd_mametap)

    p = sub.add_parser("graph", parents=[common], help="write an interactive HTML call graph")
    p.add_argument("--out", help="HTML path (default build/f3a/<game>-callgraph.html)")
    p.add_argument("--no-disassembly", action="store_true", help="omit per-routine disassembly (smaller file)")
    p.set_defaults(func=cmd_graph)

    p = sub.add_parser("profile", parents=[common], help="record execution profile from attract mode and input scripts")
    p.add_argument("--frames", type=int, default=6000, help="frames per run (default: 6000)")
    p.add_argument("--inputs", action="extend", nargs="+", default=[], metavar="SCRIPT",
                   help="input script(s) to run in addition to attract mode")
    p.add_argument("--out", help="output profile path (default: profiles/<game>.profile)")
    p.add_argument("--build", action="store_true", help="configure and build build-<id>-instrument if missing")
    p.add_argument("--replace", action="store_true", help="start the profile fresh instead of merging these runs "
                   "into the existing file")
    p.add_argument("--exe", help="explicit instrumented executable")
    p.set_defaults(func=cmd_profile)

    return parser


def main(argv: list[str] | None = None) -> None:
    args = build_parser().parse_args(argv)
    try:
        args.func(args)
    except BrokenPipeError:
        pass


EPILOG = """
evidence column: xN = basic block executed N times in profiles/<game>.profile; rooted = statically
reached from vectors/entries/tables; code = only reached via profile seeds; decoded? = not proven code.
routine kinds: vector:irqN/trapN/reset, entry (config), seed (task/callback scan), call, table-call
(jump-table/devirtualized jsr), ref (code address taken by pea/lea/move #), task (contains a cooperative
task yield trap), resume (routine entered at a task yield resume point), dispatch? (profile-hit
entry with no static predecessor: entered via f3_dispatch).
edge kinds: call, table-call, table (jmp table), tail (branch into another routine), fall
(falls through into the next routine), regconst (jsr (aN) with aN loaded from a constant), regtable
(jsr (aN) with aN read from a constant ROM pointer table), ref, resume (task routine -> resume point
after cooperative yield), slot (candidate target of an unresolved jmp/jsr that reads its target from
structure offset d: a routine the program stores at offset d of some structure; an over-approximation),
dispatch (unresolved computed transfer → ?dispatch, or ?dispatch → dispatch-only entry).
roles: r read, w write, rw read-modify-write, & address taken (lea/pea).
"""
