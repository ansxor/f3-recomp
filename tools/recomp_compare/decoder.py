"""Evidence-based rules for baseline 68EC020 decoder comparisons."""

from tools.recomp_py.opcodes import LEGAL_68020


def matches(word, mask, value):
    return word & mask == value


def word_at(data, offset):
    return int.from_bytes(data[offset : offset + 2].ljust(2, b"\0"), "big")


def ea_bytes(code, data, offset, immediate, *, capstone):
    if code < 40:
        return 0
    if code < 48 or code in (56, 58):
        return 2
    if code == 57:
        return 4
    if code == 60:
        return immediate
    if code >= 56 and code != 59:
        return 0
    extension = word_at(data, offset)
    if not extension & 0x100:
        return 2
    base = (0, 0, 2, 4)[extension >> 4 & 3]
    outer = (0, 0, 2, 4)[extension & 3]
    if capstone and extension & 0x44 == 0x44:
        outer = 0
    return 2 + base + outer


def indexed_length(data, opcode, *, capstone):
    """Size both MOVE EAs, or a single indexed EA after the instruction's prefix."""
    group, code = opcode >> 12, opcode & 63
    if group in (1, 2, 3):
        dst = (opcode >> 3 & 0x38) | (opcode >> 9 & 7)
        src_bytes = ea_bytes(code, data, 2, 4 if group == 2 else 2, capstone=capstone)
        return 2 + src_bytes + ea_bytes(dst, data, 2 + src_bytes, 0, capstone=capstone)
    if not (48 <= code < 56 or code == 59):
        return None
    offset = 2
    if group == 0 and not opcode & 0x100:
        opmode = opcode >> 6 & 3
        offset += 4 if opmode == 2 and opcode >> 9 not in (4, 7) else 2
    if matches(opcode, 0xFF80, 0x4C00) or matches(opcode, 0xFB80, 0x4880):
        offset += 2
    if matches(opcode, 0xF8C0, 0xE8C0):
        offset += 2
    return offset + ea_bytes(code, data, offset, 0, capstone=capstone)


def checked_extension(opcode, data):
    """Capstone 5.0.7 M68KInstructionTable.inc word2_mask checks."""
    group = opcode >> 12
    ext = word_at(data, 2)
    if matches(opcode, 0xFF00, 0x0800):
        mask = 0xFE00 if opcode & 0xC0 == 0xC0 else 0xFF00
        if ext & mask:
            return "bit-immediate-padding"
    if opcode in (0x003C, 0x023C, 0x0A3C) and ext & 0xFF00:
        return "ccr-immediate-padding"
    if group == 0 and opcode & 0x01C0 == 0x00C0:
        family = opcode >> 9
        if family <= 2 and ext & 0x07FF:
            return "bounds-extension"
        if family == 3 and opcode & 63 >= 16 and ext & 0xFF00:
            return "callm-extension"
        if opcode in (0x0CFC, 0x0EFC) and (ext | word_at(data, 4)) & 0x0E38:
            return "cas2-extension"
        if family >= 5 and opcode & 63 != 60 and ext & 0xFE38:
            return "cas-extension"
    if matches(opcode, 0xFF00, 0x0E00) and opcode & 0xC0 != 0xC0 and ext & 0x07FF:
        return "moves-extension"
    if matches(opcode, 0xFF80, 0x4C00) and ext & 0x83F8:
        return "mul-div-extension"
    if matches(opcode, 0xF8C0, 0xE8C0):
        has_register = (opcode >> 8 & 7) in (1, 3, 5, 7)
        if ext & (0x8000 if has_register else 0xF000):
            return "bitfield-extension"
    return None


def prefixed_decision(rule, opcode, ext, data):
    """Check the OCaml row even though Capstone has no corresponding length."""
    flow = "next"
    if rule == "cas2-extension":
        length = 6
    elif rule == "ccr-immediate-padding":
        length = 4
    else:
        length = 4 + ea_bytes(opcode & 63, data, 4, 4, capstone=False)
        if rule == "callm-extension":
            flow = "escape"
        elif (rule == "bounds-extension" and ext & 0x800) or (rule == "mul-div-extension" and opcode & 0x40):
            flow = "branch:next,escape"
    return f"{length}, {flow}" if length <= len(data) else None


def category(rom, pc, column, before, after):
    if pc + 2 > len(rom):
        return None
    data = rom[pc : pc + 22]
    opcode = word_at(data, 0)
    ext = word_at(data, 2)
    group = opcode >> 12

    if (column == "length" and before.isdecimal() and after == "-") or (column == "flow" and after == "escape"):
        if group in (10, 15):
            return "line-exception"
        if opcode == 0x4AFC or matches(opcode, 0xFFF8, 0x4848):
            return "illegal-bkpt"
        if opcode in (0x4E7A, 0x4E7B) and ext & 0xFFF not in (0, 1, 2, 0x800, 0x801, 0x802, 0x803, 0x804):
            return "movec-control"

    if column == "entry" and before == "<missing>" and LEGAL_68020[opcode]:
        rule = checked_extension(opcode, data)
        if rule and after == prefixed_decision(rule, opcode, ext, data):
            return rule
        if len(data) == 2 and matches(opcode, 0xFFF8, 0x0000) and after == "-, escape":
            return "rom-tail"

    if column == "length" and before.isdecimal() and after.isdecimal():
        if matches(opcode, 0xF1F8, 0x8108) and (before, after) == ("4", "2"):
            return "sbcd-length"
        if matches(opcode, 0xF1FF, 0x013C) and (before, after) == ("6", "4"):
            return "btst-immediate-length"
        if LEGAL_68020[opcode] and group not in (6, 7, 10, 15):
            old_length = indexed_length(data, opcode, capstone=True)
            new_length = indexed_length(data, opcode, capstone=False)
            if (old_length, new_length) == (int(before), int(after)):
                return "indexed-outer-length"

    if column == "flow":
        if after == "branch:next,escape" and before == "next":
            trapcc = matches(opcode, 0xF0F8, 0x50F8) and opcode & 7 in (2, 3, 4)
            chk = opcode & 0xF1C0 in (0x4100, 0x4180)
            chk2 = opcode & 0xF9C0 == 0x00C0 and ext & 0x800
            divide = opcode & 0xF0C0 == 0x80C0 or opcode & 0xFFC0 == 0x4C40
            if trapcc or chk or chk2 or divide or opcode == 0x4E76:
                return "conditional-exception-flow"
        if before == "next" and after == "escape":
            if matches(opcode, 0xFFF0, 0x4E40) or opcode in (0x50FA, 0x50FB, 0x50FC):
                return "trap-flow"
            if opcode == 0x4E72:
                return "stop-flow"
            if matches(opcode, 0xFFC0, 0x06C0):
                return "module-flow"
        if matches(opcode, 0xFFF8, 0x50C8) and before.startswith("branch:") and after == "next":
            return "dbt-flow"
        code = opcode & 63
        if (
            matches(opcode, 0xFF80, 0x4E80)
            and (48 <= code < 56 or code == 59)
            and matches(ext, 0x0147, 0x0140)
            and (ext & 0x80 or code == 59)
            and before == "escape"
        ):
            base = 0 if ext & 0x80 else pc + 2
            width = (0, 0, 2, 4)[ext >> 4 & 3]
            disp = int.from_bytes(data[4 : 4 + width], "big", signed=True)
            if after == f"branch:0x{(base + disp) & 0xFFFFFF:06x}":
                return "constant-indexed-transfer"
    return None
