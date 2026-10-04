"""Instruction test case generator for 68EC020 differential testing.

Generates deterministic synthetic test cases covering:
1. Targeted boundary states:
   - Sticky-Z in ADDX/SUBX/NEGX
   - Shift/rotate with counts 0, 1, 8, 31, 32, 33..64
   - Branch displacements: byte (.s), word (.w), long (.l on 68020) and all 16 conditions
   - MOVEA.W sign extension without CCR modification vs MOVEA.L
   - EXT.W, EXT.L, EXTB.L sign extension
   - Postincrement/predecrement A7 byte (increments/decrements by 2 instead of 1)
   - Zero, sign, carry/borrow, and signed overflow boundaries across .b, .w, .l
2. Meaningful opcode families:
   - MOVE, MOVEA, MOVEQ, MOVEM, LEA, PEA, EXG, SWAP
   - ADD, ADDA, ADDI, ADDQ, ADDX, SUB, SUBA, SUBI, SUBQ, SUBX, NEG, NEGX
   - CMP, CMPA, CMPI, CMPM, TST, CLR
   - AND, ANDI, OR, ORI, EOR, EORI, NOT
   - ASL, ASR, LSL, LSR, ROL, ROR, ROXL, ROXR
   - BTST, BSET, BCLR, BCHG
   - MULU.W, MULS.W, MULU.L, MULS.L, DIVU.W, DIVS.W, DIVU.L, DIVS.L
   - LINK, UNLK, BSR, JSR, RTS, DBcc, Scc
   - MOVE to/from CCR, ANDI/ORI/EORI to CCR
   - 68020 synthetic brief and full format EA extension words
   - 68020 bitfield instructions (BFTST, BFEXTU, BFEXTS, BFINS, BFSET, BFCLR, BFCHG, BFFFO)
3. External instruction stream ingestion (JSON / ROM discovery).
"""
from __future__ import annotations

from dataclasses import dataclass, field
import json
from pathlib import Path
import random
import struct
from typing import Any, Iterator, Sequence

# Safe memory layout for synthetic differential testing
TEST_PC = 0x00010000
TEST_SP = 0x00028000
TEST_A0 = 0x00030000
TEST_A1 = 0x00032000
TEST_A2 = 0x00034000
TEST_A3 = 0x00036000
TEST_A4 = 0x00038000
TEST_A5 = 0x0003A000
TEST_A6 = 0x0003C000

SAFE_A_REGS = [TEST_A0, TEST_A1, TEST_A2, TEST_A3, TEST_A4, TEST_A5, TEST_A6, TEST_SP]


@dataclass
class MemInitItem:
    address: int
    data: bytes


@dataclass
class RawTestCase:
    id: int
    name: str
    code_bytes: bytes
    initial_pc: int = TEST_PC
    initial_d: list[int] = field(default_factory=lambda: [0] * 8)
    initial_a: list[int] = field(default_factory=lambda: list(SAFE_A_REGS))
    initial_sr: int = 0x0000
    initial_mem: list[MemInitItem] = field(default_factory=list)
    is_boundary: bool = False
    boundary_kind: str = ""
    seed: int = 0
    instruction_count: int = 1


def generate_boundary_cases() -> list[RawTestCase]:
    """Generate exhaustive targeted boundary cases."""
    cases: list[RawTestCase] = []
    case_id = 1

    # -------------------------------------------------------------------------
    # 1. Sticky-Z in ADDX / SUBX / NEGX
    # -------------------------------------------------------------------------
    # ADDX/SUBX D0,D1; size bits are supplied independently below.
    for name, op_word, is_sub in [("addx", 0xD300, False), ("subx", 0x9300, True)]:
        for size_name, sz_bits, mask in [("b", 0x00, 0xFF), ("w", 0x40, 0xFFFF), ("l", 0x80, 0xFFFFFFFF)]:
            code = struct.pack(">H", op_word | sz_bits)
            # Case A: initial Z=1, result=0 -> Z must remain 1
            cases.append(RawTestCase(
                id=case_id,
                name=f"{name}_{size_name}_sticky_z_remains_1",
                code_bytes=code,
                initial_d=[0, 0, 0, 0, 0, 0, 0, 0],
                initial_sr=0x0004,  # Z=1, X=0
                is_boundary=True,
                boundary_kind="sticky_z",
            ))
            case_id += 1
            # Case B: initial Z=1, result != 0 -> Z must become 0
            val = 5 if not is_sub else 5
            cases.append(RawTestCase(
                id=case_id,
                name=f"{name}_{size_name}_sticky_z_cleared_by_nonzero",
                code_bytes=code,
                initial_d=[val, 0, 0, 0, 0, 0, 0, 0],
                initial_sr=0x0004,  # Z=1, X=0
                is_boundary=True,
                boundary_kind="sticky_z",
            ))
            case_id += 1
            # Case C: initial Z=0, result == 0 -> Z must remain 0! (Sticky-Z behavior)
            cases.append(RawTestCase(
                id=case_id,
                name=f"{name}_{size_name}_sticky_z_remains_0_on_zero_result",
                code_bytes=code,
                initial_d=[0, 0, 0, 0, 0, 0, 0, 0],
                initial_sr=0x0000,  # Z=0, X=0
                is_boundary=True,
                boundary_kind="sticky_z",
            ))
            case_id += 1

    # NEGX.L D0: 0x4080
    for size_name, sz_bits in [("b", 0x00), ("w", 0x40), ("l", 0x80)]:
        code = struct.pack(">H", 0x4000 | sz_bits)
        cases.append(RawTestCase(
            id=case_id,
            name=f"negx_{size_name}_sticky_z_remains_1",
            code_bytes=code,
            initial_d=[0] * 8,
            initial_sr=0x0004,  # Z=1
            is_boundary=True,
            boundary_kind="sticky_z",
        ))
        case_id += 1
        cases.append(RawTestCase(
            id=case_id,
            name=f"negx_{size_name}_sticky_z_remains_0",
            code_bytes=code,
            initial_d=[0] * 8,
            initial_sr=0x0000,  # Z=0
            is_boundary=True,
            boundary_kind="sticky_z",
        ))
        case_id += 1

    # -------------------------------------------------------------------------
    # 2. Shift / Rotate with counts 0, 1, 8, 31, 32, 33, 64
    # -------------------------------------------------------------------------
    # In 68k: Register shift/rotate takes count modulo 64 from Dy:
    # LSL.L D0, D1: 0xE1A9 (Dy=0, Dx=1, L/R=1, size=10, i/r=1)
    # LSR.L D0, D1: 0xE0A9
    # ASL.L D0, D1: 0xE1A1
    # ASR.L D0, D1: 0xE0A1
    # ROL.L D0, D1: 0xE1B9
    # ROR.L D0, D1: 0xE0B9
    # ROXL.L D0, D1: 0xE1B1
    # ROXR.L D0, D1: 0xE0B1
    shift_ops = [
        ("lsl_l_reg", 0xE1A9),
        ("lsr_l_reg", 0xE0A9),
        ("asl_l_reg", 0xE1A1),
        ("asr_l_reg", 0xE0A1),
        ("rol_l_reg", 0xE1B9),
        ("ror_l_reg", 0xE0B9),
        ("roxl_l_reg", 0xE1B1),
        ("roxr_l_reg", 0xE0B1),
    ]
    test_counts = [0, 1, 8, 16, 31, 32, 33, 63, 64]
    for name, op in shift_ops:
        code = struct.pack(">H", op)
        for count in test_counts:
            # Test with operand having MSB and LSB set
            operand = 0x80000001
            cases.append(RawTestCase(
                id=case_id,
                name=f"{name}_count_{count}",
                code_bytes=code,
                initial_d=[count, operand, 0, 0, 0, 0, 0, 0],
                initial_sr=0x0010,  # X=1
                is_boundary=True,
                boundary_kind="shift_count",
            ))
            case_id += 1

    # Immediate shifts: LSL.L #1, D0 (0xE388), LSL.L #8, D0 (0xE188 count=0 means 8)
    for shift_name, op in [("lsl_imm", 0xE388), ("lsr_imm", 0xE288), ("asl_imm", 0xE380), ("asr_imm", 0xE280)]:
        code = struct.pack(">H", op)
        cases.append(RawTestCase(
            id=case_id,
            name=f"{shift_name}_imm_edge",
            code_bytes=code,
            initial_d=[0x80000001, 0, 0, 0, 0, 0, 0, 0],
            initial_sr=0x0000,
            is_boundary=True,
            boundary_kind="shift_imm",
        ))
        case_id += 1

    # EC020 immediate rotates have fixed opcode timing, independent of count.
    for name, base in (("ror", 0xe018), ("rol", 0xe118),
                       ("roxr", 0xe010), ("roxl", 0xe110)):
        for size_bits, count in ((0, 8), (0x40, 3), (0x80, 4)):
            cases.append(RawTestCase(
                id=case_id, name=f"{name}_immediate_{size_bits:x}_{count}",
                code_bytes=struct.pack(">H", base | size_bits | ((count & 7) << 9)),
                initial_d=[0x81ff8001, 0, 0, 0, 0, 0, 0, 0], initial_sr=0x001f,
                is_boundary=True, boundary_kind="shift_imm"))
            case_id += 1

    # -------------------------------------------------------------------------
    # 3. Branch Displacements: Byte, Word, Long and All 16 Conditions
    # -------------------------------------------------------------------------
    # Bcc opcode: 0x6000 | (cond << 8) | disp8
    # If disp8 == 0: followed by 16-bit word displacement
    # If disp8 == 0xFF: followed by 32-bit long displacement (68020)
    cond_names = [
        "bra", "bsr", "bhi", "bls", "bcc", "bcs", "bne", "beq",
        "bvc", "bvs", "bpl", "bmi", "bge", "blt", "bgt", "ble"
    ]
    for cond in range(16):
        cname = cond_names[cond]
        # Condition setups for taken vs not taken
        # Test taken:
        # e.g. for BEQ (cond 7), set Z=1; for BNE (cond 6), set Z=0; etc.
        for taken in ([True] if cond < 2 else [True, False]):
            sr_val = 0
            if cname in ("bra", "bsr"):
                sr_val = 0
            elif cname == "beq":
                sr_val = 0x0004 if taken else 0x0000  # Z
            elif cname == "bne":
                sr_val = 0x0000 if taken else 0x0004
            elif cname == "bcs":
                sr_val = 0x0001 if taken else 0x0000  # C
            elif cname == "bcc":
                sr_val = 0x0000 if taken else 0x0001
            elif cname == "bmi":
                sr_val = 0x0008 if taken else 0x0000  # N
            elif cname == "bpl":
                sr_val = 0x0000 if taken else 0x0008
            elif cname == "bvs":
                sr_val = 0x0002 if taken else 0x0000  # V
            elif cname == "bvc":
                sr_val = 0x0000 if taken else 0x0002
            elif cname == "bhi":
                sr_val = 0x0000 if taken else 0x0005  # C=0, Z=0
            elif cname == "bls":
                sr_val = 0x0004 if taken else 0x0000  # C=1 or Z=1
            elif cname == "bge":
                sr_val = 0x0000 if taken else 0x0008  # N == V
            elif cname == "blt":
                sr_val = 0x0008 if taken else 0x0000  # N != V
            elif cname == "bgt":
                sr_val = 0x0000 if taken else 0x0004  # Z=0 and N==V
            elif cname == "ble":
                sr_val = 0x0004 if taken else 0x0000  # Z=1 or N!=V

            # Byte displacement (+4 bytes)
            code_byte = struct.pack(">H", 0x6000 | (cond << 8) | 0x04)
            cases.append(RawTestCase(
                id=case_id,
                name=f"{cname}_byte_{'taken' if taken else 'not_taken'}",
                code_bytes=code_byte,
                initial_sr=sr_val,
                is_boundary=True,
                boundary_kind="branch_byte",
            ))
            case_id += 1

            # Word displacement (+0x0100 bytes)
            code_word = struct.pack(">HH", 0x6000 | (cond << 8) | 0x00, 0x0100)
            cases.append(RawTestCase(
                id=case_id,
                name=f"{cname}_word_{'taken' if taken else 'not_taken'}",
                code_bytes=code_word,
                initial_sr=sr_val,
                is_boundary=True,
                boundary_kind="branch_word",
            ))
            case_id += 1

            # Long displacement (+0x00000200 bytes on 68020)
            code_long = struct.pack(">HI", 0x6000 | (cond << 8) | 0xFF, 0x00000200)
            cases.append(RawTestCase(
                id=case_id,
                name=f"{cname}_long_{'taken' if taken else 'not_taken'}",
                code_bytes=code_long,
                initial_sr=sr_val,
                is_boundary=True,
                boundary_kind="branch_long",
            ))
            case_id += 1

    # -------------------------------------------------------------------------
    # 4. Sign Extension: MOVEA.W vs MOVEA.L vs EXT/EXTB
    # -------------------------------------------------------------------------
    # MOVEA.W D0, A0: 0x3040 (sign extends 16-bit to 32-bit, preserves CCR!)
    # MOVEA.L D0, A0: 0x2040 (preserves CCR!)
    for val, label in [(0x8000, "neg"), (0x7FFF, "pos"), (0xFFFF, "minus_one")]:
        cases.append(RawTestCase(
            id=case_id,
            name=f"movea_w_{label}_preserves_ccr",
            code_bytes=struct.pack(">H", 0x3040),
            initial_d=[val, 0, 0, 0, 0, 0, 0, 0],
            initial_a=[0, 0, 0, 0, 0, 0, 0, TEST_SP],
            initial_sr=0x001F,  # All CCR flags set
            is_boundary=True,
            boundary_kind="movea_sign",
        ))
        case_id += 1

    # EXT.W D0 (0x4880), EXT.L D0 (0x48C0), EXTB.L D0 (0x49C0 68020)
    for ext_name, op in [("ext_w", 0x4880), ("ext_l", 0x48C0), ("extb_l", 0x49C0)]:
        code = struct.pack(">H", op)
        for val in [0x80, 0x7F, 0x8000, 0x7FFF]:
            cases.append(RawTestCase(
                id=case_id,
                name=f"{ext_name}_val_{val:#x}",
                code_bytes=code,
                initial_d=[val, 0, 0, 0, 0, 0, 0, 0],
                initial_sr=0x0000,
                is_boundary=True,
                boundary_kind="ext_sign",
            ))
            case_id += 1

    # -------------------------------------------------------------------------
    # 5. Postincrement / Predecrement A7 Byte (Stack alignment +2/-2)
    # -------------------------------------------------------------------------
    # MOVE.B (A7)+, D0: 0x101F -> A7 must increment by 2, not 1!
    # MOVE.B -(A7), D0: 0x1027 -> A7 must decrement by 2, not 1!
    # MOVE.B (A0)+, D0: 0x1018 -> A0 increments by 1
    # MOVE.B -(A0), D0: 0x1020 -> A0 decrements by 1
    mem_at_sp = MemInitItem(address=TEST_SP - 8, data=bytes.fromhex("123456789abcdef03ca55ac3"))
    mem_at_a0 = MemInitItem(address=TEST_A0 - 8, data=bytes.fromhex("123456789abcdef03ca55ac3"))

    cases.append(RawTestCase(
        id=case_id,
        name="move_b_postinc_a7_step2",
        code_bytes=struct.pack(">H", 0x101F),  # move.b (a7)+, d0
        initial_mem=[mem_at_sp],
        is_boundary=True,
        boundary_kind="postinc_a7_byte",
    ))
    case_id += 1

    cases.append(RawTestCase(
        id=case_id,
        name="move_b_predec_a7_step2",
        code_bytes=struct.pack(">H", 0x1027),  # move.b -(a7), d0
        initial_mem=[mem_at_sp],
        is_boundary=True,
        boundary_kind="predec_a7_byte",
    ))
    case_id += 1

    cases.append(RawTestCase(
        id=case_id,
        name="move_b_postinc_a0_step1",
        code_bytes=struct.pack(">H", 0x1018),  # move.b (a0)+, d0
        initial_mem=[mem_at_a0],
        is_boundary=True,
        boundary_kind="postinc_a0_byte",
    ))
    case_id += 1

    # -------------------------------------------------------------------------
    # 6. Arithmetic Flags: Zero / Sign / Carry / Overflow Boundaries
    # -------------------------------------------------------------------------
    # ADD.L D0, D1: 0xD280
    # SUB.L D0, D1: 0x9280
    # CMP.L D0, D1: 0xB280
    arith_ops = [("add_l", 0xD280), ("sub_l", 0x9280), ("cmp_l", 0xB280)]
    arith_cases = [
        ("zero_plus_zero", 0, 0),
        ("signed_overflow_pos", 0x7FFFFFFF, 1),
        ("signed_overflow_neg", 0x80000000, 0x80000000),
        ("unsigned_carry", 0xFFFFFFFF, 1),
        ("borrow_from_zero", 1, 0),
        ("equal_values", 0x12345678, 0x12345678),
    ]
    for op_name, op_code in arith_ops:
        code = struct.pack(">H", op_code)
        for label, d0, d1 in arith_cases:
            cases.append(RawTestCase(
                id=case_id,
                name=f"{op_name}_{label}",
                code_bytes=code,
                initial_d=[d0, d1, 0, 0, 0, 0, 0, 0],
                initial_sr=0x0000,
                is_boundary=True,
                boundary_kind="arith_flags",
            ))
            case_id += 1

    # -------------------------------------------------------------------------
    # 7. 68020 Brief and Full Format EA Extension Words
    # -------------------------------------------------------------------------
    # MOVE.L (d8, A0, D1.L*4), D2: 0x2430 | brief extension word
    # Brief extension word format:
    # bit 15: D/A=0 (D1), bits 14-12: reg 1, bit 11: W/L=1 (long), bits 10-9: scale (00=1, 01=2, 10=4, 11=8), bit 8: 0, bits 7-0: disp8
    for scale_shift, scale_name in [(0, "scale1"), (1, "scale2"), (2, "scale4"), (3, "scale8")]:
        ext_word = (1 << 12) | (1 << 11) | (scale_shift << 9) | 0x10  # D1.L*scale + 0x10
        code = struct.pack(">HH", 0x2430, ext_word)
        target_addr = TEST_A0 + (4 << scale_shift) + 0x10
        mem_item = MemInitItem(address=target_addr, data=b"\xCA\xFE\xBA\xBE")
        cases.append(RawTestCase(
            id=case_id,
            name=f"move_l_brief_ea_{scale_name}",
            code_bytes=code,
            initial_d=[0, 4, 0, 0, 0, 0, 0, 0],  # D1 = 4
            initial_a=list(SAFE_A_REGS),
            initial_mem=[mem_item],
            is_boundary=True,
            boundary_kind="brief_ea",
        ))
        case_id += 1

    # Full format extension word:
    # MOVE.L ([bd, A0, D1.L*2], od), D2
    # Full ext format: bit 8 = 1
    # BS=0, IS=0, BD size=10 (word), I/IS=001 (pre-indexed null OD)
    full_ext = (1 << 12) | (1 << 11) | (1 << 9) | 0x0100 | (2 << 4) | 1  # word BD, null OD
    code_full = struct.pack(">HHH", 0x2430, full_ext, 0x0020)  # BD = 0x20
    # Pointer table at A0 + D1*2 + BD = TEST_A0 + 8 + 0x20 = TEST_A0 + 0x28
    ptr_addr = TEST_A0 + 8 + 0x20
    target_data_addr = 0x00035000
    mem1 = MemInitItem(address=ptr_addr, data=struct.pack(">I", target_data_addr))
    mem2 = MemInitItem(address=target_data_addr, data=b"\xDE\xAD\xBE\xEF")
    cases.append(RawTestCase(
        id=case_id,
        name="move_l_full_format_ea_preindexed",
        code_bytes=code_full,
        initial_d=[0, 4, 0, 0, 0, 0, 0, 0],
        initial_mem=[mem1, mem2],
        is_boundary=True,
        boundary_kind="full_ea",
    ))
    case_id += 1

    # -------------------------------------------------------------------------
    # 8. 68020 Bitfield Instructions (BFTST, BFEXTU, BFEXTS, BFINS, BFSET, BFCLR)
    # -------------------------------------------------------------------------
    # BFEXTU D0{offset:width}, D1: 0xE9C0 | extension word
    # Ext word: bit 11: Do (0=imm), bits 10-6: offset, bit 5: Dw (0=imm), bits 4-0: width
    for bf_name, op in [("bfextu", 0xE9C0), ("bfexts", 0xEBc0), ("bftst", 0xE8C0), ("bfset", 0xEEC0)]:
        ext = (4 << 6) | 12  # offset 4, width 12
        code = struct.pack(">HH", op, ext)
        cases.append(RawTestCase(
            id=case_id,
            name=f"{bf_name}_d0_off4_w12",
            code_bytes=code,
            initial_d=[0x12345678, 0, 0, 0, 0, 0, 0, 0],
            is_boundary=True,
            boundary_kind="bitfield",
        ))
        case_id += 1

    # -------------------------------------------------------------------------
    # 9. Multiply & Divide
    # -------------------------------------------------------------------------
    # Word multiply/divide D0,D1, including signed source extension.
    for muldiv_name, op, d0, d1 in [
        ("mulu_w", 0xC2C0, 0x1234, 0x5678),
        ("muls_w_neg", 0xC3C0, 0xFFFF, 0x0005),
        ("divu_w", 0x82C0, 0x000A, 0x00000064),
        ("divs_w_neg", 0x83C0, 0xFFFE, 0x00000064),
    ]:
        code = struct.pack(">H", op)
        cases.append(RawTestCase(
            id=case_id,
            name=muldiv_name,
            code_bytes=code,
            initial_d=[d0, d1, 0, 0, 0, 0, 0, 0],
            is_boundary=True,
            boundary_kind="mul_div",
        ))
        case_id += 1

    # -------------------------------------------------------------------------
    # 10. Bit Ops: BTST, BSET, BCLR, BCHG
    # -------------------------------------------------------------------------
    # Dynamic: BTST D0, D1 (0x0101)
    # Static: BTST #bit, D1 (0x0801, imm)
    for bit_op_name, op in [("btst_reg", 0x0101), ("bset_reg", 0x01C1), ("bclr_reg", 0x0181), ("bchg_reg", 0x0141)]:
        code = struct.pack(">H", op)
        cases.append(RawTestCase(
            id=case_id,
            name=bit_op_name,
            code_bytes=code,
            initial_d=[5, 0x00000020, 0, 0, 0, 0, 0, 0],
            is_boundary=True,
            boundary_kind="bit_ops",
        ))
        case_id += 1

    # Aliasing, control registers, MOVEM ordering, and extension-word PC bases.
    def add_case(name, code, *, d=None, a=None, sr=0, memory=()):
        cases.append(RawTestCase(
            id=len(cases) + 1, name=name, code_bytes=bytes.fromhex(code),
            initial_d=list(d or [0x12345678, 7, 0, 0, 0, 0, 0, 0]),
            initial_a=list(a or SAFE_A_REGS), initial_sr=sr,
            initial_mem=list(memory), is_boundary=True, boundary_kind="operand_aliasing"))

    add_case("move_a0_predec_alias", "2108")
    add_case("move_a0_postinc_alias", "20c8")
    add_case("jsr_stack_target", "4e97")
    add_case("pea_stack_target", "4857")
    add_case("unlk_stack_alias", "4e5f", memory=[
        MemInitItem(TEST_SP, bytes.fromhex("00029000"))])
    for value in (0, 0x7f, 0x80, 0xff):
        for sr in (0, 0x1f):
            add_case(f"tas_register_{value:x}_{sr:x}", "4ac0",
                     d=[0x12345600 | value, 0, 0, 0, 0, 0, 0, 0], sr=sr)
            for mode, opcode, address in (
                    ("indirect", "4ad0", TEST_A0),
                    ("postinc", "4ad8", TEST_A0),
                    ("stack_predec", "4ae7", TEST_SP - 2)):
                add_case(f"tas_{mode}_{value:x}_{sr:x}", opcode, sr=sr,
                         memory=[MemInitItem(address, bytes([value]))])
    for sr in (0x001f, 0x201f):
        for name, code in (("read_sr", "40c0"), ("write_sr", "46c0"),
                           ("read_ccr", "42c0"), ("write_ccr", "44c0"),
                           ("read_usp", "4e68"), ("write_usp", "4e60"),
                           ("ori_sr", "007c0001"), ("andi_sr", "027c2700"),
                           ("eori_sr", "0a7c0004")):
            add_case(f"{name}_{sr:04x}", code, sr=sr)
    add_case("movem_predec_base_in_list", "48e080c0")
    add_case("movem_word_predec_base_in_list", "48a080c0")
    add_case("movem_postinc_base_in_list", "4cd80301", memory=[
        MemInitItem(TEST_A0, bytes.fromhex("800012341122334455667788"))])
    add_case("movem_word_sign_extend", "4c900003", memory=[
        MemInitItem(TEST_A0, bytes.fromhex("80007fff"))])
    add_case("movem_pc_relative", "4cfa00010008", memory=[
        MemInitItem(TEST_PC + 12, bytes.fromhex("cafebabe"))])
    add_case("btst_immediate_pc_relative", "083a00000008", memory=[
        MemInitItem(TEST_PC + 12, bytes.fromhex("01"))])
    for count in (0, 1, 0xffff, 0x12340000):
        add_case(f"dbra_counter_{count:x}", "51c8fffc", d=[count, 0, 0, 0, 0, 0, 0, 0])
    for x in (0, 0x10):
        add_case(f"addx_byte_carry_{x}", "d300", d=[255, 0, 0, 0, 0, 0, 0, 0], sr=x | 4)
        add_case(f"subx_byte_borrow_{x}", "9300", d=[255, 0, 0, 0, 0, 0, 0, 0], sr=x | 4)
    add_case("scaled_index_wrap", "24301e10", d=[0, 0x40000000, 0, 0, 0, 0, 0, 0],
             memory=[MemInitItem(TEST_A0 + 16, bytes.fromhex("fedcba98"))])
    add_case("pc_full_extension", "243b11200020", d=[0, 4, 0, 0, 0, 0, 0, 0],
             memory=[MemInitItem(TEST_PC + 38, bytes.fromhex("1234abcd"))])
    add_case("asl_all_bits_shifted", "e1a1", d=[32, 0xffffffff, 0, 0, 0, 0, 0, 0])
    add_case("divs_word_min_over_minus_one", "83c0",
             d=[0xffff, 0x80000000, 0, 0, 0, 0, 0, 0])
    add_case("divu_word_overflow_flags", "82c0",
             d=[1, 0x10000, 0, 0, 0, 0, 0, 0], sr=0x1d)
    for name, code in (("mulu32", "4c001000"), ("mulu64", "4c001402"),
                       ("muls32", "4c001800"), ("muls64", "4c001c02"),
                       ("divu32", "4c401001"), ("divu32_remainder", "4c401002"),
                       ("divu64", "4c401402"), ("divs32", "4c401801"),
                       ("divs32_remainder", "4c401802"), ("divs64", "4c401c02")):
        for source, low, high in ((7, 100, 0), (0xfffffffe, 0xfffffff1, 0xffffffff),
                                  (1, 0x80000000, 0x12345678)):
            add_case(f"{name}_{source:x}_{low:x}_{high:x}", code,
                     d=[source, low, high, 0, 0, 0, 0, 0], sr=0x1f)
    for name, code in (("divu_zero", "82c0"), ("divs_zero", "83c0"),
                       ("divul_zero", "4c401002"), ("divsl_zero", "4c401802")):
        add_case(name, code, d=[0, 100, 23, 0, 0, 0, 0, 0], sr=0x201f)
    for name, code in (("rol", "e1b8"), ("ror", "e0b8"), ("roxl", "e1b0"), ("roxr", "e0b0")):
        add_case(f"{name}_count_destination_alias", code,
                 d=[0x80000021, 0, 0, 0, 0, 0, 0, 0], sr=0x1f)
    add_case("full_negative_word_bd", "41f50520fa26")
    add_case("full_negative_word_outer", "24301b220000fff8",
             d=[0, 4, 0, 0, 0, 0, 0, 0], memory=[
                 MemInitItem(TEST_A0 + 8, bytes.fromhex("00035008")),
                 MemInitItem(0x35000, bytes.fromhex("cafebabe"))])
    add_case("move_two_full_extensions", "23b01120fff021200010",
             d=[0, 4, 8, 0, 0, 0, 0, 0], memory=[
                 MemInitItem(TEST_A0 - 12, bytes.fromhex("deadbeef"))])
    add_case("brief_zero_displacement", "10300000",
             d=[3, 0, 0, 0, 0, 0, 0, 0],
             memory=[MemInitItem(TEST_A0 + 3, bytes.fromhex("a5"))])
    add_case("move_two_brief_extensions", "21b100001000",
             d=[4, 8, 0, 0, 0, 0, 0, 0],
             memory=[MemInitItem(TEST_A1 + 4, bytes.fromhex("cafebabe"))])
    for operation in range(8):
        for memory_ea in (False, True):
            for offset in (-9, -1, 0, 7, 31, 32):
                for width in (1, 8, 9, 16, 17, 24, 31, 32):
                    opcode = 0xe8c0 | (operation << 8) | (0x10 if memory_ea else 4)
                    add_case(f"bitfield_{operation}_{int(memory_ea)}_{offset}_{width}",
                             struct.pack(">HH", opcode, 0x3862 if operation & 1 else 0x0862).hex(),
                             d=[0, offset & 0xffffffff, width, 0xdeadbeef, 0xaabbccdd, 0, 0, 0],
                             sr=0x1f, memory=[
                                 MemInitItem(TEST_A0 - 8, bytes.fromhex("80ff5533aa661177809abcdeff012345"))])
    for fmt in (0, 1, 2, 3):
        frame = struct.pack(">HIH", 0x2015, TEST_PC + 0x80, fmt << 12)
        if fmt == 1:
            frame += struct.pack(">HIH", 0x2004, TEST_PC + 0x100, 0)
        add_case(f"rte_format_{fmt}", "4e73", sr=0x2700,
                 memory=[MemInitItem(TEST_SP, frame + bytes(8))])
    add_case("rte_privilege", "4e73", sr=0x001f)
    for vector in range(16):
        for sr in (0x0015, 0x201f):
            add_case(f"trap_{vector}_{sr:x}", f"{0x4e40 | vector:04x}", sr=sr,
                     memory=[MemInitItem((32 + vector) * 4,
                                         struct.pack(">I", TEST_PC + 0x100 + vector * 2))])
    for control in (0, 1, 2, 0x800, 0x801, 0x802, 0x803, 0x804):
        code = struct.pack(">HHHH", 0x4e7b, control, 0x4e7a, 0x1000 | control)
        cases.append(RawTestCase(
            id=len(cases) + 1, name=f"movec_roundtrip_{control:03x}", code_bytes=code,
            initial_d=[0xabcdef01, 0, 0, 0, 0, 0, 0, 0], initial_sr=0x201f,
            instruction_count=2, is_boundary=True, boundary_kind="control_register"))
    add_case("movec_privilege", "4e7b0002", sr=0x001f)
    for width, opcode in ((1, 0x00d0), (2, 0x02d0), (4, 0x04d0)):
        for value in (-129, -128, -1, 0, 1, 126, 127, 128, 0x10000):
            for reg in (0, 8):
                add_case(f"cmp2_{width}_{reg}_{value}", struct.pack(">HH", opcode, reg << 12).hex(),
                         d=[value & 0xffffffff, 0, 0, 0, 0, 0, 0, 0],
                         sr=0x1f, memory=[MemInitItem(TEST_A0,
                             (-128).to_bytes(width, "big", signed=True) +
                             (127).to_bytes(width, "big", signed=True))])
    for name, code in (
            ("lazy_x_through_move", "d2807600d583"),
            ("lazy_x_through_compare", "d280b680d583"),
            ("lazy_sr_observation", "d28040c2")):
        cases.append(RawTestCase(
            id=len(cases) + 1, name=name, code_bytes=bytes.fromhex(code),
            initial_d=[0xffffffff, 1, 0, 0, 0, 0, 0, 0], initial_sr=0x2000,
            instruction_count=len(code) // 4, is_boundary=True,
            boundary_kind="lazy_flag_transition"))

    return cases


def generate_random_cases(count: int, seed: int, start_id: int) -> list[RawTestCase]:
    """Generate deterministic randomized instruction cases across all opcode families."""
    rng = random.Random(seed)
    cases: list[RawTestCase] = []

    # Opcode templates (base word, possible sizes, operand generators)
    # 1. MOVE.B/W/L Dn, Dm
    # 2. ADD / SUB / AND / OR / EOR Dn, Dm
    # 3. MOVEQ #imm, Dn
    # 4. SWAP Dn
    # 5. NEG / NOT Dn
    # 6. CLR / TST Dn
    # 7. LEA (d16, An), Am
    # 8. PEA (d16, An)
    # 9. EXG Dn, Dm
    # 10. Shifts / Rotates

    for i in range(count):
        case_seed = rng.randint(0, 0xFFFFFFFFFFFF)
        case_rng = random.Random(case_seed)
        family = case_rng.choice([
            "move_dn", "moveq", "add_sub", "logical", "swap_ext",
            "clr_tst", "lea_pea", "exg", "shift", "cmp", "bit_op"
        ])

        initial_d = [case_rng.randint(0, 0xFFFFFFFF) for _ in range(8)]
        # Bias some registers to boundary values
        if case_rng.random() < 0.4:
            b_val = case_rng.choice([0, 1, 0xFF, 0x80, 0x7FFF, 0x8000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF])
            initial_d[case_rng.randint(0, 7)] = b_val

        initial_a = list(SAFE_A_REGS)
        initial_sr = case_rng.choice([0x0000, 0x0004, 0x0008, 0x0001, 0x0002, 0x0010, 0x001F])
        initial_mem: list[MemInitItem] = []

        code = b""
        name = ""

        if family == "moveq":
            d_reg = case_rng.randint(0, 7)
            imm = case_rng.randint(-128, 127) & 0xFF
            code = struct.pack(">H", 0x7000 | (d_reg << 9) | imm)
            name = f"moveq_d{d_reg}_{imm}"

        elif family == "move_dn":
            src = case_rng.randint(0, 7)
            dst = case_rng.randint(0, 7)
            sz = case_rng.choice([(0x1000, "b"), (0x3000, "w"), (0x2000, "l")])
            code = struct.pack(">H", sz[0] | (dst << 9) | src)
            name = f"move_{sz[1]}_d{src}_d{dst}"

        elif family == "add_sub":
            src = case_rng.randint(0, 7)
            dst = case_rng.randint(0, 7)
            is_sub = case_rng.choice([True, False])
            base = 0x9000 if is_sub else 0xD000
            sz = case_rng.choice([(0x00, "b"), (0x40, "w"), (0x80, "l")])
            code = struct.pack(">H", base | (dst << 9) | sz[0] | src)
            name = f"{'sub' if is_sub else 'add'}_{sz[1]}_d{src}_d{dst}"

        elif family == "logical":
            src = case_rng.randint(0, 7)
            dst = case_rng.randint(0, 7)
            op = case_rng.choice([(0xC000, "and"), (0x8000, "or"), (0xB100, "eor")])
            sz = case_rng.choice([(0x00, "b"), (0x40, "w"), (0x80, "l")])
            code = struct.pack(">H", op[0] | (dst << 9) | sz[0] | src)
            name = f"{op[1]}_{sz[1]}_d{src}_d{dst}"

        elif family == "swap_ext":
            d_reg = case_rng.randint(0, 7)
            op = case_rng.choice([(0x4840, "swap"), (0x4880, "ext_w"), (0x48C0, "ext_l")])
            code = struct.pack(">H", op[0] | d_reg)
            name = f"{op[1]}_d{d_reg}"

        elif family == "clr_tst":
            d_reg = case_rng.randint(0, 7)
            is_clr = case_rng.choice([True, False])
            base = 0x4200 if is_clr else 0x4A00
            sz = case_rng.choice([(0x00, "b"), (0x40, "w"), (0x80, "l")])
            code = struct.pack(">H", base | sz[0] | d_reg)
            name = f"{'clr' if is_clr else 'tst'}_{sz[1]}_d{d_reg}"

        elif family == "lea_pea":
            an = case_rng.randint(0, 6)
            disp = case_rng.randint(-512, 512)
            if case_rng.choice([True, False]):
                am = case_rng.randint(0, 6)
                code = struct.pack(">HH", 0x41E8 | (am << 9) | an, disp & 0xFFFF)
                name = f"lea_d16_a{an}_a{am}"
            else:
                code = struct.pack(">HH", 0x4868 | an, disp & 0xFFFF)
                name = f"pea_d16_a{an}"

        elif family == "exg":
            r1 = case_rng.randint(0, 7)
            r2 = case_rng.randint(0, 7)
            code = struct.pack(">H", 0xC140 | (r1 << 9) | r2)
            name = f"exg_d{r1}_d{r2}"

        elif family == "shift":
            src = case_rng.randint(0, 7)
            dst = case_rng.randint(0, 7)
            initial_d[src] = case_rng.choice([0, 1, 2, 4, 8, 16, 31, 32, 33, 64])
            shift_op = case_rng.choice([(0xE1A9, "lsl"), (0xE0A9, "lsr"), (0xE1A1, "asl"), (0xE0A1, "asr")])
            code = struct.pack(">H", (shift_op[0] & 0xF1F8) | (src << 9) | dst)
            name = f"{shift_op[1]}_reg_d{src}_d{dst}"

        elif family == "cmp":
            src = case_rng.randint(0, 7)
            dst = case_rng.randint(0, 7)
            sz = case_rng.choice([(0x00, "b"), (0x40, "w"), (0x80, "l")])
            code = struct.pack(">H", 0xB000 | (dst << 9) | sz[0] | src)
            name = f"cmp_{sz[1]}_d{src}_d{dst}"

        else:  # bit_op
            d_reg = case_rng.randint(0, 7)
            bit_num = case_rng.randint(0, 31)
            op = case_rng.choice([(0x0800, "btst"), (0x08C0, "bset"), (0x0880, "bclr"), (0x0840, "bchg")])
            code = struct.pack(">HH", op[0] | d_reg, bit_num)
            name = f"{op[1]}_imm_{bit_num}_d{d_reg}"

        cases.append(RawTestCase(
            id=start_id + i,
            name=name,
            code_bytes=code,
            initial_d=initial_d,
            initial_a=initial_a,
            initial_sr=initial_sr,
            initial_mem=initial_mem,
            is_boundary=False,
            boundary_kind="randomized",
            seed=case_seed,
        ))

    return cases


def load_external_instructions(json_path: Path | str, start_id: int) -> list[RawTestCase]:
    """Ingest external instruction cases from parent discovery JSON."""
    path = Path(json_path)
    data = json.loads(path.read_text())
    cases: list[RawTestCase] = []
    case_id = start_id

    for item in data:
        pc = item.get("pc", TEST_PC)
        raw_hex = item.get("bytes", "")
        code_bytes = bytes.fromhex(raw_hex)
        name = item.get("name", f"ext_insn_{pc:06x}")
        cases.append(RawTestCase(
            id=case_id,
            name=name,
            code_bytes=code_bytes,
            initial_pc=pc,
            initial_d=[0] * 8,
            initial_a=list(SAFE_A_REGS),
            initial_sr=0x0000,
            is_boundary=False,
            boundary_kind="external_rom",
        ))
        case_id += 1

    return cases
