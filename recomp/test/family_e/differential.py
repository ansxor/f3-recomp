"""Run the Musashi flow corpus with seeded states and the shared C harness."""

import argparse
import struct
from dataclasses import replace
from pathlib import Path
from random import Random
from unittest.mock import patch

from tools.differential.generator import (
    SAFE_A_REGS,
    TEST_PC,
    TEST_SP,
    MemInitItem,
    RawTestCase,
    generate_boundary_cases,
    generate_semantic_core_cases,
)
from tools.differential.runner import _get_capstone_disassembler, run_differential


def words(*values):
    return struct.pack(">" + "H" * len(values), *values)


def generated_cases(seed):
    random = Random(seed)
    cases = []

    def add(name, code, *, sr=None, memory=(), registers=None, count=1):
        case_id = len(cases) + 1
        cases.append(
            RawTestCase(
                id=case_id,
                name=f"{name}_{case_id}",
                code_bytes=code,
                initial_d=registers if registers is not None else [random.getrandbits(32) for _ in range(8)],
                initial_sr=sr if sr is not None else random.choice((0, 0x2000, 0x3700)) | random.randrange(32),
                initial_mem=list(memory),
                seed=seed,
                instruction_count=count,
            )
        )

    for index in range(160):
        condition = index % 16
        displacement = 2 * random.randrange(4, 62)
        for cc in (0, 1, 2 + index % 14):
            opcode = 0x6000 | cc << 8
            code = (
                words(opcode | displacement),
                words(opcode, displacement),
                words(opcode | 0xFF, 0, displacement),
            )[index % 3]
            add("branch", code)
        registers = [random.getrandbits(32) for _ in range(8)]
        reg = index % 8
        if index % 3 == 0:
            registers[reg] = (registers[reg] & 0xFFFF0000) | random.randrange(2)
        add("dbcc", words(0x50C8 | condition << 8 | reg, displacement), registers=registers)

        # Include brief and full indexes, and a JSR whose base aliases SP.
        ea, extension, pointers = (
            (0x17, b"", ()),
            (0x28, words(8), ()),
            (0x30, words(0x8008), ()),
            (0x30, words(0x0161, 8), (MemInitItem(SAFE_A_REGS[0] + 8, struct.pack(">I", TEST_PC + 0x80)),)),
            (0x38, words(0x6000), ()),
            (0x39, words(1, 0x0200), ()),
            (0x3A, words(displacement), ()),
            (0x3B, words(0x8008), ()),
        )[index % 8]
        add("jmp", words(0x4EC0 | ea) + extension, memory=pointers)
        add("jsr", words(0x4E80 | ea) + extension, memory=pointers)

        target = TEST_PC + displacement
        stack_pc = [MemInitItem(TEST_SP, struct.pack(">I", target))]
        add("rts", words(0x4E75), memory=stack_pc)
        add("rtd", words(0x4E74, (displacement if index % 2 else -displacement) & 0xFFFF), memory=stack_pc)
        add("rtr", words(0x4E77), memory=[MemInitItem(TEST_SP, struct.pack(">HI", random.randrange(0x10000), target))])
        frame_sr = random.choice((0, 0x2000, 0x3000)) | random.randrange(32)
        frame = struct.pack(">HIHI", frame_sr, target, (index % 2) << 13, TEST_PC - 2)
        add("rte", words(0x4E73), memory=[MemInitItem(TEST_SP, frame)])

        for status_bit in (0, 0x40):
            opcode = (0x003C, 0x023C, 0x0A3C)[index % 3] | status_bit
            immediate = random.randrange(0x4000) & (0xFFFF if status_bit else 0xFF)
            add("logic_status", words(opcode, immediate))
        for opcode in (0x40C0, 0x42C0):
            add("from_status", words(opcode | reg))
        for opcode in (0x44FC, 0x46FC):
            add("to_status", words(opcode, random.randrange(0x4000)))
        add("move_usp", words((0x4E60 if index % 2 else 0x4E68) | reg))
        add("stop", words(0x4E72, random.randrange(0x4000)))
        add("reset", words(0x4E70))
        add("nop", words(0x4E71))

        # Keep trace clear and let each trap use the harness's vector table.
        add("trap", words(0x4E40 | condition))
        add("trapv", words(0x4E76))
        opcode, operand = ((0x50FC, b""), (0x50FA, words(0x1234)), (0x50FB, words(0x1234, 0x5678)))[index % 3]
        add("trapcc", words(opcode | condition << 8) + operand)

        width = (1, 2, 4)[index % 3]
        addressing = (0x10, 0x18, 0x20)[index // 3 % 3]
        extension = (0x8000 if index % 2 else 0x1000) | (0x800 if index // 2 % 2 else 0)
        address = SAFE_A_REGS[0] - (width if addressing == 0x20 else 0)
        data = random.getrandbits(width * 8).to_bytes(width, "big")
        add("moves", words(0x0E00 | (index % 3) << 6 | addressing, extension), memory=[MemInitItem(address, data)])

        control = (0, 1, 2, 0x800, 0x801, 0x802, 0x803, 0x804)[index % 8]
        registers[0] &= 3 if control == 2 else 0xFFFFFFFF
        sr = 0x2000 | random.choice((0, 0x2000, 0x3700)) | random.randrange(32)
        add("movec_roundtrip", words(0x4E7B, control, 0x4E7A, 0x1000 | control),
            sr=sr, registers=registers, count=2)

    # A throwaway frame on ISP resumes by popping the real frame on MSP.
    throwaway = MemInitItem(TEST_SP, struct.pack(">HIH", 0x3000, TEST_PC + 0x40, 0x1000))
    master = MemInitItem(SAFE_A_REGS[1], struct.pack(">HIH", 0x2015, TEST_PC + 0x80, 0))
    add("rte_master", words(0x4E7B, 0x1803, 0x4E73), sr=0x2700,
        registers=[0, SAFE_A_REGS[1], 0, 0, 0, 0, 0, 0], memory=[throwaway, master], count=2)
    add("movec_invalid_user", words(0x4E7B, 0x0003), sr=0x001F)
    add("movec_invalid_supervisor", words(0x4E7B, 0x0003), sr=0x201F)
    add("movec_cache_clear", words(0x4E7B, 2, 0x4E7A, 0x1002), sr=0x201F,
        registers=[15, 0, 0, 0, 0, 0, 0, 0], count=2)
    add("callm", words(0x06D0, 0), sr=0x201F)
    add("rtm", words(0x06C0), sr=0x201F)
    return cases


def stock_flow_cases():
    decoder = _get_capstone_disassembler()
    return [
        case for case in generate_boundary_cases() + generate_semantic_core_cases(10000)
        if any(
            insn.mnemonic.startswith(("db", "trap", "movec", "moves"))
            or insn.mnemonic in ("jmp", "jsr", "rte", "rts", "rtd", "rtr", "reset", "stop", "nop")
            or any(reg in insn.op_str for reg in ("sr", "ccr", "usp"))
            for insn in decoder.disasm(case.code_bytes, case.initial_pc, 1)
        )
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    corpus = generated_cases(args.seed) + stock_flow_cases()
    cases = [replace(case, id=index) for index, case in enumerate(corpus, 1)]
    with (
        patch("tools.differential.runner.generate_boundary_cases", return_value=cases),
        patch("tools.differential.runner.generate_semantic_core_cases", return_value=[]),
    ):
        return run_differential(cases_count=len(cases), output_dir=args.output, seed=args.seed)


if __name__ == "__main__":
    raise SystemExit(main())
