"""68EC020 scheduling costs, from the pinned Musashi metadata.

These are the reference emulator's timings, not a claim of bus-cycle-accurate
hardware. The committed CSV contains CPU opcode metadata, never game ROM data.
"""
from pathlib import Path
from capstone.m68k import M68K_OP_MEM


def _load_base_cycles() -> bytes:
    table = bytearray(65536)
    for line in Path(__file__).with_name("68020_cycles.csv").read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        mask_text, match_text, cycles_text = line.split(",")
        mask, match, cycles = int(mask_text, 16), int(match_text, 16), int(cycles_text)
        free = (~mask) & 0xffff
        subset = free
        while True:
            table[match | subset] = cycles
            if subset == 0:
                break
            subset = (subset - 1) & free
    return bytes(table)


BASE_CYCLES = _load_base_cycles()
# Indexed full-extension costs; index is extension & 0x3f.
FULL_INDEX_CYCLES = (
    (0,) * 16 + (0, 5, 7, 7) * 4 +
    (2, 7, 9, 9) + (0, 7, 9, 9) * 3 +
    (6, 11, 13, 13) + (0, 11, 13, 13) * 3
)


def instruction_cycles(insn, pc_base, ea_extension_bytes) -> str:
    raw = bytes(insn.bytes)
    opcode = int.from_bytes(raw[:2], "big")
    mnemonic = insn.mnemonic.split(".")[0]
    cycles = BASE_CYCLES[opcode]
    if mnemonic == "movem":
        cycles += int.from_bytes(raw[2:4], "big").bit_count() * 4
    if mnemonic == "reset":
        cycles += 518
    offset = pc_base(insn) - insn.address
    try:
        size = insn.op_size.size
    except AttributeError:
        size = 4
    source_mode, source_reg = (opcode >> 3) & 7, opcode & 7
    operands = [(source_mode, source_reg, offset)]
    if mnemonic == "move":
        destination_offset = offset + ea_extension_bytes(raw, offset, source_mode, source_reg, size)
        operands.append(((opcode >> 6) & 7, (opcode >> 9) & 7, destination_offset))
    # Only opcodes that actually decode a memory operand have EA extension costs.
    if any(op.type == M68K_OP_MEM for op in insn.operands):
        for mode, reg, position in operands:
            if mode == 6 or (mode == 7 and reg == 3):
                extension = int.from_bytes(raw[position:position + 2], "big")
                if extension & 0x100:
                    cycles += FULL_INDEX_CYCLES[extension & 0x3f]
    if opcode & 0xf000 == 0x6000 and (opcode >> 8) & 15 >= 2 and insn.size == 2:
        return f"(cpu->pc == 0x{insn.address + insn.size:x}u ? {cycles - 2}u : {cycles}u)"
    return f"{cycles}u"
