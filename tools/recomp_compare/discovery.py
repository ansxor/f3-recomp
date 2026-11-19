"""Explain T9 decisions using the ROM, payloads and the compared decoder rows."""

import re

from capstone import CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_020, Cs, m68k

from tools.recomp_compare.decoder import category, checked_extension, matches, word_at
from tools.recomp_compare.decisions import pc_text
from tools.recomp_py.discovery import _scan_computed_targets, parse_exclusions
from tools.recomp_py.opcodes import LEGAL_68020
from tools.recomp_py.ram_code import decode_ram_routines

CHAIN_LIMIT = 0x200000
PC_MASK = 0xFFFFFF
PC_TEXT = re.compile(r"0x[0-9a-f]+")
DECODED = ("native", "fallback")


def eligibility(records, size):
    entries = bytearray(size)
    for (pc, _), row in records:
        if pc < len(entries) and row[1] in DECODED:
            entries[pc] = 1
    return entries


def eligible(entries, target):
    return 0 <= target < min(CHAIN_LIMIT, len(entries)) and entries[target]


def addresses(text):
    return {int(match[0], 16) for match in PC_TEXT.finditer(text)}


def physical_flow(flow):
    return PC_TEXT.sub(lambda match: pc_text(int(match[0], 16) & PC_MASK), flow)


def static_candidates(row, entries):
    pc, disposition, length, flow = row[:4]
    if disposition not in ("native", "ram") or length == "-":
        return set()
    targets = addresses(flow)
    if flow == "next" or "next" in flow.removeprefix("branch:").split(","):
        targets.add(int(pc, 16) + int(length))
    return {target for target in targets if eligible(entries, target)}


class DiscoveryRules:
    def __init__(self, rom, config, before, after):
        self.rom = rom
        self.ram = decode_ram_routines(rom, config)
        self.exclusions = parse_exclusions(config, len(rom), image=rom)
        self.capstone = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        self.capstone.detail = True
        self.before = eligibility(before, len(rom))
        self.after = eligibility(after, len(rom))

    def window(self, pc, length=12):
        for routine in self.ram:
            offset = pc - routine.destination
            if 0 <= offset < len(routine.payload):
                return routine.payload[offset : offset + length]
        return self.rom[pc : pc + length]

    def category(self, pc, column, before, after, old, new):
        data = self.window(pc)
        if len(data) < 2:
            return None
        opcode = word_at(data, 0)
        if column == "entry" and old is None:
            for routine in self.ram:
                if (routine.destination <= pc < routine.destination + len(routine.payload)
                        and pc % 2 == 0 and pc not in routine.instructions
                        and (new[1] == "ram" or new[1].startswith("ram-exception:"))):
                    return "ram-all-aligned"
        if column == "disposition":
            if before == "fallback" and after == "native":
                return "lowering-fallback"
            if before in ("native", "ram"):
                prefix = "ram-exception" if before == "ram" else "exception"
                if opcode >> 12 in (10, 15) and after == f"{prefix}:{10 + (opcode >> 12 == 15)}":
                    return "line-exception"
                if (opcode == 0x4AFC or matches(opcode, 0xFFF8, 0x4848)) and after == f"{prefix}:4":
                    return "illegal-bkpt"
                if after == f"{prefix}:4" and category(data, 0, "length", old[2], new[2]) == "movec-control":
                    return "movec-control"
        if column == "flow" and before != after and physical_flow(before) == after:
            return "physical-transfer"
        if column == "chain_candidates":
            return self.chain_category(pc, before, after, old, new)
        return None

    def chain_category(self, pc, before, after, old, new):
        previous, current = addresses(before), addresses(after)
        canonical = ",".join(pc_text(target) for target in sorted(current)) or "-"
        if previous == current or canonical != after:
            return None
        old_static = static_candidates(old, self.before)
        new_static = static_candidates(new, self.after)
        if not new_static <= current:
            return None
        removed, added = previous - current, current - previous
        if (removed <= old_static - new_static and added <= new_static - old_static):
            if old[1] == "fallback" and new[1] == "native":
                return "chain-lowering-fallback"
            if old[2] != new[2] or old[3] != new[3]:
                rule = category(self.rom, pc, "length", old[2], new[2])
                rule = rule or category(self.rom, pc, "flow", old[3], new[3])
                if rule or (old[3] != new[3] and physical_flow(old[3]) == new[3]):
                    return "chain-source-decode"
            if old[2:4] == new[2:4]:
                return "chain-target-decode"
        if old[3] == new[3] == "escape" and new[1] == "native":
            return self.indirect_category(pc, previous, current, old[1])
        return None

    def indirect_category(self, pc, previous, current, disposition):
        data = self.window(pc, 24)
        opcode, extension = word_at(data, 0), word_at(data, 2)
        if not matches(opcode, 0xFFBF, 0x4EBB):
            return None
        insn = next(self.capstone.disasm(data, pc, count=1), None)
        if insn is None or not insn.operands:
            return None
        operand = insn.operands[-1]
        old_scan = _scan_computed_targets(self.rom, self.capstone, insn, operand)
        old_targets = set(sorted(t for t in old_scan if eligible(self.before, t))[:256])
        if disposition == "fallback":
            old_targets.clear()
        if previous != old_targets:
            return None

        full = bool(extension & 0x100)
        width = (0, 0, 2, 4)[extension >> 4 & 3] if full else 1
        start = 4 if full else 3
        displacement = int.from_bytes(data[start : start + width], "big", signed=True)
        table = ((0 if full and extension & 0x80 else pc + 2) + displacement) & PC_MASK
        selector = extension & 7 if full else 0
        if selector == 0 and not (full and extension & 0x80):
            scanned = self.relative_words(table)
            targets = {t for t in scanned if eligible(self.after, t)}
            if current != targets:
                return None
            if set(old_scan) == scanned:
                return "chain-table-target-decode"
            if full and not old_scan and operand.address_mode == m68k.M68K_AM_AREGI_INDEX_BASE_DISP:
                return "chain-pc-index-mode"
        elif selector in (1, 2, 3) and not extension & 0x40:
            outer_width = (0, 0, 2, 4)[selector]
            outer_start = start + width
            outer = int.from_bytes(data[outer_start : outer_start + outer_width], "big", signed=True)
            scanned, stop = self.absolute_pointers(table, outer)
            targets = {t for t in scanned if eligible(self.after, t)}
            if current != targets:
                return None
            if table < 0x400:
                return "chain-pointer-vector-header"
            if set(old_scan) == scanned:
                return "chain-lowering-fallback" if disposition == "fallback" else "chain-table-target-decode"
            if stop in old_scan:
                if any(ex.start <= stop < ex.end for ex in self.exclusions):
                    return "chain-pointer-exclusion"
                stop_opcode = word_at(self.window(stop), 0)
                if stop_opcode >> 12 in (10, 15):
                    return "chain-pointer-line-exception"
            if scanned - set(old_scan):
                # The first added pointer is where Capstone stopped.
                for slot in range(table, table + 64 * 4, 4):
                    target = (int.from_bytes(self.rom[slot : slot + 4], "big") + outer) & 0xFFFFFFFF
                    if target in scanned - set(old_scan):
                        target_data = self.window(target)
                        target_opcode = word_at(target_data, 0)
                        if LEGAL_68020[target_opcode] and checked_extension(target_opcode, target_data):
                            return "chain-pointer-extension"
                        break
        return None

    def relative_words(self, table):
        if not 0 <= table <= len(self.rom) - 2:
            return set()
        offsets = [
            int.from_bytes(self.rom[slot : slot + 2], "big", signed=True)
            for slot in range(table, min(len(self.rom), table + 256), 2)
        ]
        first = offsets[0]
        if not 0 < first <= 0x7000:
            return set()
        offsets = offsets[:first // 2]
        limit = min((off for off in offsets if off > 0), default=0) // 2
        return {
            target for off in offsets[:limit]
            if off != -1 and 0x400 <= (target := (table + off) & PC_MASK) < len(self.rom) and target % 2 == 0
        }

    def absolute_pointers(self, table, outer):
        if table < 0x400:
            return set(), None
        stop = len(self.rom)
        scanned = set()
        for slot in range(table, table + 64 * 4, 4):
            if slot + 4 > stop:
                break
            target = (int.from_bytes(self.rom[slot : slot + 4], "big") + outer) & 0xFFFFFFFF
            if not 0x400 <= target < len(self.after) or not self.after[target]:
                return scanned, target
            scanned.add(target)
            if target >= table:
                stop = min(stop, target)
        return scanned, None
