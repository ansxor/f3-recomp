"""Broad instruction-specific 68EC020-to-C lowering from Capstone CsInsn.

Emits literal decoded C statements for 68EC020 instructions to execute natively
against <f3rt/cpu_abi.h> and "recomp/cpu_ops.h".
"""
from __future__ import annotations

import capstone.m68k as m68k
from capstone import CsInsn
from .timing import instruction_cycles

# Condition code map for Bcc, DBcc, Scc
COND_MAP = {
    't': 0, 'f': 1, 'ra': 1,
    'hi': 2, 'ls': 3,
    'cc': 4, 'hs': 4,
    'cs': 5, 'lo': 5,
    'ne': 6, 'eq': 7,
    'vc': 8, 'vs': 9,
    'pl': 10, 'mi': 11,
    'ge': 12, 'lt': 13,
    'gt': 14, 'le': 15,
}

# Bit masks for size
MASK_MAP = {1: 0xff, 2: 0xffff, 4: 0xffffffff}
SIGN_MAP = {1: 0x80, 2: 0x8000, 4: 0x80000000}


def _get_base_mnemonic(insn: CsInsn) -> str:
    """Extract base mnemonic without size suffix."""
    return insn.mnemonic.split('.')[0].lower()


def _get_size(insn: CsInsn, default_size: int = 4) -> int:
    """Extract operand size: 1 (byte), 2 (word), 4 (long)."""
    try:
        if insn.op_size.size in (1, 2, 4):
            return insn.op_size.size
    except Exception:
        pass
    mnem = insn.mnemonic.lower()
    if mnem.endswith('.b'):
        return 1
    elif mnem.endswith('.w'):
        return 2
    elif mnem.endswith('.l'):
        return 4
    return default_size

class _EA:
    """Decoded effective address operand."""
    def __init__(self):
        self.is_reg = False
        self.reg_type = None  # 'd', 'a', 'ccr', 'sr', 'usp'
        self.reg_num = 0
        self.is_imm = False
        self.imm_val = 0
        self.is_mem = False
        self.ea_setup: list[str] = []
        self.ea_expr = ""
        self.post_step: list[str] = []
        self.read_stmts: list[str] = []
        self.val_expr = ""
        self.write_low_first = False


def _decode_index_expr(mem) -> str:
    """Decode index register with scaling and sign extension."""
    idx_reg = mem.index_reg
    scale = mem.scale if mem.scale > 0 else 1
    if not idx_reg:
        return "0"
    if m68k.M68K_REG_D0 <= idx_reg <= m68k.M68K_REG_D7:
        reg_s = f"cpu->d[{idx_reg - m68k.M68K_REG_D0}]"
    elif m68k.M68K_REG_A0 <= idx_reg <= m68k.M68K_REG_A7:
        reg_s = f"cpu->a[{idx_reg - m68k.M68K_REG_A0}]"
    else:
        return "0"

    if mem.index_size == 0:  # word
        cast = f"(uint32_t)(int32_t)(int16_t)({reg_s} & 0xffffu)"
    else:  # long, modulo-2^32 address arithmetic, never signed overflow
        cast = f"(uint32_t){reg_s}"

    if scale > 1:
        return f"({cast} * {scale}u)"
    return cast


def _pc_base(insn: CsInsn) -> int:
    """PC-relative EA base is its extension word, after any operand prefix."""
    opcode = int.from_bytes(insn.bytes[:2], "big")
    mnem = _get_base_mnemonic(insn)
    if mnem.startswith("bf") or mnem in ("cmp2", "chk2"):
        return insn.address + 4
    if mnem == "movem" or (mnem in ("mulu", "muls", "divu", "divs", "divul", "divsl") and _get_size(insn) == 4):
        return insn.address + 4
    if opcode & 0xff00 == 0x0800:
        return insn.address + 4
    if mnem in ("ori", "andi", "subi", "addi", "eori", "cmpi"):
        return insn.address + 2 + max(2, _get_size(insn))
    return insn.address + 2


def _base_expr(insn: CsInsn, mem) -> str:
    # Capstone 5 reports full-format PC-relative operands as AREGI_INDEX with
    # base_reg=PC. Do not turn this into the nonexistent address register A16.
    if mem.base_reg == m68k.M68K_REG_PC:
        return f"0x{_pc_base(insn):08x}u"
    if m68k.M68K_REG_A0 <= mem.base_reg <= m68k.M68K_REG_A7:
        return f"cpu->a[{mem.base_reg - m68k.M68K_REG_A0}]"
    return "0u"  # Full extension base-suppression bit.


def _ea_extension_bytes(raw: bytes, offset: int, mode: int, reg: int, size: int) -> int:
    if mode <= 4:
        return 0
    if mode == 5 or (mode == 7 and reg in (0, 2)):
        return 2
    if mode == 7 and reg == 1:
        return 4
    if mode == 7 and reg == 4:
        return max(2, size)
    if mode == 6 or (mode == 7 and reg == 3):
        ext = int.from_bytes(raw[offset:offset + 2], "big")
        if not ext & 0x100:
            return 2
        return 2 + {0: 0, 1: 0, 2: 2, 3: 4}[(ext >> 4) & 3] + \
            {0: 0, 1: 0, 2: 2, 3: 4}[ext & 3]
    return 0


def _full_ea(insn: CsInsn, op, size: int, prefix: str) -> tuple[list[str], str] | None:
    # Capstone exposes full-format word displacements as unsigned values.
    # Decode the extension layout to retain their widths and suppression bits.
    raw = bytes(insn.bytes)
    offset = _pc_base(insn) - insn.address
    if _get_base_mnemonic(insn) == "move" and prefix == "dst":
        opcode = int.from_bytes(raw[:2], "big")
        offset += _ea_extension_bytes(raw, offset, (opcode >> 3) & 7, opcode & 7, size)
    if offset + 2 > len(raw):
        return None
    ext = int.from_bytes(raw[offset:offset + 2], "big")
    reg = f"cpu->{'a' if ext & 0x8000 else 'd'}[{(ext >> 12) & 7}]"
    idx = reg if ext & 0x0800 else f"(uint32_t)(int32_t)(int16_t){reg}"
    idx = f"({idx} * {1 << ((ext >> 9) & 3)}u)"
    address = f"ea_{prefix}"
    if not ext & 0x100:
        # Capstone labels brief extensions with zero displacement BASE_DISP.
        disp = int.from_bytes(raw[offset + 1:offset + 2], "big", signed=True)
        expression = f"{_base_expr(insn, op.mem)} + 0x{disp & 0xffffffff:x}u + {idx}"
        return [f"uint32_t {address} = {expression};"], address
    bd_size = {1: 0, 2: 2, 3: 4}.get((ext >> 4) & 3)
    indirect = ext & 7
    if ext & 8 or bd_size is None or indirect == 4:
        return None
    od_size = {0: 0, 1: 0, 2: 2, 3: 4}[indirect & 3]
    if offset + 2 + bd_size + od_size > len(raw):
        return None
    bd = int.from_bytes(raw[offset + 2:offset + 2 + bd_size], "big", signed=True)
    od = int.from_bytes(raw[offset + 2 + bd_size:offset + 2 + bd_size + od_size], "big", signed=True)
    base = "0u" if ext & 0x80 else _base_expr(insn, op.mem)
    idx = "0u" if ext & 0x40 else idx
    if indirect == 0:
        expression = f"{base} + 0x{bd & 0xffffffff:x}u + {idx}"
    elif indirect < 4:
        expression = f"f3_read32(cpu, {base} + 0x{bd & 0xffffffff:x}u + {idx}) + 0x{od & 0xffffffff:x}u"
    else:
        expression = f"f3_read32(cpu, {base} + 0x{bd & 0xffffffff:x}u) + {idx} + 0x{od & 0xffffffff:x}u"
    return [f"uint32_t {address} = {expression};"], address


def _decode_ea(insn: CsInsn, op, size: int, var_prefix: str, post_inc_on_read: bool = False) -> _EA | None:
    """Decode a Capstone operand into an _EA helper."""
    ea = _EA()
    am = op.address_mode

    # Register direct
    if op.type == m68k.M68K_OP_REG:
        reg = op.reg
        if m68k.M68K_REG_D0 <= reg <= m68k.M68K_REG_D7:
            ea.is_reg = True
            ea.reg_type = 'd'
            ea.reg_num = reg - m68k.M68K_REG_D0
            if size == 1:
                ea.val_expr = f"(uint8_t)(cpu->d[{ea.reg_num}] & 0xffu)"
            elif size == 2:
                ea.val_expr = f"(uint16_t)(cpu->d[{ea.reg_num}] & 0xffffu)"
            else:
                ea.val_expr = f"cpu->d[{ea.reg_num}]"
            return ea
        elif m68k.M68K_REG_A0 <= reg <= m68k.M68K_REG_A7:
            ea.is_reg = True
            ea.reg_type = 'a'
            ea.reg_num = reg - m68k.M68K_REG_A0
            if size == 2:
                ea.val_expr = f"(uint16_t)(cpu->a[{ea.reg_num}] & 0xffffu)"
            else:
                ea.val_expr = f"cpu->a[{ea.reg_num}]"
            return ea
        elif reg == m68k.M68K_REG_CCR:
            ea.is_reg = True
            ea.reg_type = 'ccr'
            ea.val_expr = "(cpu->sr & 0x1fu)"
            return ea
        elif reg == m68k.M68K_REG_SR:
            ea.is_reg = True
            ea.reg_type = 'sr'
            ea.val_expr = "cpu->sr"
            return ea
        elif reg == m68k.M68K_REG_USP:
            ea.is_reg = True
            ea.reg_type = 'usp'
            ea.val_expr = "cpu->usp"
            return ea
        return None

    # Immediate
    if op.type == m68k.M68K_OP_IMM or am == m68k.M68K_AM_IMMEDIATE:
        ea.is_imm = True
        mask = MASK_MAP.get(size, 0xffffffff)
        ea.imm_val = op.imm & mask
        ea.val_expr = f"0x{ea.imm_val:x}u"
        return ea

    # Memory modes
    ea.is_mem = True
    addr_var = f"ea_{var_prefix}"

    if am == m68k.M68K_AM_REGI_ADDR:
        reg_num = op.reg - m68k.M68K_REG_A0
        ea.ea_expr = f"cpu->a[{reg_num}]"

    elif am == m68k.M68K_AM_REGI_ADDR_POST_INC:
        reg_num = op.reg - m68k.M68K_REG_A0
        step = 2 if (reg_num == 7 and size == 1) else size
        ea.ea_setup.append(f"uint32_t {addr_var} = cpu->a[{reg_num}];")
        ea.ea_expr = addr_var
        ea.post_step.append(f"cpu->a[{reg_num}] += {step}u;")

    elif am == m68k.M68K_AM_REGI_ADDR_PRE_DEC:
        reg_num = op.reg - m68k.M68K_REG_A0
        step = 2 if (reg_num == 7 and size == 1) else size
        ea.write_low_first = size == 4 and _get_base_mnemonic(insn) == "move"
        ea.ea_setup.append(f"cpu->a[{reg_num}] -= {step}u;")
        ea.ea_expr = f"cpu->a[{reg_num}]"

    elif am == m68k.M68K_AM_REGI_ADDR_DISP:
        reg_num = op.mem.base_reg - m68k.M68K_REG_A0
        disp = op.mem.disp
        ea.ea_setup.append(f"uint32_t {addr_var} = (uint32_t)(cpu->a[{reg_num}] + (int32_t)({disp}));")
        ea.ea_expr = addr_var

    elif am == m68k.M68K_AM_AREGI_INDEX_8_BIT_DISP:
        base_s = _base_expr(insn, op.mem)
        idx_expr = _decode_index_expr(op.mem)
        disp = op.mem.disp
        ea.ea_setup.append(f"uint32_t {addr_var} = (uint32_t)({base_s} + {idx_expr} + (int32_t)({disp}));")
        ea.ea_expr = addr_var

    elif am in (m68k.M68K_AM_AREGI_INDEX_BASE_DISP,
                m68k.M68K_AM_MEMI_POST_INDEX, m68k.M68K_AM_MEMI_PRE_INDEX,
                m68k.M68K_AM_PCI_INDEX_BASE_DISP,
                m68k.M68K_AM_PC_MEMI_POST_INDEX, m68k.M68K_AM_PC_MEMI_PRE_INDEX):
        decoded = _full_ea(insn, op, size, var_prefix)
        if decoded is None:
            return None
        ea.ea_setup, ea.ea_expr = decoded

    elif am == m68k.M68K_AM_PCI_DISP:
        pc_base = _pc_base(insn)
        ea.ea_setup.append(f"uint32_t {addr_var} = (uint32_t)(0x{pc_base:08x}u + (int32_t)({op.mem.disp}));")
        ea.ea_expr = addr_var

    elif am == m68k.M68K_AM_PCI_INDEX_8_BIT_DISP:
        pc_base = _pc_base(insn)
        idx_expr = _decode_index_expr(op.mem)
        ea.ea_setup.append(f"uint32_t {addr_var} = (uint32_t)(0x{pc_base:08x}u + {idx_expr} + (int32_t)({op.mem.disp}));")
        ea.ea_expr = addr_var

    elif am == m68k.M68K_AM_ABSOLUTE_DATA_SHORT:
        raw = op.imm & 0xffff
        s16 = raw if (raw < 0x8000) else (raw - 0x10000)
        ea.ea_expr = f"0x{(s16 & 0xffffffff):08x}u"

    elif am == m68k.M68K_AM_ABSOLUTE_DATA_LONG:
        ea.ea_expr = f"0x{(op.imm & 0xffffffff):08x}u"

    else:
        return None

    # Setup memory read expression
    val_var = f"val_{var_prefix}"
    if size == 1:
        ea.read_stmts.append(f"uint32_t {val_var} = f3_read8(cpu, {ea.ea_expr});")
    elif size == 2:
        ea.read_stmts.append(f"uint32_t {val_var} = f3_read16(cpu, {ea.ea_expr});")
    else:
        ea.read_stmts.append(f"uint32_t {val_var} = f3_read32(cpu, {ea.ea_expr});")
    ea.val_expr = val_var

    if post_inc_on_read or var_prefix in ("src", "tst", "cmp_dst", "btst"):
        ea.read_stmts.extend(ea.post_step)
        ea.post_step = []

    return ea


def _gen_write(ea: _EA, res_expr: str, size: int) -> list[str]:
    """Generate write statements for an EA destination."""
    stmts = []
    if ea.is_reg:
        if ea.reg_type == 'd':
            if size == 1:
                stmts.append(f"cpu->d[{ea.reg_num}] = (cpu->d[{ea.reg_num}] & 0xffffff00u) | (({res_expr}) & 0xffu);")
            elif size == 2:
                stmts.append(f"cpu->d[{ea.reg_num}] = (cpu->d[{ea.reg_num}] & 0xffff0000u) | (({res_expr}) & 0xffffu);")
            else:
                stmts.append(f"cpu->d[{ea.reg_num}] = ({res_expr});")
        elif ea.reg_type == 'a':
            if size == 2:
                stmts.append(f"cpu->a[{ea.reg_num}] = (uint32_t)(int32_t)(int16_t)({res_expr});")
            else:
                stmts.append(f"cpu->a[{ea.reg_num}] = ({res_expr});")
        elif ea.reg_type == 'ccr':
            stmts.append("f3_cc_flush(cpu);")
            stmts.append(f"cpu->sr = (cpu->sr & 0xff00u) | (({res_expr}) & 0x1fu);")
            stmts.append("cpu->cc_op = F3_CC_OP_NONE;")
        elif ea.reg_type == 'sr':
            stmts.append("f3_cc_flush(cpu);")
            stmts.append(f"f3_set_sr(cpu, (uint16_t)({res_expr}));")
            stmts.append("cpu->cc_op = F3_CC_OP_NONE;")
        elif ea.reg_type == 'usp':
            stmts.append(f"cpu->usp = ({res_expr});")
    elif ea.is_mem:
        if size == 1:
            stmts.append(f"f3_write8(cpu, {ea.ea_expr}, (uint8_t)({res_expr}));")
        elif size == 2:
            stmts.append(f"f3_write16(cpu, {ea.ea_expr}, (uint16_t)({res_expr}));")
        elif ea.write_low_first:
            stmts.append(f"f3_write16(cpu, {ea.ea_expr} + 2u, (uint16_t)({res_expr}));")
            stmts.append(f"f3_write16(cpu, {ea.ea_expr}, (uint16_t)(({res_expr}) >> 16));")
        else:
            stmts.append(f"f3_write32(cpu, {ea.ea_expr}, (uint32_t)({res_expr}));")
        stmts.extend(ea.post_step)
    return stmts


def lower(insn: CsInsn) -> list[str] | None:
    """Lower a single Capstone CsInsn into equivalent C statements.

    Returns None if the instruction is unsupported (signaling fallback to Musashi).
    """
    if getattr(insn, 'id', 0) == 0:
        return None
    try:
        ops = insn.operands
    except Exception:
        return None
    mnem = _get_base_mnemonic(insn)
    size = _get_size(insn)
    next_pc = insn.address + insn.size
    cycles = instruction_cycles(insn, _pc_base, _ea_extension_bytes)

    if mnem == "movec":
        ext = int.from_bytes(insn.bytes[2:4], "big")
        reg = f"cpu->{'a' if ext & 0x8000 else 'd'}[{(ext >> 12) & 7}]"
        controls = {0: "sfc", 1: "dfc", 2: "cacr", 0x800: "usp",
                    0x801: "vbr", 0x802: "caar", 0x803: "msp", 0x804: "ssp"}
        control = ext & 0xfff
        stmts = ["f3_cc_flush(cpu);",
                 f"if (!(cpu->sr & 0x2000u)) {{ f3_exception(cpu, 8, 0x{insn.address:x}u); return; }}"]
        if control not in controls:
            return stmts + [f"f3_exception(cpu, 4, 0x{insn.address:x}u);", "return;"]
        field = f"cpu->{controls[control]}"
        if int.from_bytes(insn.bytes[:2], "big") == 0x4e7a:
            if control in (0x803, 0x804):
                active = "cpu->sr & 0x1000u" if control == 0x803 else "!(cpu->sr & 0x1000u)"
                field = f"(({active}) ? cpu->a[7] : {field})"
            stmts.append(f"{reg} = {field};")
        else:
            value = f"({reg} & {7 if control < 2 else 15}u)" if control <= 2 else reg
            if control in (0x803, 0x804):
                active = "cpu->sr & 0x1000u" if control == 0x803 else "!(cpu->sr & 0x1000u)"
                stmts.append(f"if ({active}) cpu->a[7] = {value}; else {field} = {value};")
            else:
                stmts.append(f"{field} = {value};")
        return stmts + [f"cpu->pc = 0x{next_pc:x}u;", f"cpu->cycles += {cycles};"]

    if mnem == "rte":
        return [
            "f3_cc_flush(cpu);",
            f"if (!(cpu->sr & 0x2000u)) {{ f3_exception(cpu, 8, 0x{insn.address:x}u); return; }}",
            "for (;;) {",
            "    uint32_t sp = cpu->a[7];",
            "    unsigned format = f3_read16(cpu, sp + 6u) >> 12;",
            f"    if (format > 2) {{ f3_exception(cpu, 14, 0x{next_pc:x}u); return; }}",
            "    uint16_t sr = f3_read16(cpu, sp);",
            "    uint32_t pc = format == 1 ? 0u : f3_read32(cpu, sp + 2u);",
            "    cpu->a[7] = sp + (format == 2 ? 12u : 8u);",
            "    f3_set_sr(cpu, sr);",
            "    if (format != 1) { cpu->pc = pc; break; }",
            "}",
            f"cpu->cycles += {cycles};",
            "return;",
        ]

    if mnem in ("bftst", "bfextu", "bfchg", "bfexts", "bfclr", "bfffo", "bfset", "bfins"):
        opcode = int.from_bytes(insn.bytes[:2], "big")
        ext = int.from_bytes(insn.bytes[2:4], "big")
        operation = (opcode >> 8) & 7
        operand = ops[-1] if mnem == "bfins" else ops[0]
        ea = _decode_ea(insn, operand, 1, "bitfield")
        if not ea or (ea.is_reg and ea.reg_type != "d"):
            return None
        offset = f"(int32_t)cpu->d[{(ext >> 6) & 7}]" if ext & 0x800 else str((ext >> 6) & 31)
        width = f"cpu->d[{ext & 7}]" if ext & 0x20 else str(ext & 31)
        location = ea.ea_expr if ea.is_mem else str(ea.reg_num)
        return ea.ea_setup + [
            f"f3_bitfield(cpu, {operation}, {int(ea.is_mem)}, {location}, {offset}, {width}, {(ext >> 12) & 7});",
            f"cpu->pc = 0x{next_pc:x}u;", f"cpu->cycles += {cycles};"]

    if mnem in ("cmp2", "chk2"):
        ext = int.from_bytes(insn.bytes[2:4], "big")
        ea = _decode_ea(insn, ops[0], size, "bounds")
        if not ea or not ea.is_mem:
            return None
        reg = f"cpu->{'a' if ext & 0x8000 else 'd'}[{(ext >> 12) & 7}]"
        cast = f"uint{size * 8}_t" if ext & 0x8000 and size < 4 else f"int{size * 8}_t"
        stmts = ea.ea_setup + [
            f"int32_t value = ({cast}){reg};",
            f"int32_t lower = (int{size * 8}_t)f3_read{size * 8}(cpu, {ea.ea_expr});",
            f"int32_t upper = (int{size * 8}_t)f3_read{size * 8}(cpu, {ea.ea_expr} + {size}u);",
            "f3_cc_flush(cpu);",
            "unsigned outside = value < lower || value > upper;",
            "cpu->sr = (uint16_t)((cpu->sr & ~5u) | outside | ((value == lower || value == upper) ? 4u : 0u));",
        ]
        if ext & 0x800:
            stmts.append(f"if (outside) {{ f3_exception(cpu, 6, 0x{next_pc:x}u); return; }}")
        return stmts + [f"cpu->pc = 0x{next_pc:x}u;", f"cpu->cycles += {cycles};"]

    # 1. NOP
    if mnem == 'nop':
        return [
            f"cpu->pc = 0x{next_pc:08x}u;",
            f"cpu->cycles += {cycles};",
        ]

    # 2. Branches: BRA, BSR, Bcc
    if mnem == 'bra':
        if not ops or ops[0].type != m68k.M68K_OP_BR_DISP:
            return None
        target = (insn.address + 2 + ops[0].br_disp.disp) & 0xffffffff
        return [
            f"cpu->pc = 0x{target:08x}u;",
            f"cpu->cycles += {cycles};",
        ]

    if mnem == 'bsr':
        if not ops or ops[0].type != m68k.M68K_OP_BR_DISP:
            return None
        target = (insn.address + 2 + ops[0].br_disp.disp) & 0xffffffff
        return [
            "cpu->a[7] -= 4u;",
            f"f3_write32(cpu, cpu->a[7], 0x{next_pc:08x}u);",
            f"cpu->pc = 0x{target:08x}u;",
            f"cpu->cycles += {cycles};",
        ]

    if mnem.startswith('b') and mnem[1:] in COND_MAP:
        cond_name = mnem[1:]
        cond_val = COND_MAP[cond_name]
        if not ops or ops[0].type != m68k.M68K_OP_BR_DISP:
            return None
        target = (insn.address + 2 + ops[0].br_disp.disp) & 0xffffffff
        return [
            f"if (f3_eval_cond(cpu, {cond_val})) {{ cpu->pc = 0x{target:08x}u; }} else {{ cpu->pc = 0x{next_pc:08x}u; }}",
            f"cpu->cycles += {cycles};",
        ]

    # 3. DBcc
    if mnem.startswith('db'):
        cond_name = mnem[2:]
        if cond_name not in COND_MAP or len(ops) < 2:
            return None
        cond_val = COND_MAP[cond_name]
        if ops[0].type != m68k.M68K_OP_REG or ops[1].type != m68k.M68K_OP_BR_DISP:
            return None
        reg_num = ops[0].reg - m68k.M68K_REG_D0
        target = (insn.address + 2 + ops[1].br_disp.disp) & 0xffffffff
        return [
            f"if (!f3_eval_cond(cpu, {cond_val})) {{",
            f"    int16_t cnt = (int16_t)(cpu->d[{reg_num}] & 0xffffu) - 1;",
            f"    cpu->d[{reg_num}] = (cpu->d[{reg_num}] & 0xffff0000u) | ((uint16_t)cnt & 0xffffu);",
            f"    if (cnt != -1) {{ cpu->pc = 0x{target:08x}u; }} else {{ cpu->pc = 0x{next_pc:08x}u; cpu->cycles += 4u; }}",
            "} else {",
            f"    cpu->pc = 0x{next_pc:08x}u;",
            "}",
            f"cpu->cycles += {cycles};",
        ]

    # 4. Scc
    if mnem.startswith('s') and (mnem[1:] in COND_MAP or mnem in ('st', 'sf')):
        cond_name = mnem[1:] if mnem not in ('st', 'sf') else ('t' if mnem == 'st' else 'f')
        cond_val = COND_MAP[cond_name]
        if not ops:
            return None
        dst_ea = _decode_ea(insn, ops[0], 1, "dst")
        if not dst_ea:
            return None
        stmts = list(dst_ea.ea_setup)
        if cond_name == 't':
            val_s = "0xffu"
        elif cond_name == 'f':
            val_s = "0x00u"
        else:
            val_s = f"(f3_eval_cond(cpu, {cond_val}) ? 0xffu : 0x00u)"
        stmts.extend(_gen_write(dst_ea, val_s, 1))
        stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
        stmts.append(f"cpu->cycles += {cycles};")
        return stmts

    # 5. JMP & JSR
    if mnem == 'jmp':
        if not ops:
            return None
        target_ea = _decode_ea(insn, ops[0], 4, "jmp")
        if not target_ea or not target_ea.is_mem:
            return None
        stmts = list(target_ea.ea_setup)
        stmts.append(f"cpu->pc = {target_ea.ea_expr};")
        stmts.append(f"cpu->cycles += {cycles};")
        return stmts

    if mnem == 'jsr':
        if not ops:
            return None
        target_ea = _decode_ea(insn, ops[0], 4, "jsr")
        if not target_ea or not target_ea.is_mem:
            return None
        stmts = list(target_ea.ea_setup)
        stmts.append(f"uint32_t jsr_target = {target_ea.ea_expr};")
        stmts.append("cpu->a[7] -= 4u;")
        stmts.append(f"f3_write32(cpu, cpu->a[7], 0x{next_pc:08x}u);")
        stmts.append("cpu->pc = jsr_target;")
        stmts.append(f"cpu->cycles += {cycles};")
        return stmts

    # 6. RTS, RTR, RTD
    if mnem == 'rts':
        return [
            "cpu->pc = f3_read32(cpu, cpu->a[7]);",
            "cpu->a[7] += 4u;",
            f"cpu->cycles += {cycles};",
            "f3_cc_flush(cpu);",
            "return;",
        ]

    if mnem == 'rtd':
        disp = ops[0].imm if ops else 0
        return [
            "cpu->pc = f3_read32(cpu, cpu->a[7]);",
            f"cpu->a[7] = (uint32_t)(cpu->a[7] + 4u + (int32_t)(int16_t)({disp}));",
            f"cpu->cycles += {cycles};",
            "f3_cc_flush(cpu);",
            "return;",
        ]
    if mnem == 'rtr':
        return [
            "f3_cc_flush(cpu);",
            "uint16_t rtr_ccr = f3_read16(cpu, cpu->a[7]);",
            "cpu->a[7] += 2u;",
            "cpu->sr = (cpu->sr & 0xff00u) | (rtr_ccr & 0x1fu);",
            "cpu->cc_op = F3_CC_OP_NONE;",
            "cpu->pc = f3_read32(cpu, cpu->a[7]);",
            "cpu->a[7] += 4u;",
            f"cpu->cycles += {cycles};",
            "return;",
        ]

    # 7. LEA & PEA
    if mnem == 'lea':
        if len(ops) < 2:
            return None
        src_ea = _decode_ea(insn, ops[0], 4, "lea")
        if not src_ea or not src_ea.is_mem or ops[1].type != m68k.M68K_OP_REG:
            return None
        dst_reg = ops[1].reg - m68k.M68K_REG_A0
        stmts = list(src_ea.ea_setup)
        stmts.append(f"cpu->a[{dst_reg}] = {src_ea.ea_expr};")
        stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
        stmts.append(f"cpu->cycles += {cycles};")
        return stmts

    if mnem == 'pea':
        if not ops:
            return None
        src_ea = _decode_ea(insn, ops[0], 4, "pea")
        if not src_ea or not src_ea.is_mem:
            return None
        stmts = list(src_ea.ea_setup)
        stmts.append(f"uint32_t pea_target = {src_ea.ea_expr};")
        stmts.append("cpu->a[7] -= 4u;")
        stmts.append("f3_write32(cpu, cpu->a[7], pea_target);")
        stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
        stmts.append(f"cpu->cycles += {cycles};")
        return stmts

    # 8. LINK & UNLK
    if mnem == 'link':
        if len(ops) < 2 or ops[0].type != m68k.M68K_OP_REG or ops[1].type != m68k.M68K_OP_IMM:
            return None
        reg_num = ops[0].reg - m68k.M68K_REG_A0
        disp = ops[1].imm
        cast_disp = "(int32_t)(int16_t)" if size == 2 else "(int32_t)"
        return [
            "cpu->a[7] -= 4u;",
            f"f3_write32(cpu, cpu->a[7], cpu->a[{reg_num}]);",
            f"cpu->a[{reg_num}] = cpu->a[7];",
            f"cpu->a[7] = (uint32_t)(cpu->a[7] + {cast_disp}({disp}));",
            f"cpu->pc = 0x{next_pc:08x}u;",
            f"cpu->cycles += {cycles};",
        ]

    if mnem == 'unlk':
        if not ops or ops[0].type != m68k.M68K_OP_REG:
            return None
        reg_num = ops[0].reg - m68k.M68K_REG_A0
        return [
            f"cpu->a[7] = cpu->a[{reg_num}];",
            f"cpu->a[{reg_num}] = f3_read32(cpu, cpu->a[7]);",
            "cpu->a[7] += 4u;" if reg_num != 7 else "(void)cpu;",
            f"cpu->pc = 0x{next_pc:08x}u;",
            f"cpu->cycles += {cycles};",
        ]

    # 9. EXT, EXTB, SWAP
    if mnem in ('ext', 'extb', 'swap'):
        if not ops or ops[0].type != m68k.M68K_OP_REG:
            return None
        reg_num = ops[0].reg - m68k.M68K_REG_D0
        stmts = []
        if mnem == 'ext' and size == 2:
            stmts.append(f"int16_t ext_val = (int8_t)(cpu->d[{reg_num}] & 0xffu);")
            stmts.append(f"cpu->d[{reg_num}] = (cpu->d[{reg_num}] & 0xffff0000u) | ((uint16_t)ext_val & 0xffffu);")
            stmts.append(f"cpu->cc_op = F3_CC_OP_LOGIC; cpu->cc_result = (uint16_t)ext_val; cpu->cc_width = 2;")
        elif mnem == 'ext' and size == 4:
            stmts.append(f"int32_t ext_val = (int16_t)(cpu->d[{reg_num}] & 0xffffu);")
            stmts.append(f"cpu->d[{reg_num}] = (uint32_t)ext_val;")
            stmts.append(f"cpu->cc_op = F3_CC_OP_LOGIC; cpu->cc_result = (uint32_t)ext_val; cpu->cc_width = 4;")
        elif mnem == 'extb':
            stmts.append(f"int32_t ext_val = (int8_t)(cpu->d[{reg_num}] & 0xffu);")
            stmts.append(f"cpu->d[{reg_num}] = (uint32_t)ext_val;")
            stmts.append(f"cpu->cc_op = F3_CC_OP_LOGIC; cpu->cc_result = (uint32_t)ext_val; cpu->cc_width = 4;")
        elif mnem == 'swap':
            stmts.append(f"cpu->d[{reg_num}] = (cpu->d[{reg_num}] >> 16) | (cpu->d[{reg_num}] << 16);")
            stmts.append(f"cpu->cc_op = F3_CC_OP_LOGIC; cpu->cc_result = cpu->d[{reg_num}]; cpu->cc_width = 4;")
        stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
        stmts.append(f"cpu->cycles += {cycles};")
        return stmts

    # 10. MOVEQ
    if mnem == 'moveq':
        if len(ops) < 2 or ops[0].type != m68k.M68K_OP_IMM or ops[1].type != m68k.M68K_OP_REG:
            return None
        imm8 = ops[0].imm & 0xff
        s32 = imm8 if (imm8 < 0x80) else (imm8 - 0x100)
        dst_reg = ops[1].reg - m68k.M68K_REG_D0
        return [
            f"cpu->d[{dst_reg}] = 0x{(s32 & 0xffffffff):08x}u;",
            f"cpu->cc_op = F3_CC_OP_LOGIC; cpu->cc_result = cpu->d[{dst_reg}]; cpu->cc_width = 4;",
            f"cpu->pc = 0x{next_pc:08x}u;",
            f"cpu->cycles += {cycles};",
        ]

    if mnem == 'exg' and len(ops) == 2:
        left = _decode_ea(insn, ops[0], 4, "left")
        right = _decode_ea(insn, ops[1], 4, "right")
        if not left or not right or left.reg_type not in ('a', 'd') or right.reg_type not in ('a', 'd'):
            return None
        return ["uint32_t exchanged = " + left.val_expr + ";"] + \
            _gen_write(left, right.val_expr, 4) + _gen_write(right, "exchanged", 4) + [
                f"cpu->pc = 0x{next_pc:08x}u;", f"cpu->cycles += {cycles};"]

    # 11. MOVE & MOVEA
    if mnem in ('move', 'movea'):
        if len(ops) < 2:
            return None
        is_movea = (mnem == 'movea') or (ops[1].type == m68k.M68K_OP_REG and m68k.M68K_REG_A0 <= ops[1].reg <= m68k.M68K_REG_A7)
        src_ea = _decode_ea(insn, ops[0], size, "src")
        dst_ea = _decode_ea(insn, ops[1], size, "dst")
        if not src_ea or not dst_ea:
            return None
        special = any(ea.reg_type in ('sr', 'ccr', 'usp') for ea in (src_ea, dst_ea))
        privileged = any(ea.reg_type in ('sr', 'usp') for ea in (src_ea, dst_ea))
        stmts = ["f3_cc_flush(cpu);"] if special else []
        if privileged:
            stmts.append(f"if (!(cpu->sr & 0x2000u)) {{ f3_exception(cpu, 8u, 0x{insn.address:08x}u); return; }}")
        stmts.extend(src_ea.ea_setup)
        stmts.extend(src_ea.read_stmts)
        stmts.append(f"uint32_t move_value = {src_ea.val_expr};")
        val_expr = "move_value"
        if is_movea:
            if size == 2:
                val_expr = f"(uint32_t)(int32_t)(int16_t)({val_expr})"
            stmts.extend(dst_ea.ea_setup)
            stmts.extend(_gen_write(dst_ea, val_expr, 4))
        else:
            stmts.extend(dst_ea.ea_setup)
            stmts.extend(_gen_write(dst_ea, val_expr, size))
            if not special and (not dst_ea.is_reg or dst_ea.reg_type in ('d', None)):
                stmts.append(f"cpu->cc_op = F3_CC_OP_LOGIC; cpu->cc_result = {val_expr}; cpu->cc_width = {size};")
        stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
        stmts.append(f"cpu->cycles += {cycles};")
        return stmts

    # 12. MOVEM
    if mnem == 'movem':
        if len(ops) < 2:
            return None
        movem_size = 2 if (size == 2) else 4
        if ops[0].type == m68k.M68K_OP_REG_BITS:
            # Registers to Memory
            mask = ops[0].register_bits
            ea_op = ops[1]
            stmts = []
            if ea_op.address_mode == m68k.M68K_AM_REGI_ADDR_PRE_DEC:
                base_reg = ea_op.reg - m68k.M68K_REG_A0
                stmts.append(f"uint32_t movem_addr = cpu->a[{base_reg}];")
                # Hardware stores A7..A0, then D7..D0
                for bit in range(15, -1, -1):
                    if (mask >> bit) & 1:
                        stmts.append(f"movem_addr -= {movem_size}u;")
                        val_s = f"cpu->a[{bit - 8}]" if bit >= 8 else f"cpu->d[{bit}]"
                        if movem_size == 2:
                            stmts.append(f"f3_write16(cpu, movem_addr, (uint16_t){val_s});")
                        else:
                            stmts.append(f"f3_write16(cpu, movem_addr + 2u, (uint16_t){val_s});")
                            stmts.append(f"f3_write16(cpu, movem_addr, (uint16_t)({val_s} >> 16));")
                stmts.append(f"cpu->a[{base_reg}] = movem_addr;")
            else:
                mem_ea = _decode_ea(insn, ea_op, movem_size, "movem")
                if not mem_ea or not mem_ea.is_mem:
                    return None
                stmts.extend(mem_ea.ea_setup)
                stmts.append(f"uint32_t movem_addr = {mem_ea.ea_expr};")
                # Hardware stores D0..D7, then A0..A7
                for bit in range(16):
                    if (mask >> bit) & 1:
                        val_s = f"cpu->d[{bit}]" if bit < 8 else f"cpu->a[{bit - 8}]"
                        if movem_size == 2:
                            stmts.append(f"f3_write16(cpu, movem_addr, (uint16_t){val_s});")
                        else:
                            stmts.append(f"f3_write32(cpu, movem_addr, {val_s});")
                        stmts.append(f"movem_addr += {movem_size}u;")
            stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
            stmts.append(f"cpu->cycles += {cycles};")
            return stmts
        elif ops[1].type == m68k.M68K_OP_REG_BITS:
            # Memory to Registers
            mask = ops[1].register_bits
            ea_op = ops[0]
            stmts = []
            if ea_op.address_mode == m68k.M68K_AM_REGI_ADDR_POST_INC:
                base_reg = ea_op.reg - m68k.M68K_REG_A0
                stmts.append(f"uint32_t movem_addr = cpu->a[{base_reg}];")
                for bit in range(16):
                    if (mask >> bit) & 1:
                        if movem_size == 2:
                            cast = "(uint32_t)(int32_t)(int16_t)"
                            val_read = f"{cast}f3_read16(cpu, movem_addr)"
                        else:
                            val_read = "f3_read32(cpu, movem_addr)"
                        dst_reg = f"cpu->d[{bit}]" if bit < 8 else f"cpu->a[{bit - 8}]"
                        stmts.append(f"{dst_reg} = {val_read};")
                        stmts.append(f"movem_addr += {movem_size}u;")
                stmts.append(f"cpu->a[{base_reg}] = movem_addr;")
            else:
                mem_ea = _decode_ea(insn, ea_op, movem_size, "movem")
                if not mem_ea or not mem_ea.is_mem:
                    return None
                stmts.extend(mem_ea.ea_setup)
                stmts.append(f"uint32_t movem_addr = {mem_ea.ea_expr};")
                for bit in range(16):
                    if (mask >> bit) & 1:
                        if movem_size == 2:
                            cast = "(uint32_t)(int32_t)(int16_t)"
                            val_read = f"{cast}f3_read16(cpu, movem_addr)"
                        else:
                            val_read = "f3_read32(cpu, movem_addr)"
                        dst_reg = f"cpu->d[{bit}]" if bit < 8 else f"cpu->a[{bit - 8}]"
                        stmts.append(f"{dst_reg} = {val_read};")
                        stmts.append(f"movem_addr += {movem_size}u;")
            stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
            stmts.append(f"cpu->cycles += {cycles};")
            return stmts
        return None

    # 13. CLR
    if mnem == 'clr':
        if not ops:
            return None
        dst_ea = _decode_ea(insn, ops[0], size, "clr")
        if not dst_ea:
            return None
        stmts = list(dst_ea.ea_setup)
        stmts.extend(_gen_write(dst_ea, "0u", size))
        stmts.append(f"cpu->cc_op = F3_CC_OP_LOGIC; cpu->cc_result = 0u; cpu->cc_width = {size};")
        stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
        stmts.append(f"cpu->cycles += {cycles};")
        return stmts

    # 14. TST / TAS: flags describe the original operand, before setting bit 7.
    if mnem in ('tst', 'tas'):
        if not ops:
            return None
        src_ea = _decode_ea(insn, ops[0], size, "tst")
        if not src_ea:
            return None
        stmts = list(src_ea.ea_setup)
        stmts.extend(src_ea.read_stmts)
        stmts.append(f"cpu->cc_op = F3_CC_OP_LOGIC; cpu->cc_result = {src_ea.val_expr}; cpu->cc_width = {size};")
        if mnem == 'tas':
            stmts.extend(_gen_write(src_ea, f"({src_ea.val_expr} | 0x80u)", 1))
        stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
        stmts.append(f"cpu->cycles += {cycles};")
        return stmts

    # 15. ADD, ADDA, ADDI, ADDQ
    if mnem in ('add', 'adda', 'addi', 'addq'):
        if len(ops) < 2:
            return None
        is_adda = (mnem == 'adda') or (ops[1].type == m68k.M68K_OP_REG and m68k.M68K_REG_A0 <= ops[1].reg <= m68k.M68K_REG_A7)
        if is_adda:
            src_ea = _decode_ea(insn, ops[0], size, "src")
            if not src_ea:
                return None
            dst_reg = ops[1].reg - m68k.M68K_REG_A0
            stmts = list(src_ea.ea_setup)
            stmts.extend(src_ea.read_stmts)
            cast = "(uint32_t)(int32_t)(int16_t)" if size == 2 else "(uint32_t)"
            stmts.append(f"cpu->a[{dst_reg}] = (uint32_t)(cpu->a[{dst_reg}] + {cast}({src_ea.val_expr}));")
            stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
            stmts.append(f"cpu->cycles += {cycles};")
            return stmts
        else:
            src_ea = _decode_ea(insn, ops[0], size, "src")
            dst_ea = _decode_ea(insn, ops[1], size, "dst")
            if not src_ea or not dst_ea:
                return None
            stmts = list(src_ea.ea_setup)
            stmts.extend(src_ea.read_stmts)
            stmts.extend(dst_ea.ea_setup)
            stmts.extend(dst_ea.read_stmts)
            mask_s = f"0x{MASK_MAP[size]:x}u"
            stmts.append(f"uint32_t add_src = {src_ea.val_expr};")
            stmts.append(f"uint32_t add_dst = {dst_ea.val_expr};")
            stmts.append(f"uint32_t add_res = (add_dst + add_src) & {mask_s};")
            stmts.extend(_gen_write(dst_ea, "add_res", size))
            stmts.append(f"cpu->sr = (cpu->sr & ~0x10u) | (((add_res & {mask_s}) < (add_src & {mask_s})) ? 0x10u : 0u);")
            stmts.append(f"cpu->cc_op = F3_CC_OP_ADD; cpu->cc_src = add_src; cpu->cc_dst = add_dst; cpu->cc_result = add_res; cpu->cc_width = {size};")
            stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
            stmts.append(f"cpu->cycles += {cycles};")
            return stmts

    # 16. SUB, SUBA, SUBI, SUBQ
    if mnem in ('sub', 'suba', 'subi', 'subq'):
        if len(ops) < 2:
            return None
        is_suba = (mnem == 'suba') or (ops[1].type == m68k.M68K_OP_REG and m68k.M68K_REG_A0 <= ops[1].reg <= m68k.M68K_REG_A7)
        if is_suba:
            src_ea = _decode_ea(insn, ops[0], size, "src")
            if not src_ea:
                return None
            dst_reg = ops[1].reg - m68k.M68K_REG_A0
            stmts = list(src_ea.ea_setup)
            stmts.extend(src_ea.read_stmts)
            cast = "(uint32_t)(int32_t)(int16_t)" if size == 2 else "(uint32_t)"
            stmts.append(f"cpu->a[{dst_reg}] = (uint32_t)(cpu->a[{dst_reg}] - {cast}({src_ea.val_expr}));")
            stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
            stmts.append(f"cpu->cycles += {cycles};")
            return stmts
        else:
            src_ea = _decode_ea(insn, ops[0], size, "src")
            dst_ea = _decode_ea(insn, ops[1], size, "dst")
            if not src_ea or not dst_ea:
                return None
            stmts = list(src_ea.ea_setup)
            stmts.extend(src_ea.read_stmts)
            stmts.extend(dst_ea.ea_setup)
            stmts.extend(dst_ea.read_stmts)
            mask_s = f"0x{MASK_MAP[size]:x}u"
            stmts.append(f"uint32_t sub_src = {src_ea.val_expr};")
            stmts.append(f"uint32_t sub_dst = {dst_ea.val_expr};")
            stmts.append(f"uint32_t sub_res = (sub_dst - sub_src) & {mask_s};")
            stmts.extend(_gen_write(dst_ea, "sub_res", size))
            stmts.append(f"cpu->sr = (cpu->sr & ~0x10u) | (((sub_dst & {mask_s}) < (sub_src & {mask_s})) ? 0x10u : 0u);")
            stmts.append(f"cpu->cc_op = F3_CC_OP_SUB; cpu->cc_src = sub_src; cpu->cc_dst = sub_dst; cpu->cc_result = sub_res; cpu->cc_width = {size};")
            stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
            stmts.append(f"cpu->cycles += {cycles};")
            return stmts

    # 17. CMP, CMPA, CMPI
    if mnem in ('cmp', 'cmpa', 'cmpi', 'cmpm'):
        if len(ops) < 2:
            return None
        is_cmpa = (mnem == 'cmpa') or (ops[1].type == m68k.M68K_OP_REG and m68k.M68K_REG_A0 <= ops[1].reg <= m68k.M68K_REG_A7)
        if is_cmpa:
            src_ea = _decode_ea(insn, ops[0], size, "src")
            if not src_ea:
                return None
            dst_reg = ops[1].reg - m68k.M68K_REG_A0
            stmts = list(src_ea.ea_setup)
            stmts.extend(src_ea.read_stmts)
            cast = "(uint32_t)(int32_t)(int16_t)" if size == 2 else "(uint32_t)"
            stmts.append(f"uint32_t cmp_src = {cast}({src_ea.val_expr});")
            stmts.append(f"uint32_t cmp_dst = cpu->a[{dst_reg}];")
            stmts.append("uint32_t cmp_res = cmp_dst - cmp_src;")
            stmts.append("cpu->cc_op = F3_CC_OP_CMP; cpu->cc_src = cmp_src; cpu->cc_dst = cmp_dst; cpu->cc_result = cmp_res; cpu->cc_width = 4;")
            stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
            stmts.append(f"cpu->cycles += {cycles};")
            return stmts
        else:
            src_ea = _decode_ea(insn, ops[0], size, "src")
            dst_ea = _decode_ea(insn, ops[1], size, "cmp_dst")
            if not src_ea or not dst_ea:
                return None
            stmts = list(src_ea.ea_setup)
            stmts.extend(src_ea.read_stmts)
            stmts.extend(dst_ea.ea_setup)
            stmts.extend(dst_ea.read_stmts)
            mask_s = f"0x{MASK_MAP[size]:x}u"
            stmts.append(f"uint32_t cmp_src = {src_ea.val_expr};")
            stmts.append(f"uint32_t cmp_dst = {dst_ea.val_expr};")
            stmts.append(f"uint32_t cmp_res = (cmp_dst - cmp_src) & {mask_s};")
            stmts.append(f"cpu->cc_op = F3_CC_OP_CMP; cpu->cc_src = cmp_src; cpu->cc_dst = cmp_dst; cpu->cc_result = cmp_res; cpu->cc_width = {size};")
            stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
            stmts.append(f"cpu->cycles += {cycles};")
            return stmts

    # 18. NEG & NEGX
    if mnem in ('neg', 'negx'):
        if not ops:
            return None
        dst_ea = _decode_ea(insn, ops[0], size, "neg")
        if not dst_ea:
            return None
        stmts = list(dst_ea.ea_setup)
        stmts.extend(dst_ea.read_stmts)
        mask_s = f"0x{MASK_MAP[size]:x}u"
        sign_s = f"0x{SIGN_MAP[size]:x}u"
        if mnem == 'neg':
            stmts.append(f"uint32_t neg_src = {dst_ea.val_expr};")
            stmts.append(f"uint32_t neg_res = (0u - neg_src) & {mask_s};")
            stmts.extend(_gen_write(dst_ea, "neg_res", size))
            stmts.append(f"cpu->sr = (cpu->sr & ~0x10u) | (((neg_src & {mask_s}) != 0u) ? 0x10u : 0u);")
            stmts.append(f"cpu->cc_op = F3_CC_OP_SUB; cpu->cc_src = neg_src; cpu->cc_dst = 0u; cpu->cc_result = neg_res; cpu->cc_width = {size};")
        else:  # negx
            stmts.append("f3_cc_flush(cpu);")
            stmts.append("uint32_t neg_x = (cpu->sr >> 4) & 1u;")
            stmts.append("uint32_t prev_z = (cpu->sr >> 2) & 1u;")
            stmts.append(f"uint32_t neg_src = {dst_ea.val_expr};")
            stmts.append(f"uint32_t neg_res = (0u - neg_src - neg_x) & {mask_s};")
            stmts.extend(_gen_write(dst_ea, "neg_res", size))
            stmts.append(f"int c = ((uint64_t)(neg_src & {mask_s}) + neg_x) > 0;")
            stmts.append(f"int v = ((neg_src & neg_res & {sign_s}) != 0);")
            stmts.append(f"int n = (neg_res & {sign_s}) != 0;")
            stmts.append(f"int z = (neg_res == 0) ? (int)prev_z : 0;")
            stmts.append("cpu->sr = (cpu->sr & ~0x1fu) | (c ? 0x11u : 0u) | (n ? 0x08u : 0u) | (z ? 0x04u : 0u) | (v ? 0x02u : 0u);")
            stmts.append("cpu->cc_op = F3_CC_OP_NONE;")
        stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
        stmts.append(f"cpu->cycles += {cycles};")
        return stmts

    # 19. ADDX & SUBX
    if mnem in ('addx', 'subx'):
        if len(ops) < 2:
            return None
        src_ea = _decode_ea(insn, ops[0], size, "src")
        dst_ea = _decode_ea(insn, ops[1], size, "dst")
        if not src_ea or not dst_ea:
            return None
        stmts = list(src_ea.ea_setup)
        stmts.extend(src_ea.read_stmts)
        stmts.extend(dst_ea.ea_setup)
        stmts.extend(dst_ea.read_stmts)
        mask_s = f"0x{MASK_MAP[size]:x}u"
        sign_s = f"0x{SIGN_MAP[size]:x}u"
        stmts.append("f3_cc_flush(cpu);")
        stmts.append("uint32_t ext_x = (cpu->sr >> 4) & 1u;")
        stmts.append("uint32_t prev_z = (cpu->sr >> 2) & 1u;")
        stmts.append(f"uint32_t op_src = {src_ea.val_expr};")
        stmts.append(f"uint32_t op_dst = {dst_ea.val_expr};")
        if mnem == 'addx':
            stmts.append(f"uint64_t full = (uint64_t)(op_dst & {mask_s}) + (uint64_t)(op_src & {mask_s}) + ext_x;")
            stmts.append(f"uint32_t op_res = (uint32_t)full & {mask_s};")
            stmts.extend(_gen_write(dst_ea, "op_res", size))
            stmts.append(f"int c = full > {mask_s};")
            stmts.append(f"int v = ((op_src ^ op_res) & (op_dst ^ op_res) & {sign_s}) != 0;")
        else:  # subx
            stmts.append(f"int c = (op_dst & {mask_s}) < ((uint64_t)(op_src & {mask_s}) + ext_x);")
            stmts.append(f"uint32_t op_res = (op_dst - op_src - ext_x) & {mask_s};")
            stmts.extend(_gen_write(dst_ea, "op_res", size))
            stmts.append(f"int v = ((op_dst ^ op_src) & (op_dst ^ op_res) & {sign_s}) != 0;")
        stmts.append(f"int n = (op_res & {sign_s}) != 0;")
        stmts.append("int z = (op_res == 0) ? (int)prev_z : 0;")
        stmts.append("cpu->sr = (cpu->sr & ~0x1fu) | (c ? 0x11u : 0u) | (n ? 0x08u : 0u) | (z ? 0x04u : 0u) | (v ? 0x02u : 0u);")
        stmts.append("cpu->cc_op = F3_CC_OP_NONE;")
        stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
        stmts.append(f"cpu->cycles += {cycles};")
        return stmts

    # 20. AND, ANDI, OR, ORI, EOR, EORI, NOT
    if mnem in ('and', 'andi', 'or', 'ori', 'eor', 'eori', 'not'):
        if mnem == 'not':
            if not ops:
                return None
            dst_ea = _decode_ea(insn, ops[0], size, "not")
            if not dst_ea:
                return None
            stmts = list(dst_ea.ea_setup)
            stmts.extend(dst_ea.read_stmts)
            mask_s = f"0x{MASK_MAP[size]:x}u"
            stmts.append(f"uint32_t not_res = (~{dst_ea.val_expr}) & {mask_s};")
            stmts.extend(_gen_write(dst_ea, "not_res", size))
            stmts.append(f"cpu->cc_op = F3_CC_OP_LOGIC; cpu->cc_result = not_res; cpu->cc_width = {size};")
            stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
            stmts.append(f"cpu->cycles += {cycles};")
            return stmts
        else:
            if len(ops) < 2:
                return None
            op_sym = '&' if 'and' in mnem else ('|' if 'or' in mnem and 'eor' not in mnem else '^')
            # Check CCR / SR destination
            if ops[1].type == m68k.M68K_OP_REG and ops[1].reg in (m68k.M68K_REG_CCR, m68k.M68K_REG_SR):
                imm_val = ops[0].imm
                stmts = ["f3_cc_flush(cpu);"]
                if ops[1].reg == m68k.M68K_REG_SR:
                    stmts.append(f"if (!(cpu->sr & 0x2000u)) {{ f3_exception(cpu, 8u, 0x{insn.address:08x}u); return; }}")
                if ops[1].reg == m68k.M68K_REG_CCR:
                    stmts.append(f"cpu->sr = (cpu->sr & 0xff00u) | ((cpu->sr {op_sym} 0x{imm_val:x}u) & 0x1fu);")
                else:
                    stmts.append(f"f3_set_sr(cpu, (uint16_t)(cpu->sr {op_sym} 0x{imm_val:x}u));")
                stmts.append("cpu->cc_op = F3_CC_OP_NONE;")
                stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
                stmts.append(f"cpu->cycles += {cycles};")
                return stmts

            src_ea = _decode_ea(insn, ops[0], size, "src")
            dst_ea = _decode_ea(insn, ops[1], size, "dst")
            if not src_ea or not dst_ea:
                return None
            stmts = list(src_ea.ea_setup)
            stmts.extend(src_ea.read_stmts)
            stmts.extend(dst_ea.ea_setup)
            stmts.extend(dst_ea.read_stmts)
            mask_s = f"0x{MASK_MAP[size]:x}u"
            stmts.append(f"uint32_t log_res = ({dst_ea.val_expr} {op_sym} {src_ea.val_expr}) & {mask_s};")
            stmts.extend(_gen_write(dst_ea, "log_res", size))
            stmts.append(f"cpu->cc_op = F3_CC_OP_LOGIC; cpu->cc_result = log_res; cpu->cc_width = {size};")
            stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
            stmts.append(f"cpu->cycles += {cycles};")
            return stmts

    # 21. Bit Operations: BTST, BSET, BCLR, BCHG
    if mnem in ('btst', 'bset', 'bclr', 'bchg'):
        if len(ops) < 2:
            return None
        is_dest_reg = (ops[1].type == m68k.M68K_OP_REG and m68k.M68K_REG_D0 <= ops[1].reg <= m68k.M68K_REG_D7)
        bit_size = 4 if is_dest_reg else 1
        if ops[0].type == m68k.M68K_OP_IMM:
            bit_s = f"0x{ops[0].imm:x}u"
            setup_bit = []
        elif ops[0].type == m68k.M68K_OP_REG and m68k.M68K_REG_D0 <= ops[0].reg <= m68k.M68K_REG_D7:
            bit_s = f"cpu->d[{ops[0].reg - m68k.M68K_REG_D0}]"
            setup_bit = []
        else:
            return None

        bit_prefix = "btst" if mnem == 'btst' else "bit"
        dst_ea = _decode_ea(insn, ops[1], bit_size, bit_prefix)
        if not dst_ea:
            return None
        stmts = list(setup_bit)
        stmts.extend(dst_ea.ea_setup)
        stmts.extend(dst_ea.read_stmts)
        fn_name = f"f3_{mnem}"
        if mnem == 'btst':
            stmts.append(f"{fn_name}(cpu, {dst_ea.val_expr}, {bit_s}, {bit_size});")
        else:
            stmts.append(f"uint32_t bit_res = {fn_name}(cpu, {dst_ea.val_expr}, {bit_s}, {bit_size});")
            stmts.extend(_gen_write(dst_ea, "bit_res", bit_size))
        stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
        stmts.append(f"cpu->cycles += {cycles};")
        return stmts

    # 22. Shifts & Rotates: ASL, ASR, LSL, LSR, ROL, ROR, ROXL, ROXR
    if mnem in ('asl', 'asr', 'lsl', 'lsr', 'rol', 'ror', 'roxl', 'roxr'):
        if not ops:
            return None
        fn = f"f3_{mnem}"
        if len(ops) == 1:
            # Memory shift (always word, count 1)
            dst_ea = _decode_ea(insn, ops[0], 2, "shift")
            if not dst_ea or not dst_ea.is_mem:
                return None
            stmts = list(dst_ea.ea_setup)
            stmts.extend(dst_ea.read_stmts)
            stmts.append(f"uint32_t shift_res = {fn}(cpu, {dst_ea.val_expr}, 1u, 2);")
            stmts.extend(_gen_write(dst_ea, "shift_res", 2))
            stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
            stmts.append(f"cpu->cycles += {cycles};")
            return stmts
        else:
            # Register shift
            if ops[0].type == m68k.M68K_OP_IMM:
                cnt_s = f"{ops[0].imm}u"
            elif ops[0].type == m68k.M68K_OP_REG and m68k.M68K_REG_D0 <= ops[0].reg <= m68k.M68K_REG_D7:
                cnt_s = f"cpu->d[{ops[0].reg - m68k.M68K_REG_D0}]"
            else:
                return None
            dst_ea = _decode_ea(insn, ops[1], size, "shift")
            if not dst_ea or not dst_ea.is_reg or dst_ea.reg_type != 'd':
                return None
            stmts = list(dst_ea.read_stmts)
            stmts.append(f"uint32_t shift_res = {fn}(cpu, {dst_ea.val_expr}, {cnt_s}, {size});")
            stmts.extend(_gen_write(dst_ea, "shift_res", size))
            stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
            stmts.append(f"cpu->cycles += {cycles};")
            return stmts

    # 23. Multiply: MULU, MULS
    if mnem in ('mulu', 'muls'):
        if len(ops) < 2:
            return None
        is_signed = (mnem == 'muls')
        fn = f"f3_{mnem}_w" if size == 2 else f"f3_{mnem}_l"
        src_ea = _decode_ea(insn, ops[0], size, "src")
        if not src_ea:
            return None
        stmts = list(src_ea.ea_setup)
        stmts.extend(src_ea.read_stmts)

        if size == 2:
            if ops[1].type != m68k.M68K_OP_REG or not (m68k.M68K_REG_D0 <= ops[1].reg <= m68k.M68K_REG_D7):
                return None
            dst_reg = ops[1].reg - m68k.M68K_REG_D0
            cast = "int16_t" if is_signed else "uint16_t"
            stmts.append(f"cpu->d[{dst_reg}] = {fn}(cpu, ({cast})({src_ea.val_expr}), ({cast})(cpu->d[{dst_reg}] & 0xffffu));")
        else:  # size == 4
            cast = "int32_t" if is_signed else "uint32_t"
            if ops[1].type == m68k.M68K_OP_REG_PAIR:
                reg_hi = ops[1].reg_pair.reg_0 - m68k.M68K_REG_D0
                reg_lo = ops[1].reg_pair.reg_1 - m68k.M68K_REG_D0
                stmts.append("uint32_t prod_hi = 0;")
                stmts.append(f"uint32_t prod_lo = {fn}(cpu, ({cast})({src_ea.val_expr}), ({cast})cpu->d[{reg_lo}], &prod_hi);")
                stmts.append(f"cpu->d[{reg_hi}] = prod_hi;")
                stmts.append(f"cpu->d[{reg_lo}] = prod_lo;")
            elif ops[1].type == m68k.M68K_OP_REG and m68k.M68K_REG_D0 <= ops[1].reg <= m68k.M68K_REG_D7:
                dst_reg = ops[1].reg - m68k.M68K_REG_D0
                stmts.append(f"cpu->d[{dst_reg}] = {fn}(cpu, ({cast})({src_ea.val_expr}), ({cast})cpu->d[{dst_reg}], NULL);")
            else:
                return None
        stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
        stmts.append(f"cpu->cycles += {cycles};")
        return stmts

    # 24. Divide: DIVU, DIVS
    if mnem in ('divu', 'divs'):
        if len(ops) < 2:
            return None
        is_signed = (mnem == 'divs')
        fn = f"f3_{mnem}_w" if size == 2 else f"f3_{mnem}_l"
        src_ea = _decode_ea(insn, ops[0], size, "src")
        if not src_ea:
            return None
        stmts = list(src_ea.ea_setup)
        stmts.extend(src_ea.read_stmts)
        stmts.append("int div_ex = 0;")

        if size == 2:
            if ops[1].type != m68k.M68K_OP_REG or not (m68k.M68K_REG_D0 <= ops[1].reg <= m68k.M68K_REG_D7):
                return None
            dst_reg = ops[1].reg - m68k.M68K_REG_D0
            cast = "int16_t" if is_signed else "uint16_t"
            stmts.append(f"cpu->d[{dst_reg}] = {fn}(cpu, ({cast})({src_ea.val_expr}), cpu->d[{dst_reg}], 0x{next_pc:08x}u, &div_ex);")
            stmts.append("if (div_ex) return;")
        else:  # Capstone omits Dr in the 32/32 remainder form; decode extension.
            extension = int.from_bytes(insn.bytes[2:4], "big")
            reg_quot = (extension >> 12) & 7
            reg_rem = extension & 7
            is_64 = int(bool(extension & 0x0400))
            cast = "int32_t" if is_signed else "uint32_t"
            stmts.append(f"uint32_t div_rem = cpu->d[{reg_rem}];")
            stmts.append(f"uint32_t div_quot = {fn}(cpu, ({cast})({src_ea.val_expr}), cpu->d[{reg_quot}], cpu->d[{reg_rem}], {is_64}, 0x{next_pc:08x}u, &div_rem, &div_ex);")
            stmts.append("if (div_ex) return;")
            stmts.append(f"cpu->d[{reg_rem}] = div_rem;")
            stmts.append(f"cpu->d[{reg_quot}] = div_quot;")
        stmts.append(f"cpu->pc = 0x{next_pc:08x}u;")
        stmts.append(f"cpu->cycles += {cycles};")
        return stmts

    # 25. TRAP & STOP
    if mnem == 'trap':
        if not ops or ops[0].type != m68k.M68K_OP_IMM:
            return None
        vec = 32 + (ops[0].imm & 0x0f)
        return [
            "f3_cc_flush(cpu);",
            f"f3_exception(cpu, {vec}u, 0x{next_pc:08x}u);",
            "return;",
        ]

    if mnem == 'stop':
        if not ops or ops[0].type != m68k.M68K_OP_IMM:
            return None
        imm16 = ops[0].imm & 0xffff
        return [
            f"if (!(cpu->sr & 0x2000u)) {{ f3_cc_flush(cpu); f3_exception(cpu, 8u, 0x{insn.address:08x}u); return; }}",
            "f3_cc_flush(cpu);",
            f"f3_set_sr(cpu, 0x{imm16:04x}u);",
            "cpu->stopped = 1;",
            f"cpu->pc = 0x{next_pc:08x}u;",
            f"cpu->cycles += {cycles};",
            "return;",
        ]

    # Unsupported instruction - honestly return None to delegate Musashi
    return None
