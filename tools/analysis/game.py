"""Game model shared by every f3a command.

Code evidence comes from the recompiler's own discovery (recomp/discovery.py), so
the analysis sees the same instructions, jump-table edges and devirtualized
computed transfers the generated C was built from:

  rooted    reached by recursive discovery from vectors, config entry points,
            task/callback scans and scanned jump tables
  executed  inside a basic block whose entry was hit in profiles/<game>.profile
  decoded   only decodes (all-aligned); may be data or a misaligned stream

Routines are the intra-procedural flow closure of every entry. Anything the static
pass cannot attribute is kept visible instead of dropped: unresolved computed
jmp/jsr sites and profile-hit entries with no static predecessor both connect to
the pseudo routine DISPATCH ("?dispatch"), i.e. the runtime's f3_dispatch lookup.
"""
from __future__ import annotations

from bisect import bisect_right
import dataclasses
import copy
from dataclasses import dataclass, field
import os
from pathlib import Path
import re
import sys
import tomllib
import zlib

ROOT = Path(__file__).resolve().parents[2]
for _path in (ROOT / "build" / "python", ROOT):
    if str(_path) not in sys.path:
        sys.path.insert(0, str(_path))

from recomp.discovery import (  # noqa: E402  (path set up above)
    CALL_MNEMONICS, COND_BRANCH_MNEMONICS, TERMINAL_MNEMONICS, UNCOND_BRANCH_MNEMONICS,
    Cs, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_020, _computed_transfer,
    _resolve_target, discover, load_rom,
)
from recomp.emitter import _ea_extension_bytes, _pc_base  # noqa: E402
from capstone import m68k as M  # noqa: E402

DISPATCH = -1  # pseudo routine: f3_dispatch / anything the static pass cannot attribute

# Fixed Taito F3 main-CPU map. Layer names match runtime/discovery_log.cpp.
REGIONS = [
    (0x000000, 0x200000, "rom"), (0x400000, 0x440000, "ram"), (0x440000, 0x448000, "palette"),
    (0x4a0000, 0x4a0020, "io"), (0x4c0000, 0x4c0004, "timer"), (0x600000, 0x610000, "sprites"),
    (0x610000, 0x612000, "pf0"), (0x612000, 0x614000, "pf1"), (0x614000, 0x616000, "pf2"),
    (0x616000, 0x618000, "pf3"), (0x618000, 0x61a000, "pf2-alt"), (0x61a000, 0x61c000, "pf3-alt"),
    (0x61c000, 0x61e000, "text"), (0x61e000, 0x620000, "charram"), (0x620000, 0x630000, "lines"),
    (0x630000, 0x640000, "pivot"), (0x660000, 0x660040, "control"), (0xc00000, 0xc00800, "shared"),
    (0xc80000, 0xc80104, "sound-reset"),
]
VIDEO_REGIONS = {"sprites", "pf0", "pf1", "pf2", "pf3", "pf2-alt", "pf3-alt", "text", "charram",
                 "lines", "pivot", "control", "palette"}
# discovery_log.cpp folds charram into text and pivot into lines.
LOG_LAYER = {"charram": "text", "pivot": "lines"}

VECTOR_NAMES = {1: "reset", 2: "bus-error", 3: "address-error", 4: "illegal", 5: "zero-divide",
                6: "chk", 7: "trapv", 8: "privilege", 9: "trace", 24: "spurious"}
VECTOR_NAMES.update({24 + level: f"irq{level}" for level in range(1, 8)})
VECTOR_NAMES.update({32 + n: f"trap{n}" for n in range(16)})

READ_ONLY = {"tst", "cmp", "cmpa", "cmpi", "cmpm", "cmp2", "btst", "chk", "chk2", "bftst",
             "bfextu", "bfexts", "bfffo", "pack", "unpk"}
WRITE_ONLY = {"move", "movea", "moveq", "clr", "st", "sf", "shi", "sls", "scc", "scs", "sne",
              "seq", "svc", "svs", "spl", "smi", "sge", "slt", "sgt", "sle", "movep", "move16",
              "bfins", "lea"}
ADDRESS_ONLY = {"lea", "pea", "jmp", "jsr"}


def region_of(address: int) -> str:
    for start, end, name in REGIONS:
        if start <= address < end:
            return name
    return "unmapped"


def canonical(address: int) -> int:
    """Fold the work-RAM mirror (128 KiB repeated across 0x400000..0x43ffff)."""
    address &= 0xffffffff
    if 0x400000 <= address < 0x440000:
        return 0x400000 + ((address - 0x400000) & 0x1ffff)
    return address & 0xffffff


ALL_SCRATCH = ("a0", "a1", "a2", "a3", "a4", "a6", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7")


def register_list(bits: int) -> set[str]:
    """Capstone movem register mask: bits 0-7 = d0-d7, bits 8-15 = a0-a7."""
    return {(f"d{i}" if i < 8 else f"a{i - 8}") for i in range(16) if bits >> i & 1}


def ordered_registers(bits: int) -> list[str]:
    """movem register mask in memory order (d0..d7, a0..a7: lowest address first)."""
    return [(f"d{i}" if i < 8 else f"a{i - 8}") for i in range(16) if bits >> i & 1]


def parse_address(text: str) -> int:
    """Accept 0x9b32, $9b32, 9b32, 009b32 (hex) or -0x7cd7 offsets."""
    text = text.strip().replace("_", "")
    sign = -1 if text.startswith("-") else 1
    text = text.lstrip("+-")
    if text.startswith("$"):
        text = text[1:]
    return sign * int(text, 16)


def h(value: int) -> str:
    return f"0x{value:06x}"


def base_mnemonic(insn) -> str:
    return insn.mnemonic.split(".")[0].lower()


def access_size(insn) -> int:
    suffix = insn.mnemonic.rsplit(".", 1)[-1] if "." in insn.mnemonic else ""
    size = {"b": 1, "w": 2, "l": 4}.get(suffix)
    if size:
        return size
    return 1 if base_mnemonic(insn) in ("btst", "bset", "bclr", "bchg", "tas") else 4


def base_register(insn, op) -> str:
    """Address register of a memory operand. Capstone keeps (An), (An)+ and -(An) in op.reg and
    displacement/index forms in op.mem.base_reg."""
    if op.address_mode in (M.M68K_AM_REGI_ADDR, M.M68K_AM_REGI_ADDR_POST_INC, M.M68K_AM_REGI_ADDR_PRE_DEC):
        return insn.reg_name(op.reg) if op.reg else ""
    return insn.reg_name(op.mem.base_reg) if op.mem.base_reg else ""


@dataclass
class Operand:
    """One memory operand with whatever its address statically resolves to."""
    index: int
    role: str            # read | write | rmw | addr
    mode: str            # abs | pc | a5 | areg | areg-idx | pc-idx | memi
    address: int | None  # statically known effective address (base for indexed modes)
    reg: str = ""        # base address register for areg modes
    disp: int = 0
    index_reg: str = ""
    scale: int = 1
    indirect: bool = False
    postinc: int = 0      # +size for (An)+, -size for -(An)


@dataclass
class Routine:
    entry: int
    kinds: set = field(default_factory=set)  # vector:irq5, entry, call, table, ref, seed, dispatch?
    pcs: list = field(default_factory=list)
    name: str = ""


@dataclass
class Edge:
    src: int      # routine entry or DISPATCH
    dst: int      # routine entry or DISPATCH
    kind: str     # call | tail | fall | table | table-call | regconst | regtable | ref | slot | dispatch | resume
    site: int     # instruction PC making the transfer (-1 for synthetic)


class Game:
    def __init__(self, game: str, rom_dir: str | None = None, profile: str | None = None,
                 symbols: str | None = None, a5: int | None = None):
        self.id = game
        self.config_path = ROOT / "games" / game / "config.toml"
        if not self.config_path.is_file():
            raise SystemExit(f"f3a: no config for game {game!r} at {self.config_path}")
        self.rom_dir = Path(rom_dir or os.environ.get("F3_ROM_DIR") or self._cached_rom_dir())
        self.rom, self.config = load_rom(self.config_path, self.rom_dir)  # CRC/SHA1-checked lanes
        self.md = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        self.md.detail = True
        self.symbols_code, self.symbols_data = self._load_symbols(symbols)
        self._discover(profile)
        self.a5 = a5 if a5 is not None else self._detect_a5()
        self._build_routines()
        self._build_edges()

    # ---------------------------------------------------------------- loading
    def _cached_rom_dir(self) -> str:
        for cache in sorted(ROOT.glob("build*/CMakeCache.txt")):
            text = cache.read_text(errors="replace")
            if re.search(rf"^F3_GAME:STRING={re.escape(self.id)}$", text, re.M):
                match = re.search(r"^F3_ROM_DIR:PATH=(.+)$", text, re.M)
                if match and match.group(1).strip():
                    return match.group(1).strip()
        raise SystemExit(f"f3a: pass --rom-dir (or F3_ROM_DIR); no build*/CMakeCache.txt names ROMs for {self.id}")

    def _load_symbols(self, path: str | None):
        """Code labels from docs/notes routine pages (`?` while a hypothesis), then the symbols file, which wins."""
        code = self._note_labels()
        candidate = Path(path) if path else ROOT / "games" / self.id / "analysis" / "symbols.toml"
        if not candidate.is_file():
            if path:
                raise SystemExit(f"f3a: symbols file not found: {path}")
            return code, {}
        data = tomllib.loads(candidate.read_text())
        code.update({parse_address(k): v for k, v in data.get("code", {}).items()})
        data_syms = {}
        for key, value in data.get("data", {}).items():
            if isinstance(value, str):
                value = {"name": value}
            data_syms[canonical(parse_address(key))] = (value["name"], int(value.get("size", 1)))
        return code, data_syms

    def _note_labels(self) -> dict[int, str]:
        """docs/notes/routines/<game>-<8 hex>-<kebab-name>.md names that routine (docs/notes/SCHEMA.md)."""
        labels = {}
        for page in sorted((ROOT / "docs" / "notes" / "routines").glob(f"{self.id}-*.md")):
            match = re.fullmatch(rf"{re.escape(self.id)}-([0-9a-fA-F]{{8}})-([a-z0-9-]+)\.md", page.name)
            if not match:
                continue
            status = re.search(r"^status:\s*(\w+)", page.read_text(errors="replace"), re.M)
            if status and status.group(1) == "refuted":
                continue
            labels[int(match.group(1), 16)] = match.group(2).replace("-", "_") + (
                "" if status and status.group(1) == "confirmed" else "?")
        return labels

    def _discover(self, profile: str | None) -> None:
        config = copy.deepcopy(self.config)
        discovery = config.setdefault("discovery", {})
        discovery["coverage"] = "recursive"
        # Exclusions shrink the generated binary; analysis wants every rooted path.
        config.pop("exclude", None)
        rooted = discover(self.rom, config)
        self.rooted = set(rooted.instructions)
        self.seeds_proven = {int(s, 16) for s in rooted.report.get("proven_seeds", [])}
        self.seeds_speculative = {int(s, 16) for s in rooted.report.get("speculative_seeds", [])}
        self.hits: dict[int, int] = {}
        self.profile_path = Path(profile) if profile else ROOT / "profiles" / f"{self.id}.profile"
        if self.profile_path.is_file():
            self.hits = self._read_profile(self.profile_path)
        elif profile:
            raise SystemExit(f"f3a: profile not found: {profile}")
        if self.hits:
            discovery["entry_points"] = list(discovery.get("entry_points", [])) + sorted(
                pc for pc in self.hits if pc < len(self.rom) and not pc & 1)
            merged = discover(self.rom, config)
        else:
            merged = rooted
        self.code = merged.instructions
        self.branch_targets = merged.branch_targets
        self.indirect_targets = merged.indirect_targets
        self.config_tables = {int(str(t["address"]), 0) for t in discovery.get("jump_tables", [])
                              if isinstance(t, dict) and "address" in t}
        self.unresolved = {int(item["pc"], 16): item for item in merged.report.get("unresolved_branches", [])}
        # Instruction -> owning basic-block entry, for "executed" evidence.
        self.block_of: dict[int, int] = {}
        for entry, pcs in merged.blocks.items():
            for pc in pcs:
                self.block_of[pc] = entry
        self.sorted_pcs = sorted(self.code)
        self._decoded_cache: dict[int, object] = {}

    def _read_profile(self, path: Path) -> dict[int, int]:
        hits: dict[int, int] = {}
        crc = zlib.crc32(self.rom) & 0xffffffff
        for line in path.read_text().splitlines():
            parts = line.split()
            if len(parts) == 5 and parts[0] == "rom" and parts[1] == "main":
                size = int(parts[4], 16)
                image_crc = zlib.crc32(self.rom.ljust(size, b"\0")) & 0xffffffff
                if int(parts[2], 16) not in (crc, image_crc):
                    print(f"f3a: warning: {path} was recorded on a different ROM; ignoring it", file=sys.stderr)
                    return {}
            elif len(parts) == 5 and parts[0] == "hit" and parts[1] == "main":
                hits[int(parts[3], 16)] = hits.get(int(parts[3], 16), 0) + int(parts[4])
        return hits

    def _detect_a5(self) -> int | None:
        values = set()
        for pc in self.sorted_pcs:
            if pc not in self.rooted:
                continue
            insn = self.code[pc]
            ops = insn.operands
            if base_mnemonic(insn) in ("lea", "movea") and len(ops) == 2 and ops[1].type == M.M68K_OP_REG \
                    and insn.reg_name(ops[1].reg) == "a5":
                if ops[0].address_mode in (M.M68K_AM_ABSOLUTE_DATA_LONG, M.M68K_AM_IMMEDIATE,
                                           M.M68K_AM_ABSOLUTE_DATA_SHORT):
                    values.add(ops[0].imm & 0xffffffff)
        return values.pop() if len(values) == 1 else None

    # ---------------------------------------------------------------- evidence
    def evidence(self, pc: int) -> str:
        if pc in self.code:
            hits = self.hits.get(self.block_of.get(pc, pc), 0)
            if hits:
                return f"x{hits}"
            return "rooted" if pc in self.rooted else "code"
        return "decoded?"

    def insn_at(self, pc: int):
        """Discovered instruction, else an unrooted independent decode (may be data)."""
        insn = self.code.get(pc)
        if insn is not None:
            return insn
        if pc not in self._decoded_cache:
            chunk = self.rom[pc:pc + 24].ljust(24, b"\0")
            insn = next(self.md.disasm(chunk, pc, count=1), None)
            if insn is not None and (not insn.id or insn.mnemonic.startswith("dc")):
                insn = None  # Capstone SKIPDATA record: data, not an instruction
            self._decoded_cache[pc] = insn
        return self._decoded_cache[pc]

    # ---------------------------------------------------------------- routines
    def _transfer_targets(self, pc: int, insn) -> tuple[str, list[int]]:
        """Classify control flow: (kind, targets). kind is one of
        none, terminal, branch, cond, call, table-jump, table-call, unresolved-jump, unresolved-call."""
        base = base_mnemonic(insn)
        if base in TERMINAL_MNEMONICS:
            return "terminal", []
        if base not in CALL_MNEMONICS | UNCOND_BRANCH_MNEMONICS | COND_BRANCH_MNEMONICS:
            return "none", []
        if base in ("jmp", "jsr") and insn.operands and _computed_transfer(insn):
            targets = sorted(set(self.indirect_targets.get(pc, []) or self.branch_targets.get(pc, [])))
            call = base == "jsr"
            if targets:
                return ("table-call" if call else "table-jump"), targets
            return ("unresolved-call" if call else "unresolved-jump"), []
        target = _resolve_target(insn, insn.operands[-1]) if insn.operands else None
        targets = [target] if target is not None and 0 <= target < len(self.rom) else []
        if base in CALL_MNEMONICS:
            return "call", targets
        if base in UNCOND_BRANCH_MNEMONICS:
            return "branch", targets
        return "cond", targets

    def _code_refs(self, pc: int, insn) -> list[int]:
        """Code addresses an instruction takes as data (pea/lea/move #imm): callbacks, tasks."""
        base = base_mnemonic(insn)
        if base not in ("pea", "lea", "move", "movea") or not insn.operands:
            return []
        op = insn.operands[0]
        target = None
        if base == "pea" or base == "lea":
            if op.address_mode in (M.M68K_AM_ABSOLUTE_DATA_LONG, M.M68K_AM_ABSOLUTE_DATA_SHORT):
                target = op.imm
            elif op.address_mode == M.M68K_AM_PCI_DISP:
                target = _pc_base(insn) + op.mem.disp
        elif op.type == M.M68K_OP_IMM and access_size(insn) == 4 and op.imm & 0xfff:
            target = op.imm  # 4 KiB multiples (0x4000, 0x5000...) are fixed-point numbers far more often than code
        if target is None or target & 1 or not 0x400 <= target < len(self.rom) or target not in self.code:
            return []
        return [target]

    def _build_routines(self) -> None:
        entries: dict[int, set] = {}

        def add(pc: int, kind: str) -> None:
            if pc in self.code:
                entries.setdefault(pc, set()).add(kind)

        self.vectors: dict[int, int] = {}
        for vector in range(1, 256):
            target = int.from_bytes(self.rom[vector * 4:vector * 4 + 4], "big")
            self.vectors[vector] = target
            if target in self.code and not target & 1 and target >= 0x400:
                add(target, f"vector:{VECTOR_NAMES.get(vector, f'v{vector}')}")
        for ep in self.config.get("discovery", {}).get("entry_points", []):
            add(int(str(ep), 0), "entry")
        for pc in self.seeds_speculative:
            add(pc, "seed")
        self.refs: dict[int, list[int]] = {}
        for pc in self.sorted_pcs:
            insn = self.code[pc]
            kind, targets = self._transfer_targets(pc, insn)
            if kind == "call":
                for t in targets:
                    add(t, "call")
            elif kind == "table-call":
                for t in targets:
                    add(t, "table-call")
            refs = self._code_refs(pc, insn)
            if refs:
                self.refs[pc] = refs
                for t in refs:
                    add(t, "ref")
        self.routines: dict[int, Routine] = {}
        self.owners: dict[int, list[int]] = {}
        for entry in sorted(entries):
            self._claim(entry, entries[entry], entries)
        # Profile-hit block entries that no static edge or fallthrough reaches were entered
        # through f3_dispatch (computed transfer, rte, task switch): routines of uncertain origin.
        if self.hits:
            reached = self._static_successors()
            for pc in sorted(self.hits):
                if pc in self.code and pc not in self.owners and pc not in reached:
                    entries[pc] = {"dispatch?"}
                    self._claim(pc, entries[pc], entries)
        self.entry_list = sorted(self.routines)
        for entry, routine in self.routines.items():
            routine.name = self.symbols_code.get(entry) or f"sub_{entry:06x}"
        self._detect_yield_traps()

    def _detect_yield_traps(self) -> None:
        """Classify trap vector handlers as cooperative task switches (yield).

        A trap vector handler is classified as a task switch when it saves the stack
        pointer (a7/sp) to memory (e.g. move.l a7, d(aN) / move.l sp, ...) and/or loads
        a7 from memory (e.g. movea.l d(aN), a7 / move.l ..., sp), or otherwise swaps the
        return address.

        Exposes:
          self.yield_traps: dict[int trapN, handler entry]
          self.resume_points: dict[int pc_after_trap, int routine entry]
        """
        self.yield_traps: dict[int, int] = {}
        for vector, target in sorted(self.vectors.items()):
            name = VECTOR_NAMES.get(vector, f"v{vector}")
            if not name.startswith("trap") or name == "trapv":
                continue
            r = self.routines.get(target)
            if not r:
                continue
            saves_sp = False
            loads_sp = False
            for pc in r.pcs:
                insn = self.code.get(pc)
                if insn is None:
                    continue
                base = base_mnemonic(insn)
                ops = insn.operands
                if base in ("move", "movea") and len(ops) == 2:
                    # saves a7/sp to memory (excluding stack push -(a7))
                    if ops[0].type == M.M68K_OP_REG and insn.reg_name(ops[0].reg) in ("a7", "sp"):
                        if ops[1].type == M.M68K_OP_MEM:
                            if not (ops[1].address_mode == M.M68K_AM_REGI_ADDR_PRE_DEC and base_register(insn, ops[1]) in ("a7", "sp")):
                                saves_sp = True
                    # loads a7/sp from memory (excluding stack pop (a7)+)
                    if ops[1].type == M.M68K_OP_REG and insn.reg_name(ops[1].reg) in ("a7", "sp"):
                        if ops[0].type == M.M68K_OP_MEM:
                            if not (ops[0].address_mode == M.M68K_AM_REGI_ADDR_POST_INC and base_register(insn, ops[0]) in ("a7", "sp")):
                                loads_sp = True
            if saves_sp and loads_sp:
                trap_num = int(name.removeprefix("trap"))
                self.yield_traps[trap_num] = target

        self.resume_points: dict[int, int] = {}
        for pc in self.sorted_pcs:
            insn = self.code[pc]
            if insn.mnemonic == "trap" and insn.operands:
                num = insn.operands[0].imm & 15
                if num in self.yield_traps:
                    nxt = pc + insn.size
                    routine_entry = self.routine_of(pc)
                    if routine_entry is not None:
                        self.resume_points[nxt] = routine_entry

        for res_pc, routine_entry in self.resume_points.items():
            routine = self.routines.get(routine_entry)
            if routine is not None:
                routine.kinds.add("task")

    def _static_successors(self) -> set[int]:
        reached = set()
        for pc in self.sorted_pcs:
            insn = self.code[pc]
            kind, targets = self._transfer_targets(pc, insn)
            reached.update(targets)
            if kind not in ("terminal", "branch", "table-jump", "unresolved-jump"):
                reached.add(pc + insn.size)
            reached.update(self.refs.get(pc, []))
        return reached

    def _claim(self, entry: int, kinds: set, entries: dict) -> None:
        routine = Routine(entry, set(kinds))
        seen = {entry}
        work = [entry]
        while work:
            pc = work.pop()
            insn = self.code.get(pc)
            if insn is None:
                continue
            routine.pcs.append(pc)
            kind, targets = self._transfer_targets(pc, insn)
            follow = []
            if kind in ("branch", "cond", "table-jump"):
                follow += [t for t in targets if t not in entries or t == entry]
            if kind not in ("terminal", "branch", "table-jump", "unresolved-jump"):
                nxt = pc + insn.size
                if nxt not in entries:
                    follow.append(nxt)
            for t in follow:
                if t not in seen and t in self.code:
                    seen.add(t)
                    work.append(t)
        routine.pcs.sort()
        self.routines[entry] = routine
        for pc in routine.pcs:
            self.owners.setdefault(pc, []).append(entry)

    def routine_of(self, pc: int) -> int | None:
        """Owning routine entry; for code no routine claims, the nearest entry below."""
        owners = self.owners.get(pc)
        if owners:
            return min(owners, key=lambda e: (pc - e) if e <= pc else 1 << 30)
        if pc in self.routines:
            return pc
        i = bisect_right(self.entry_list, pc) - 1
        return self.entry_list[i] if i >= 0 else None

    def label(self, entry: int | None) -> str:
        if entry is None:
            return "?"
        if entry == DISPATCH:
            return "?dispatch"
        routine = self.routines.get(entry)
        return routine.name if routine else (self.symbols_code.get(entry) or f"loc_{entry:06x}")

    def owner_label(self, pc: int) -> str:
        entry = self.routine_of(pc)
        if entry is None:
            return "?"
        claimed = pc in self.owners
        return self.label(entry) + ("" if claimed else "~")

    # ---------------------------------------------------------------- edges
    def _build_edges(self) -> None:
        self.edges: list[Edge] = []
        entries = self.routines
        self.dispatch_hint: dict[int, str] = {}
        for entry, routine in entries.items():
            if "dispatch?" in routine.kinds:
                self.dispatch_hint[entry] = self._dispatch_hint(entry)
        for entry, routine in entries.items():
            for pc in routine.pcs:
                insn = self.code[pc]
                kind, targets = self._transfer_targets(pc, insn)
                if kind == "call":
                    self.edges += [Edge(entry, t, "call", pc) for t in targets if t in entries]
                elif kind == "table-call":
                    self.edges += [Edge(entry, t, "table-call", pc) for t in targets if t in entries]
                elif kind in ("branch", "cond", "table-jump"):
                    edge_kind = "table" if kind == "table-jump" else "tail"
                    self.edges += [Edge(entry, t, edge_kind, pc) for t in targets
                                   if t in entries and t != entry]
                elif kind in ("unresolved-call", "unresolved-jump"):
                    resolved, how = self.register_target(entry, pc)
                    resolved = [t for t in resolved if t in entries]
                    if resolved:
                        edge_kind = "regconst" if how.startswith("register constant") else "regtable"
                        self.edges += [Edge(entry, t, edge_kind, pc) for t in resolved]
                    else:
                        self.edges.append(Edge(entry, DISPATCH, "dispatch", pc))
                        _, candidates = self.slot_targets(entry, pc)
                        self.edges += [Edge(entry, t, "slot", pc) for t in candidates if t in entries]
                if kind not in ("terminal", "branch", "table-jump", "unresolved-jump"):
                    nxt = pc + insn.size
                    if nxt in entries and nxt != entry:
                        self.edges.append(Edge(entry, nxt, "fall", pc))
                for t in self.refs.get(pc, []):
                    if t in entries:
                        self.edges.append(Edge(entry, t, "ref", pc))
        resolved_by_tracker = {e.dst for e in self.edges if e.kind in ("regconst", "regtable")}
        for entry, routine in entries.items():
            if "dispatch?" in routine.kinds and entry in resolved_by_tracker:
                routine.kinds.discard("dispatch?")  # the register tracker found its computed transfer
                routine.kinds.add("regtable")
                self.dispatch_hint.pop(entry, None)
            elif "dispatch?" in routine.kinds and entry in self.resume_points:
                routine.kinds.discard("dispatch?")
                routine.kinds.add("resume")
                self.dispatch_hint.pop(entry, None)
                self.edges.append(Edge(self.resume_points[entry], entry, "resume", -1))
            if "dispatch?" in routine.kinds:
                self.edges.append(Edge(DISPATCH, entry, "dispatch", -1))
        self.callers: dict[int, list[Edge]] = {}
        self.callees: dict[int, list[Edge]] = {}
        for edge in self.edges:
            self.callers.setdefault(edge.dst, []).append(edge)
            self.callees.setdefault(edge.src, []).append(edge)

    def _register_call_targets(self, insn, state: dict) -> list[int]:
        """Targets of jmp/jsr (aN) when aN holds a code constant or an entry of a constant ROM pointer table."""
        op = insn.operands[-1] if insn.operands else None
        if op is None or op.address_mode != M.M68K_AM_REGI_ADDR:
            return []
        value = state.get(base_register(insn, op))
        if value is None:
            return []
        if value.kind == "const":
            return value.constants()
        if value.kind == "table":
            return self.pointer_table(value.value)
        return []

    def register_target(self, entry: int, pc: int) -> tuple[list[int], str]:
        """Targets of a computed jmp/jsr the register tracker can still explain:
        (aN) holding a code constant, or (aN) loaded from a constant ROM pointer table
        (movea.l (a0,d1.w*4),a1 with a0 known). Else ([], description of where the target lives)."""
        insn = self.code[pc]
        op = insn.operands[-1] if insn.operands else None
        if op is None:
            return [], ""
        state = self.states(entry).get(pc, {}) if entry in self.routines else {}
        if op.address_mode == M.M68K_AM_REGI_ADDR:
            reg = base_register(insn, op)
            value = state.get(reg)
            if value is None:
                return [], f"target = {reg}"
            targets = self._register_call_targets(insn, state)
            if value.kind == "const":
                return targets, "register constant"
            if value.kind == "table":
                return targets, (f"ROM pointer table {h(value.value)} loaded at {h(value.set_at)}: {len(targets)} "
                                 "plausible code pointers (bounded by the first non-code long; f3a dis --data longs)")
            if value.kind == "field":
                return [], f"target = long at {value.reg}{value.value:+#x} (loaded at {h(value.set_at)}; " \
                           f"stores to that field: f3a writes --field {value.value:#x})"
            if value.kind == "stream":
                return [], f"target = next long of a data stream ({value.reg})+ (loaded at {h(value.set_at)}): " \
                           f"a script or callback list operand; see the ROM data that names the candidates"
            return [], f"target = {reg} ({value.kind}, set {h(value.set_at)})"
        if op.address_mode in (M.M68K_AM_MEMI_PRE_INDEX, M.M68K_AM_MEMI_POST_INDEX):
            operand = self._operand(insn, op, len(insn.operands) - 1, "addr")
            if operand is not None and operand.reg:
                return [], f"target = long at {operand.reg}{operand.disp:+#x} (a code-pointer slot; who stores " \
                           f"it: f3a xref --field {operand.disp:#x} --role write --code)"
        if op.address_mode in (M.M68K_AM_PCI_INDEX_8_BIT_DISP, M.M68K_AM_PCI_INDEX_BASE_DISP):
            # jmp table(pc,d0.w) with d0 read from one RAM variable: the constants the program stores there.
            operand = self._operand(insn, op, len(insn.operands) - 1, "addr")
            index = operand.index_reg.split(".")[0] if operand is not None and operand.index_reg else ""
            value = state.get(index)
            if value is not None and value.kind == "cell" and not value.step and not value.drift:
                values, complete = self.cell_values(value.value)
                cell = self.describe_address(value.value)
                if values and complete:
                    targets = sorted({operand.address + v * operand.scale for v in values})
                    return targets, (f"index {index} = {cell} (read at {h(value.set_at)}); the program only stores "
                                     f"{', '.join(hex(v) for v in sorted(values))} there")
                return [], f"index {index} = {cell}; non-constant stores to it (f3a xref {h(value.value)})"
        return [], ""

    def cell_values(self, address: int) -> tuple[set[int], bool]:
        """Constants stored to a fixed RAM variable (move #imm / clr to the absolute or a5 address):
        (values as signed numbers, True when every store to it is one of those)."""
        values: set[int] = set()
        complete = True
        for pc in self.static_writers().get(address, []):
            insn = self.code[pc]
            if not any(o.address == address and o.role in ("write", "rmw") for o in self.operands(insn)):
                complete = False  # a wider store covering this byte
                continue
            base = base_mnemonic(insn)
            size = access_size(insn)
            src = insn.operands[0]
            if base == "clr":
                values.add(0)
            elif base == "move" and src.type == M.M68K_OP_IMM:
                raw = src.imm & ((1 << (8 * size)) - 1)
                values.add(raw - (1 << (8 * size)) if raw >> (8 * size - 1) else raw)
            else:
                complete = False
        return values, complete

    def static_writers(self) -> dict[int, list[int]]:
        """Byte address -> PCs that store to it through an absolute or a5 operand (every byte of
        the access). Pointer stores are not included."""
        if "_static_writers" in self.__dict__:
            return self._static_writers
        found: dict[int, list[int]] = {}
        for pc, insn in self.code.items():
            for operand in self.operands(insn):
                if operand.role in ("write", "rmw") and operand.address is not None and operand.mode in ("abs", "a5"):
                    for byte in range(access_size(insn)):
                        found.setdefault(operand.address + byte, []).append(pc)
        self._static_writers = found
        return found

    def pointer_table(self, base: int, limit: int = 256) -> list[int]:
        """Consecutive big-endian longs at a ROM address that plausibly point at code (even, inside
        ROM, decodable). Entries need not be discovered yet; the scan stops at the first implausible
        long, so adjacent tables can run together."""
        targets = []
        for address in range(base, min(len(self.rom) - 3, base + 4 * limit), 4):
            value = int.from_bytes(self.rom[address:address + 4], "big")
            if value & 1 or not 0x400 <= value < len(self.rom) or self.insn_at(value) is None:
                break
            targets.append(value)
        return targets

    def code_stores(self) -> tuple[dict[int, dict[int, list[int]]], dict[int, dict[int, list[int]]]]:
        """Routine addresses the program stores as data, the targets of later computed jmp/jsr:
        (slots, cells). slots = {displacement: {routine: [store pcs]}} for move.l into d(aN)/(aN)+
        of a structure (any base register: slots of different structures with equal displacement
        merge); cells = {RAM address: {routine: [store pcs]}} for fixed RAM / a5 longwords."""
        if "_code_stores" in self.__dict__:
            return self._code_stores
        slots: dict[int, dict[int, list[int]]] = {}
        cells: dict[int, dict[int, list[int]]] = {}
        for entry in self.entry_list:
            states = self.states(entry)
            for pc in self.routines[entry].pcs:
                insn = self.code[pc]
                ops = list(insn.operands)
                if base_mnemonic(insn) not in ("move", "movea") or len(ops) != 2 or access_size(insn) != 4 \
                        or ops[1].type != M.M68K_OP_MEM:
                    continue
                state = states.get(pc, {})
                src = ops[0]
                value = None
                if src.type == M.M68K_OP_IMM:
                    value = src.imm & 0xffffffff
                elif src.type == M.M68K_OP_REG:
                    prior = state.get(insn.reg_name(src.reg))
                    if prior is not None and prior.kind == "const":
                        value = (prior.value + prior.step) & 0xffffffff
                if value not in self.routines:
                    continue
                dest = self._operand(insn, ops[1], 1, "write")
                if dest is None:
                    continue
                if dest.address is not None and dest.mode in ("abs", "a5"):
                    cells.setdefault(dest.address, {}).setdefault(value, []).append(pc)
                elif dest.mode == "areg" and dest.reg not in ("a5", "a7"):
                    holder = state.get(dest.reg)
                    if holder is not None and holder.kind in ("sp", "frame"):
                        continue  # a pushed argument or local, not a structure slot
                    disp = dest.disp + (holder.step if holder is not None and holder.kind == "in" else 0)
                    slots.setdefault(disp, {}).setdefault(value, []).append(pc)
        self._code_stores = (slots, cells)
        return self._code_stores

    def slot_of(self, entry: int, pc: int) -> tuple[str, int] | None:
        """Where an unresolved jmp/jsr takes its target from: ("slot", displacement) for
        jmp ([d,aN]) and jsr (aN) after movea.l d(aM),aN; ("cell", address) after movea.l cell,aN."""
        insn = self.code[pc]
        op = insn.operands[-1] if insn.operands else None
        if op is None:
            return None
        if op.address_mode in (M.M68K_AM_MEMI_PRE_INDEX, M.M68K_AM_MEMI_POST_INDEX):
            operand = self._operand(insn, op, len(insn.operands) - 1, "addr")
            if operand is not None and operand.reg and operand.reg != "a5" and not operand.index_reg:
                return "slot", operand.disp
            if operand is not None and operand.address is not None and not operand.index_reg:
                return "cell", operand.address
            return None
        if op.address_mode == M.M68K_AM_REGI_ADDR:
            value = self.states(entry).get(pc, {}).get(base_register(insn, op))
            if value is not None and value.kind == "field":
                return "slot", value.value + value.step
            if value is not None and value.kind == "ptr":
                return "cell", canonical(value.value)
        return None

    def slot_targets(self, entry: int, pc: int) -> tuple[str, dict[int, list[int]]]:
        """Candidate targets of an unresolved transfer from code_stores(): (description, {routine: store pcs})."""
        where = self.slot_of(entry, pc)
        if where is None:
            return "", {}
        slots, cells = self.code_stores()
        if where[0] == "slot":
            return f"slot {where[1]:+#x}", slots.get(where[1], {})
        return f"cell {self.describe_address(where[1])}", cells.get(where[1], {})

    def rom_pointers(self) -> dict[int, list[int]]:
        """Aligned longs in ROM outside decoded instructions whose value is a routine entry:
        script data, callback lists and pointer tables the static pass does not follow."""
        if "_rom_pointers" in self.__dict__:
            return self._rom_pointers
        covered = bytearray(len(self.rom))
        for pc, insn in self.code.items():
            covered[pc:pc + insn.size] = b"\1" * insn.size
        found: dict[int, list[int]] = {}
        rom = self.rom
        for address in range(0, len(rom) - 3, 2):
            if covered[address] or covered[address + 3]:
                continue
            value = (rom[address] << 24) | (rom[address + 1] << 16) | (rom[address + 2] << 8) | rom[address + 3]
            if value in self.routines:
                found.setdefault(value, []).append(address)
        self._rom_pointers = found
        return found

    def _dispatch_hint(self, entry: int) -> str:
        """Why a profile-hit entry has no static predecessor: code that stores its address into a
        structure slot or RAM cell, ROM data that names it, or the instruction laid out before it."""
        slots, cells = self.code_stores()
        stored = [(f"slot {disp:+#x}", pcs) for disp, targets in slots.items() for t, pcs in targets.items() if t == entry]
        stored += [(f"cell {self.describe_address(a)}", pcs) for a, targets in cells.items()
                   for t, pcs in targets.items() if t == entry]
        if stored:
            return "stored as a code pointer: " + ", ".join(f"{where} at {', '.join(h(p) for p in pcs[:2])}"
                                                           for where, pcs in stored[:3])
        listed = self.rom_pointers().get(entry, [])
        if listed:
            more = f" +{len(listed) - 3}" if len(listed) > 3 else ""
            return f"its address is a long in ROM data at {', '.join(h(a) for a in listed[:3])}{more} " \
                   "(script, callback list or pointer table)"
        for back in (2, 4, 6, 8, 10):
            prev = self.code.get(entry - back)
            if prev is not None and entry - back + prev.size == entry:
                base = base_mnemonic(prev)
                if base in ("jmp", "bra") and prev.operands and not _computed_transfer(prev):
                    target = _resolve_target(prev, prev.operands[-1])
                    return f"laid out after a tail jump ({base} {h(target)} at {h(prev.address)}); entered by computed transfer" \
                        if target is not None else f"laid out after {base} at {h(prev.address)}"
                if base in ("rts", "rte", "rtr"):
                    return f"laid out after {base} at {h(prev.address)}; entered by computed transfer"
                return f"laid out after {prev.mnemonic} at {h(prev.address)}; entered by computed transfer"
        return "no decoded predecessor; entered by computed transfer"

    # ---------------------------------------------------------------- operands
    def operands(self, insn) -> list[Operand]:
        """Memory operands of an instruction with role and static address."""
        base = base_mnemonic(insn)
        try:
            ops = list(insn.operands)
        except Exception:  # Capstone SKIPDATA records have no operand detail
            return []
        if not ops:
            return []
        roles = ["read"] * len(ops)
        if base in ADDRESS_ONLY:
            roles = ["addr"] * len(ops)
        elif base == "movem":
            roles = ["read", "write"] if ops[0].type == M.M68K_OP_REG_BITS else ["read", "read"]
        elif base not in READ_ONLY:
            roles[-1] = "write" if base in WRITE_ONLY else "rmw"
        out = []
        for index, op in enumerate(ops):
            if op.type != M.M68K_OP_MEM:
                continue
            resolved = self._operand(insn, op, index, roles[index])
            if resolved is not None:
                out.append(resolved)
        return out

    def _operand(self, insn, op, index: int, role: str) -> Operand | None:
        mode = op.address_mode
        mem = op.mem
        reg = base_register(insn, op)
        size = access_size(insn)
        if mode in (M.M68K_AM_ABSOLUTE_DATA_LONG, M.M68K_AM_ABSOLUTE_DATA_SHORT):
            value = op.imm & 0xffffffff
            if mode == M.M68K_AM_ABSOLUTE_DATA_SHORT and value & 0x8000:
                value |= 0xffff0000
            return Operand(index, role, "abs", canonical(value))
        if mode == M.M68K_AM_PCI_DISP:
            return Operand(index, role, "pc", canonical(_pc_base(insn) + mem.disp))
        if mode in (M.M68K_AM_REGI_ADDR, M.M68K_AM_REGI_ADDR_POST_INC, M.M68K_AM_REGI_ADDR_PRE_DEC,
                    M.M68K_AM_REGI_ADDR_DISP):
            disp = mem.disp if mode == M.M68K_AM_REGI_ADDR_DISP else 0
            step = size if mode == M.M68K_AM_REGI_ADDR_POST_INC else -size if mode == M.M68K_AM_REGI_ADDR_PRE_DEC else 0
            if reg == "a5" and self.a5 is not None:
                return Operand(index, role, "a5", canonical(self.a5 + disp), reg, disp, postinc=step)
            return Operand(index, role, "areg", None, reg, disp, postinc=step)
        if mode in (M.M68K_AM_AREGI_INDEX_8_BIT_DISP, M.M68K_AM_AREGI_INDEX_BASE_DISP,
                    M.M68K_AM_MEMI_POST_INDEX, M.M68K_AM_MEMI_PRE_INDEX, M.M68K_AM_PCI_INDEX_8_BIT_DISP,
                    M.M68K_AM_PCI_INDEX_BASE_DISP, M.M68K_AM_PC_MEMI_POST_INDEX, M.M68K_AM_PC_MEMI_PRE_INDEX):
            return self._extension_operand(insn, op, index, role)
        return None

    def _extension_operand(self, insn, op, index: int, role: str) -> Operand | None:
        """Decode the brief/full extension word from raw bytes: Capstone drops index scales from
        op_str and reports full-format word displacements unsigned."""
        raw = bytes(insn.bytes)
        offset = _pc_base(insn) - insn.address
        opcode = int.from_bytes(raw[:2], "big")
        if base_mnemonic(insn) == "move" and index == 1:
            offset += _ea_extension_bytes(raw, offset, (opcode >> 3) & 7, opcode & 7, access_size(insn))
        if offset + 2 > len(raw):
            return None
        ext = int.from_bytes(raw[offset:offset + 2], "big")
        index_reg = f"{'a' if ext & 0x8000 else 'd'}{(ext >> 12) & 7}.{'l' if ext & 0x800 else 'w'}"
        scale = 1 << ((ext >> 9) & 3)
        pc_based = op.mem.base_reg == M.M68K_REG_PC or op.address_mode in (
            M.M68K_AM_PCI_INDEX_8_BIT_DISP, M.M68K_AM_PCI_INDEX_BASE_DISP,
            M.M68K_AM_PC_MEMI_POST_INDEX, M.M68K_AM_PC_MEMI_PRE_INDEX)
        reg = "" if pc_based else (insn.reg_name(op.mem.base_reg) if op.mem.base_reg else "")
        indirect = False
        if not ext & 0x100:
            disp = int.from_bytes(raw[offset + 1:offset + 2], "big", signed=True)
        else:
            bd_size = {1: 0, 2: 2, 3: 4}.get((ext >> 4) & 3, 0)
            disp = int.from_bytes(raw[offset + 2:offset + 2 + bd_size], "big", signed=True) if bd_size else 0
            indirect = bool(ext & 3)
            if ext & 0x80:      # base suppressed
                pc_based, reg = False, ""
            if ext & 0x40:      # index suppressed
                index_reg, scale = "", 1
        if not index_reg and not indirect:
            # (bd,An,invalid) with the index suppressed is plain displacement addressing.
            if pc_based:
                return Operand(index, role, "pc", canonical(_pc_base(insn) + disp))
            if reg == "a5" and self.a5 is not None:
                return Operand(index, role, "a5", canonical(self.a5 + disp), reg, disp)
            if reg:
                return Operand(index, role, "areg", None, reg, disp)
        if pc_based:
            return Operand(index, role, "pc-idx" if not indirect else "memi", canonical(_pc_base(insn) + disp),
                           "pc", disp, index_reg, scale, indirect)
        if reg == "a5" and self.a5 is not None:
            return Operand(index, role, "areg-idx" if not indirect else "memi", canonical(self.a5 + disp),
                           reg, disp, index_reg, scale, indirect)
        if not reg:
            return Operand(index, role, "abs-idx" if not indirect else "memi", canonical(disp), "", disp,
                           index_reg, scale, indirect)
        return Operand(index, role, "areg-idx" if not indirect else "memi", None, reg, disp, index_reg, scale, indirect)

    # ---------------------------------------------------------------- register tracking
    @dataclass
    class Value:
        kind: str          # const | ptr (loaded from RAM cell `value`) | sp / frame (offset from the
                           # stack pointer at routine entry) | arg (loaded from the caller's stack slot
                           # at entry_sp + value) | in (the caller's value of register `reg`) | field
                           # (long at reg+value) | table (entry of constant table `value`) | stream
                           # (long read through (reg)+: script/callback list operand) | cell (data
                           # register loaded with the byte/word RAM variable at `value`)
        value: int
        set_at: int
        step: int = 0      # accumulated (An)+ / addq / adda adjustments seen after set_at
        reg: str = ""      # for kind "in"
        drift: bool = False  # also advanced by a non-constant amount (adda.w dN,aN)
        alts: tuple = ()   # const: other constants set on alternative branches (lea A,a4 / bra / lea B,a4)

        def constants(self) -> list[int]:
            """Every value a const may hold: this one plus the branch alternatives, each stepped."""
            return [(v + self.step) & 0xffffffff for v in (self.value,) + self.alts]

    def track_registers(self, entry: int, stop: int | None = None, observe=None) -> dict:
        """Forward pass over a routine in address order tracking registers that hold constants
        (lea/move #imm, ROM pointers), pointers loaded from fixed RAM, the stack pointer / link
        frame, and pointers loaded from stack arguments. Approximate: joins are not merged and
        calls clobber a0-a4, d0-d7. observe(pc, insn, state) runs before each step."""
        state: dict[str, Game.Value] = {"a7": Game.Value("sp", 0, entry)}
        for reg in ("a0", "a1", "a2", "a3", "a4", "a6"):  # whatever the caller left in the register
            state[reg] = Game.Value("in", 0, entry, reg=reg)
        if self.a5 is not None:
            state["a5"] = Game.Value("const", self.a5, -1)
        for pc in self.routines[entry].pcs if entry in self.routines else []:
            if stop is not None and pc >= stop:
                break
            insn = self.code[pc]
            if observe:
                observe(pc, insn, state)
            self._step_registers(pc, insn, state)
        return state

    def states(self, entry: int) -> dict[int, dict]:
        """Register state before every instruction of a routine (cached)."""
        cache = self.__dict__.setdefault("_state_cache", {})
        if entry not in cache:
            snapshots: dict[int, dict] = {}
            self.track_registers(entry, observe=lambda pc, insn, state: snapshots.__setitem__(
                pc, {k: dataclasses.replace(v) for k, v in state.items()}))
            cache[entry] = snapshots
        return cache[entry]

    def _step_registers(self, pc: int, insn, state: dict) -> None:
        base = base_mnemonic(insn)
        ops = list(insn.operands)
        sp = state.get("a7")
        for op in ops:  # (An)+ / -(An) side effects
            if op.type == M.M68K_OP_MEM and op.address_mode in (M.M68K_AM_REGI_ADDR_POST_INC,
                                                                 M.M68K_AM_REGI_ADDR_PRE_DEC):
                reg = base_register(insn, op)
                if reg in state:
                    size = access_size(insn)
                    if base == "movem":
                        size *= sum(bin(o.register_bits).count("1") for o in ops if o.type == M.M68K_OP_REG_BITS) or 1
                    delta = size if op.address_mode == M.M68K_AM_REGI_ADDR_POST_INC else -size
                    if state[reg].kind == "sp":
                        state[reg].value += delta
                    else:
                        state[reg].step += delta
        if base == "link" and len(ops) == 2 and sp is not None and sp.kind == "sp":
            state["a6"] = Game.Value("frame", sp.value - 4, pc)
            sp.value -= 4 - (ops[1].imm if ops[1].imm < 0x8000 else ops[1].imm - 0x10000)
            return
        if base == "unlk":
            state.pop("a6", None)
            return
        if base in ("jsr", "bsr", "trap"):
            if base == "trap":
                handler = self.vectors.get(32 + (ops[0].imm & 15)) if ops else None
                targets = [handler] if handler in self.routines else []
            else:
                kind, targets = self._transfer_targets(pc, insn)
                if kind == "unresolved-call":
                    targets = self._register_call_targets(insn, state)
            changed, replaced = self.call_effect(targets)
            for reg in changed:
                value = state.get(reg)
                if reg in replaced or value is None or value.kind in ("sp", "frame"):
                    state.pop(reg, None)
                else:
                    value.drift = True  # the callee only moved it within the same buffer
            return
        if not ops:
            return
        dest = ops[-1]
        if base == "movem" and len(ops) == 2 and sp is not None and sp.kind == "sp":
            # Register saves to the stack and their restores keep the tracked values ("@<sp offset>" slots).
            size = access_size(insn)
            if ops[0].type == M.M68K_OP_REG_BITS and ops[1].type == M.M68K_OP_MEM \
                    and base_register(insn, ops[1]) == "a7":
                for i, reg in enumerate(ordered_registers(ops[0].register_bits)):  # sp already decremented
                    slot = f"@{sp.value + size * i}"
                    if reg in state and reg != "a7":
                        state[slot] = dataclasses.replace(state[reg])
                    else:
                        state.pop(slot, None)
                return
            if dest.type == M.M68K_OP_REG_BITS and ops[0].type == M.M68K_OP_MEM \
                    and base_register(insn, ops[0]) == "a7" \
                    and ops[0].address_mode == M.M68K_AM_REGI_ADDR_POST_INC:
                registers = ordered_registers(dest.register_bits)
                start = sp.value - size * len(registers)  # sp already incremented
                for i, reg in enumerate(registers):
                    saved = state.get(f"@{start + size * i}")
                    if saved is not None and size == 4:
                        state[reg] = dataclasses.replace(saved)
                    else:
                        state.pop(reg, None)
                return
        if dest.type == M.M68K_OP_REG_BITS:
            if base == "movem":  # restoring registers from memory
                for reg in register_list(dest.register_bits):
                    state.pop(reg, None)
            return
        if dest.type != M.M68K_OP_REG:
            return
        reg = insn.reg_name(dest.reg)
        if not reg or reg[0] not in "ad":
            return
        src = ops[0] if len(ops) == 2 else None
        new = None
        if base in ("adda", "addq", "suba", "subq", "addi", "subi", "add", "sub") and src is not None \
                and src.type == M.M68K_OP_IMM and reg in state:
            amount = src.imm if base.startswith("add") else -src.imm
            if state[reg].kind == "sp":
                state[reg].value += amount
            else:
                state[reg].step += amount
            return
        if base in ("adda", "suba") and reg.startswith("a") and reg in state and state[reg].kind not in ("sp", "frame"):
            state[reg].drift = True  # same buffer, offset unknown statically
            return
        if base == "lea" and src is not None:
            operand = self._operand(insn, src, 0, "addr")
            if operand is not None and operand.address is not None and operand.mode in ("abs", "pc", "a5"):
                new = Game.Value("const", self._raw_address(insn, src, operand), pc)
            elif operand is not None and operand.mode == "areg" and operand.reg in state:
                prior = state[operand.reg]
                if operand.reg == reg:
                    if prior.kind == "sp":
                        prior.value += operand.disp
                    else:
                        prior.step += operand.disp
                    return
                if prior.kind in ("const", "sp", "frame"):
                    new = Game.Value(prior.kind, prior.value + prior.step + operand.disp, pc,
                                     alts=tuple(a + prior.step + operand.disp for a in prior.alts))
                else:  # lea d(a0),a1 from a caller/argument/field pointer: same base, moved by d
                    new = dataclasses.replace(prior, step=prior.step + operand.disp, set_at=pc)
        elif base in ("movea", "move", "moveq") and src is not None:
            if src.type == M.M68K_OP_IMM:
                new = Game.Value("const", src.imm & 0xffffffff, pc)
            elif src.type == M.M68K_OP_REG and insn.reg_name(src.reg) in state:
                prior = state[insn.reg_name(src.reg)]
                new = dataclasses.replace(prior, set_at=pc)
            elif src.type == M.M68K_OP_MEM and access_size(insn) in (1, 2) and reg.startswith("d"):
                operand = self._operand(insn, src, 0, "read")
                if operand is not None and operand.address is not None and operand.mode in ("abs", "a5") \
                        and operand.address >= len(self.rom):
                    new = Game.Value("cell", operand.address, pc)  # move.w var,d0: value of a RAM variable
            elif src.type == M.M68K_OP_MEM and access_size(insn) == 4:
                operand = self._operand(insn, src, 0, "read")
                if operand is not None and operand.address is not None and operand.mode in ("abs", "pc", "a5"):
                    if operand.address + 4 <= len(self.rom):  # pointer stored in ROM: read it
                        new = Game.Value("const", int.from_bytes(self.rom[operand.address:operand.address + 4], "big"), pc)
                    else:
                        new = Game.Value("ptr", operand.address, pc)
                elif operand is not None and operand.mode == "areg" and operand.reg in state \
                        and state[operand.reg].kind in ("sp", "frame"):
                    holder = state[operand.reg]
                    slot = holder.value + holder.step + operand.disp
                    if slot >= 4:  # above the return address: a caller-pushed argument
                        new = Game.Value("arg", slot, pc)
                elif operand is not None and operand.mode == "areg-idx" and operand.reg in state \
                        and state[operand.reg].kind == "const":
                    holder = state[operand.reg]  # movea.l (a0,d1.w*4),a1 (or a pre-scaled index): constant table
                    new = Game.Value("table", holder.value + holder.step + operand.disp, pc)
                elif operand is not None and operand.mode == "areg" and operand.reg and operand.postinc:
                    new = Game.Value("stream", 0, pc, reg=operand.reg)  # movea.l (a3)+,a0: next long of a data stream
                elif operand is not None and operand.mode == "areg" and operand.reg:
                    new = Game.Value("field", operand.disp, pc, reg=operand.reg)  # e.g. movea.l (a4),a6
        if new is None:
            state.pop(reg, None)
            return
        prior = state.get(reg)
        if new.kind == "const" and prior is not None and prior.kind == "const" and prior.set_at >= 0 \
                and prior.set_at < pc and self._jumps_between(prior.set_at, pc):
            # lea A,a4 / bra join / lea B,a4: the second assignment is the other branch, not a replacement.
            new.alts = tuple(dict.fromkeys(prior.alts + ((prior.value + prior.step) & 0xffffffff,)))
        state[reg] = new

    def _jumps_between(self, start: int, end: int) -> bool:
        """An unconditional forward bra/jmp between two PCs that skips past `end`: the if/else arms
        `lea A,a4 / bra join / lea B,a4 / join:`."""
        pc = self.code[start].address + self.code[start].size if start in self.code else end
        while pc < end:
            insn = self.code.get(pc)
            if insn is None:
                return False
            if base_mnemonic(insn) in ("bra", "jmp"):
                kind, targets = self._transfer_targets(pc, insn)
                return bool(targets) and all(t > end for t in targets)
            pc += insn.size
        return False

    def clobbers(self, entry: int) -> tuple[frozenset, frozenset]:
        """What a call to this routine does to the caller's scratch registers: (changed, replaced).
        changed = registers it writes (directly, through callees or by falling into the next routine)
        minus the ones it saves with movem to -(a7); replaced ⊆ changed = registers given a new value,
        as opposed to address registers only adjusted relative to themselves ((aN)+, lea d(aN,dX),aN,
        adda/addq/subq): those still point into the caller's buffer. Unknown callees replace everything."""
        cache = self.__dict__.setdefault("_clobber_cache", {})
        if entry in cache:
            return cache[entry]
        worst = (frozenset(ALL_SCRATCH), frozenset(ALL_SCRATCH))
        if entry not in self.routines:
            return worst
        cache[entry] = worst  # recursion guard: assume the worst inside cycles
        written: set[str] = set()
        replaced: set[str] = set()
        saved: set[str] = set()

        def merge(effect: tuple[frozenset, frozenset]) -> None:
            written.update(effect[0])
            replaced.update(effect[1])

        for pc in self.routines[entry].pcs:
            insn = self.code[pc]
            base = base_mnemonic(insn)
            ops = list(insn.operands)
            kind, targets = self._transfer_targets(pc, insn) if base != "trap" else ("trap", [])
            if kind not in ("terminal", "branch", "table-jump", "unresolved-jump") and pc + insn.size in self.routines \
                    and pc + insn.size != entry:
                merge(self.clobbers(pc + insn.size))  # falls into the next routine
            if kind in ("call", "branch", "cond", "table-call", "table-jump"):
                for target in targets:
                    if kind in ("call", "table-call") or (target in self.routines and target != entry):
                        merge(self.clobbers(target))
            if kind == "trap":
                handler = self.vectors.get(32 + (ops[0].imm & 15)) if ops else None
                merge(self.clobbers(handler) if handler in self.routines else worst)
            elif kind in ("unresolved-call", "unresolved-jump"):
                # Uncached forward pass: the cached states(entry) would freeze this cycle guard's worst case.
                merge(self.call_effect(self._register_call_targets(insn, self.track_registers(entry, stop=pc))))
            elif kind in ("table-call", "table-jump") and not targets:
                merge(worst)
            if base in ("jsr", "bsr", "jmp", "bra", "trap"):
                continue
            for op in ops:
                if op.type == M.M68K_OP_MEM and op.address_mode in (M.M68K_AM_REGI_ADDR_POST_INC,
                                                                     M.M68K_AM_REGI_ADDR_PRE_DEC):
                    written.add(base_register(insn, op))
            if base == "movem" and len(ops) == 2:
                if ops[0].type == M.M68K_OP_REG_BITS and ops[1].type == M.M68K_OP_MEM \
                        and base_register(insn, ops[1]) == "a7":
                    saved |= register_list(ops[0].register_bits)
                elif ops[1].type == M.M68K_OP_REG_BITS:
                    written |= register_list(ops[1].register_bits)
                    replaced |= register_list(ops[1].register_bits)
                continue
            if base == "exg":
                names = {insn.reg_name(op.reg) for op in ops if op.type == M.M68K_OP_REG}
                written |= names
                replaced |= names
            elif ops and ops[-1].type == M.M68K_OP_REG and base not in READ_ONLY:
                reg = insn.reg_name(ops[-1].reg)
                written.add(reg)
                if not self._self_relative(insn, base, ops, reg):
                    replaced.add(reg)
        scratch = set(ALL_SCRATCH)
        result = (frozenset((written - saved) & scratch), frozenset((replaced - saved) & scratch))
        cache[entry] = result
        return result

    def call_effect(self, targets: list[int]) -> tuple[frozenset, frozenset]:
        """clobbers() of a call with these possible targets. Targets that are not discovered routines
        (never reached nor executed, e.g. the tail of a ROM pointer table) are ignored when others are."""
        known = [t for t in targets if t in self.routines]
        if not known:
            return frozenset(ALL_SCRATCH), frozenset(ALL_SCRATCH)
        effects = [self.clobbers(t) for t in known]
        return frozenset().union(*(e[0] for e in effects)), frozenset().union(*(e[1] for e in effects))

    def _self_relative(self, insn, base: str, ops: list, reg: str) -> bool:
        """An address register adjusted relative to itself: adda/suba/addq/subq/addi/subi/lea d(aN[,dX]),aN."""
        if not reg.startswith("a"):
            return False
        if base in ("adda", "suba", "addq", "subq", "addi", "subi", "add", "sub"):
            return True
        if base == "lea" and ops[0].type == M.M68K_OP_MEM:
            return base_register(insn, ops[0]) == reg
        return False

    def call_args(self, caller: int, site: int) -> dict[int, "Game.Value | None"]:
        """Stack arguments pushed right before a call: {offset from callee entry sp: value or None}."""
        routine = self.routines.get(caller)
        if routine is None or site not in routine.pcs:
            return {}
        states = self.states(caller)
        pcs = routine.pcs
        index = pcs.index(site)
        args: dict[int, int | None] = {}
        offset = 4
        expected = site
        for pc in reversed(pcs[:index]):
            insn = self.code[pc]
            if pc + insn.size != expected:
                break
            expected = pc
            base = base_mnemonic(insn)
            ops = list(insn.operands)
            value: int | None = None
            if base == "pea" and ops:
                operand = self._operand(insn, ops[0], 0, "addr")
                if operand is not None and operand.mode in ("abs", "pc", "a5") and operand.address is not None:
                    value = Game.Value("const", self._raw_address(insn, ops[0], operand), pc)
                elif operand is not None and operand.mode == "areg":
                    prior = states.get(pc, {}).get(operand.reg)
                    if prior is not None and prior.kind not in ("sp", "frame"):
                        value = dataclasses.replace(prior, step=prior.step + operand.disp)
                size = 4
            elif base == "move" and len(ops) == 2 and ops[1].type == M.M68K_OP_MEM \
                    and ops[1].address_mode == M.M68K_AM_REGI_ADDR_PRE_DEC and base_register(insn, ops[1]) == "a7":
                size = access_size(insn)
                src = ops[0]
                if src.type == M.M68K_OP_IMM:
                    value = Game.Value("const", src.imm & ((1 << (8 * size)) - 1), pc)
                elif src.type == M.M68K_OP_REG:
                    prior = states.get(pc, {}).get(insn.reg_name(src.reg))
                    if prior is not None and prior.kind not in ("sp", "frame"):
                        value = dataclasses.replace(prior)
            else:
                break
            args[offset] = value
            offset += size
        return args

    def bases(self, entry: int, value: "Game.Value", depth: int = 4) -> tuple[list[int], int, int]:
        """Possible base addresses of a register value, following stack arguments and incoming
        registers back through every static call site and RAM pointer cells through their constant
        stores. Returns (addresses, call paths that resolved, call paths tried)."""
        cache = self.__dict__.setdefault("_bases_cache", {})
        key = (entry, value.kind, value.value, value.reg, value.step, depth)
        if key in cache:
            return cache[key]
        cache[key] = ([], 0, 0)  # cycle guard
        if value.kind == "const":
            result = (value.constants(), 1, 1)
        elif value.kind == "ptr":
            cells = sorted(self.pointer_cells().get(canonical(value.value), ()))
            result = (cells, 1 if cells else 0, 1)
        elif value.kind in ("arg", "in") and depth > 0:
            found: set[int] = set()
            resolved = tried = 0
            for edge in self.callers.get(entry, []):
                if edge.site < 0 or edge.kind not in ("call", "table-call", "regconst", "regtable", "slot", "tail", "fall"):
                    continue
                tried += 1
                if value.kind == "arg":
                    incoming = self.call_args(edge.src, edge.site).get(value.value)
                else:
                    incoming = self.states(edge.src).get(edge.site, {}).get(value.reg)
                if incoming is None or incoming.kind in ("sp", "frame"):
                    continue
                addresses, _, _ = self.bases(edge.src, incoming, depth - 1)
                if addresses:
                    resolved += 1
                    found.update(a + value.step for a in addresses)
            result = (sorted(found), resolved, tried)
        else:
            result = ([], 0, 0)
        cache[key] = result
        return result

    def pointer_cells(self) -> dict[int, set[int]]:
        """Constants the program stores into fixed RAM longwords (move.l #imm / a constant register
        to an absolute or a5 address): the possible targets of pointers later loaded from them."""
        if "_pointer_cells" in self.__dict__:
            return self._pointer_cells
        cells: dict[int, set[int]] = {}
        for entry in self.entry_list:
            states = self.states(entry)
            for pc in self.routines[entry].pcs:
                insn = self.code[pc]
                ops = list(insn.operands)
                if base_mnemonic(insn) not in ("move", "movea") or len(ops) != 2 or access_size(insn) != 4:
                    continue
                dest = self._operand(insn, ops[1], 1, "write") if ops[1].type == M.M68K_OP_MEM else None
                if dest is None or dest.address is None or dest.mode not in ("abs", "a5"):
                    continue
                src = ops[0]
                value = None
                if src.type == M.M68K_OP_IMM:
                    value = src.imm & 0xffffffff
                elif src.type == M.M68K_OP_REG:
                    prior = states.get(pc, {}).get(insn.reg_name(src.reg))
                    if prior is not None and prior.kind == "const":
                        value = (prior.value + prior.step) & 0xffffffff
                if value is not None and region_of(canonical(value)) not in ("rom", "unmapped"):
                    cells.setdefault(dest.address, set()).add(value)
        self._pointer_cells = cells
        return cells

    def _raw_address(self, insn, op, operand: Operand) -> int:
        """lea of an a5/pc/abs operand: keep the bus address (lea does not fold RAM mirrors)."""
        if operand.mode == "a5":
            return (self.a5 + operand.disp) & 0xffffffff
        if operand.mode == "abs":
            return op.imm & 0xffffffff
        return operand.address

    # ---------------------------------------------------------------- formatting helpers
    def data_name(self, address: int) -> str:
        address = canonical(address)
        for start, (name, size) in self.symbols_data.items():
            if start <= address < start + size:
                return name if start == address else f"{name}+{address - start}"
        return ""

    def describe_address(self, address: int) -> str:
        name = self.data_name(address)
        region = region_of(address)
        if region == "rom":
            region = "rom code" if address in self.code else "rom data"
        text = f"{h(address)} {region}"
        if region == "ram" and self.a5 is not None:
            offset = address - self.a5
            if -0x8000 <= offset < 0x8000:
                text += f" (a5{'-' if offset < 0 else '+'}${abs(offset):x})"
        return text + (f" [{name}]" if name else "")
