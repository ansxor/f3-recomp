"""68EC020 ROM lane loader and recursive code discovery for Taito F3."""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import sys
import tomllib
import zlib

# Ensure Capstone can be loaded from build/python if not globally installed
try:
    import capstone
    from capstone import Cs, CsInsn, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_020
    from capstone.m68k import (
        M68K_AM_ABSOLUTE_DATA_LONG,
        M68K_AM_ABSOLUTE_DATA_SHORT,
        M68K_AM_BRANCH_DISPLACEMENT,
        M68K_AM_PCI_DISP,
        M68K_AM_PCI_INDEX_8_BIT_DISP,
        M68K_AM_PCI_INDEX_BASE_DISP,
        M68K_AM_PC_MEMI_POST_INDEX,
        M68K_AM_PC_MEMI_PRE_INDEX,
        M68K_OP_BR_DISP,
        M68K_OP_IMM,
    )
except ImportError:
    build_python = Path(__file__).resolve().parent.parent / "build" / "python"
    if build_python.is_dir() and str(build_python) not in sys.path:
        sys.path.insert(0, str(build_python))
    import capstone
    from capstone import Cs, CsInsn, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_020
    from capstone.m68k import (
        M68K_AM_ABSOLUTE_DATA_LONG,
        M68K_AM_ABSOLUTE_DATA_SHORT,
        M68K_AM_BRANCH_DISPLACEMENT,
        M68K_AM_PCI_DISP,
        M68K_AM_PCI_INDEX_8_BIT_DISP,
        M68K_AM_PCI_INDEX_BASE_DISP,
        M68K_AM_PC_MEMI_POST_INDEX,
        M68K_AM_PC_MEMI_PRE_INDEX,
        M68K_OP_BR_DISP,
        M68K_OP_IMM,
    )


# 68020 control flow instruction categories
TERMINAL_MNEMONICS = {"rts", "rte", "rtr", "rtd", "illegal"}

COND_BRANCH_MNEMONICS = {
    "bcc", "bcs", "beq", "bge", "bgt", "bhi", "ble", "bls", "blt", "bmi",
    "bne", "bpl", "bvc", "bvs",
    "dbcc", "dbcs", "dbeq", "dbf", "dbge", "dbgt", "dbhi", "dble", "dbls",
    "dblt", "dbmi", "dbne", "dbpl", "dbt", "dbvc", "dbvs", "dbra",
    "fbcc", "fbeq", "fbge", "fbgt", "fble", "fblt", "fbne", "fbnge", "fbnle",
}

CALL_MNEMONICS = {"bsr", "jsr"}
UNCOND_BRANCH_MNEMONICS = {"bra", "jmp"}


@dataclass
class Discovery:
    """Discovered instructions, basic blocks, functions, and coverage report."""

    instructions: dict[int, capstone.CsInsn]
    blocks: dict[int, list[int]]
    functions: set[int]
    report: dict


def _parse_int_address(val: int | str) -> int:
    """Parse address given as integer or hex/decimal string."""
    if isinstance(val, int):
        return val
    if isinstance(val, str):
        val = val.strip()
        if val.startswith(("0x", "0X")):
            return int(val, 16)
        return int(val, 10)
    raise ValueError(f"Invalid address value: {val!r}")


def load_rom(config_path: str | Path, rom_dir: str | Path) -> tuple[bytes, dict]:
    """Load and verify ROM lanes according to game config, returning interleaved bytes and config.

    Enforces strict hash and size verification per lane. Missing or mismatched lanes
    raise explicit exceptions without silent set substitution.
    """
    config_path = Path(config_path)
    rom_dir = Path(rom_dir)
    config = tomllib.loads(config_path.read_text("utf-8"))

    game_id = config.get("game", {}).get("id", "unknown")
    rom_cfg = config.get("rom")
    if not rom_cfg:
        raise ValueError(f"Config '{config_path}' is missing [rom] section.")

    total_size = rom_cfg.get("size")
    if not isinstance(total_size, int) or total_size <= 0:
        raise ValueError(f"Config '{config_path}' must specify positive integer 'size' under [rom].")

    interleave = rom_cfg.get("interleave", 4)
    lanes = rom_cfg.get("lanes", [])
    if not isinstance(interleave, int) or interleave <= 0 or total_size % interleave:
        raise ValueError("ROM size must be a positive multiple of its interleave.")
    if not lanes:
        raise ValueError(f"Config '{config_path}' must define at least one [[rom.lanes]].")

    rom_data = bytearray(total_size)
    seen_offsets = set()

    for lane in lanes:
        filename = lane.get("file")
        if not filename:
            raise ValueError(f"Lane definition in '{config_path}' is missing 'file' name.")

        lane_path = rom_dir / filename
        if not lane_path.is_file():
            raise FileNotFoundError(
                f"Missing ROM lane file '{filename}' in '{rom_dir}' for game '{game_id}'."
            )

        data = lane_path.read_bytes()
        expected_size = lane.get("size")
        if expected_size is not None and len(data) != expected_size:
            raise ValueError(
                f"ROM lane '{filename}' size mismatch: expected {expected_size} bytes, "
                f"got {len(data)} bytes."
            )

        expected_crc = lane.get("crc")
        if expected_crc:
            actual_crc = f"{zlib.crc32(data) & 0xFFFFFFFF:08x}"
            if actual_crc.lower() != expected_crc.lower():
                raise ValueError(
                    f"ROM lane '{filename}' CRC32 mismatch: expected {expected_crc}, "
                    f"got {actual_crc}."
                )

        expected_sha1 = lane.get("sha1")
        if expected_sha1:
            actual_sha1 = hashlib.sha1(data).hexdigest().lower()
            if actual_sha1 != expected_sha1.lower():
                raise ValueError(
                    f"ROM lane '{filename}' SHA1 mismatch: expected {expected_sha1}, "
                    f"got {actual_sha1}."
                )

        offset = lane.get("offset", 0)
        if not isinstance(offset, int) or not 0 <= offset < interleave:
            raise ValueError(f"ROM lane '{filename}' has invalid byte offset {offset}.")
        if len(data) != total_size // interleave:
            raise ValueError(f"ROM lane '{filename}' does not fill the configured image.")
        if offset in seen_offsets:
            raise ValueError(f"Duplicate lane offset {offset} in '{config_path}'.")
        seen_offsets.add(offset)

        rom_data[offset::interleave] = data
    if seen_offsets != set(range(interleave)):
        raise ValueError("ROM lane configuration must cover every byte of the image.")

    return bytes(rom_data), config


def _resolve_target(insn: capstone.CsInsn, op: capstone.m68k.M68KOp) -> int | None:
    """Resolve direct branch/jump/call target address from a Capstone operand."""
    if op.type == M68K_OP_BR_DISP or op.address_mode == M68K_AM_BRANCH_DISPLACEMENT:
        return insn.address + 2 + op.br_disp.disp
    if op.address_mode in (M68K_AM_ABSOLUTE_DATA_SHORT, M68K_AM_ABSOLUTE_DATA_LONG):
        return op.imm
    if op.address_mode == M68K_AM_PCI_DISP:
        return insn.address + 2 + op.mem.disp
    if op.type == M68K_OP_IMM:
        return op.imm
    return None


def _scan_pci_index_table(rom: bytes, md: capstone.Cs, insn: capstone.CsInsn, op: capstone.m68k.M68KOp) -> list[int]:
    """Scan conservative 16-bit relative jump table (e.g. jmp table(pc, dx.w))."""
    table_addr = insn.address + 2 + op.mem.disp
    if not (0 <= table_addr < len(rom) - 2):
        return []

    # Read first 16-bit offset
    first_off = struct.unpack(">h", rom[table_addr:table_addr + 2])[0]
    if first_off <= 0 or first_off > 0x7000:
        return []

    # Read sample offsets to determine table bounds
    entries = []
    max_scan = min(first_off // 2, 128)
    if max_scan == 0:
        return []

    # Find the minimum positive offset: in 68k C compilers, the target code
    # immediately follows the jump table, so min(positive_offsets) gives the table limit.
    positive_offsets = []
    for i in range(max_scan):
        ptr = table_addr + i * 2
        if ptr + 2 > len(rom):
            break
        off = struct.unpack(">h", rom[ptr:ptr + 2])[0]
        if off > 0:
            positive_offsets.append(off)

    if not positive_offsets:
        return []

    min_off = min(positive_offsets)
    num_entries = min(min_off // 2, max_scan)
    if num_entries == 0:
        return []

    for i in range(num_entries):
        ptr = table_addr + i * 2
        off = struct.unpack(">h", rom[ptr:ptr + 2])[0]
        if off == -1:
            continue
        target = table_addr + off
        if 0x400 <= target < len(rom) and target % 2 == 0:
            entries.append(target)

    return list(set(entries))


def _scan_pc_memi_table(rom: bytes, md: capstone.Cs, insn: capstone.CsInsn, op: capstone.m68k.M68KOp) -> list[int]:
    """Scan 68020 pre-indexed memory-indirect jump table (e.g. jmp ([$table, pc, dx.w*4]))."""
    table_addr = insn.address + 2 + op.mem.in_disp
    if not (0 <= table_addr < len(rom) - 4):
        return []

    entries = []
    ptr = table_addr
    first_val = struct.unpack(">I", rom[ptr:ptr + 4])[0]
    if not (0x400 <= first_val < len(rom) and first_val % 2 == 0):
        return []

    min_target = first_val
    entries.append(first_val)
    ptr += 4

    while ptr < len(rom) - 4 and len(entries) < 64:
        if ptr >= min_target:
            break
        val = struct.unpack(">I", rom[ptr:ptr + 4])[0]
        if not (0x400 <= val < len(rom) and val % 2 == 0):
            break
        dis = list(md.disasm(rom[val:min(val + 4, len(rom))], val))
        if not dis:
            break
        if val < min_target:
            min_target = val
        entries.append(val)
        ptr += 4

    return list(set(entries))


def _extract_script_callbacks(rom: bytes, md: capstone.Cs, spec: dict) -> set[int]:
    """Follow configured actor bytecode, not instruction-decode its data words."""
    if not spec:
        return set()
    lengths = spec["operand_bytes"]
    code_ops = set(spec["code_pointer_opcodes"])
    field = int(spec["pointer_field"])
    roots = []
    # MOVE.L #script,d16(An). Other script roots can be supplied explicitly.
    for pc in range(0, len(rom) - 7, 2):
        if (int.from_bytes(rom[pc:pc + 2], "big") & 0xf1ff) == 0x217c and \
                int.from_bytes(rom[pc + 6:pc + 8], "big") == field:
            roots.append(int.from_bytes(rom[pc + 2:pc + 6], "big"))
    roots.extend(spec.get("entry_points", []))
    visited, callbacks = set(), set()
    while roots:
        pc = roots.pop()
        while 0x400 <= pc < len(rom) - 1 and not pc & 1 and pc not in visited:
            visited.add(pc)
            operation = int.from_bytes(rom[pc:pc + 2], "big")
            if operation >= len(lengths) or lengths[operation] < 0:
                break
            end = pc + 2 + lengths[operation]
            if end > len(rom):
                break
            target = int.from_bytes(rom[pc + 2:pc + 6], "big")
            if operation in code_ops and 0x400 <= target < len(rom) and not target & 1:
                if _validate_code_sequence(rom, md, target):
                    callbacks.add(target)
            if operation == spec["return_opcode"]:
                break
            if operation == spec["call_opcode"]:
                roots.append(target)
            pc = target if operation == spec["jump_opcode"] else end
    return callbacks


def _extract_trap1_tasks(rom: bytes, md: capstone.Cs) -> set[int]:
    """Extract entry points passed to TRAP #1 (thread/task spawn in Taito OS)."""
    targets = set()
    for i in range(0, len(rom) - 2, 2):
        w = (rom[i] << 8) | rom[i + 1]
        if w == 0x4E41:  # trap #1
            # Look at preceding instructions up to 24 bytes
            dis = list(md.disasm(rom[max(0, i - 24):i], max(0, i - 24)))
            if not dis:
                continue
            for ins in reversed(dis):
                base = ins.mnemonic.lower().split(".")[0]
                if base == "pea" and ins.operands:
                    t = _resolve_target(ins, ins.operands[0])
                    if t is not None and 0x400 <= t < len(rom) and t % 2 == 0:
                        targets.add(t)
                    break
                elif base == "move" and len(ins.operands) == 2:
                    if ins.operands[0].type == M68K_OP_IMM:
                        t = ins.operands[0].imm
                        if 0x400 <= t < len(rom) and t % 2 == 0:
                            targets.add(t)
                    break
    return targets


def _validate_code_sequence(rom: bytes, md: capstone.Cs, target: int) -> bool:
    """Validate that target decodes as a coherent instruction sequence ending with a branch or return."""
    cur = target
    count = 0
    while count < 32:
        if cur >= len(rom):
            return False
        chunk = rom[cur:min(cur + 24, len(rom))]
        dis = list(md.disasm(chunk, cur, count=1))
        if not dis:
            return False
        insn = dis[0]
        base = insn.mnemonic.lower().split(".")[0]
        if base in ("illegal", "dc"):
            return False
        if base in TERMINAL_MNEMONICS:
            return True
        if base in UNCOND_BRANCH_MNEMONICS or base in COND_BRANCH_MNEMONICS:
            return True
        cur += insn.size
        count += 1
    return False


def _extract_lea_move_callbacks(rom: bytes, md: capstone.Cs) -> set[int]:
    """Find callback stores and register-linked return continuations."""
    callbacks = set()
    for i in range(0, len(rom) - 8, 2):
        w1 = (rom[i] << 8) | rom[i + 1]
        if (w1 & 0xF1FF) == 0x41FA:  # lea disp16(pc), aX
            reg = (w1 >> 9) & 7
            disp = struct.unpack(">h", rom[i + 2:i + 4])[0]
            target = i + 2 + disp
            if 0x400 <= target < len(rom) and target % 2 == 0:
                w2 = (rom[i + 4] << 8) | rom[i + 5]
                # The boot ROM also uses LEA return(pc),An; JMP routine instead
                # of BSR/RTS during RAM tests before a working stack exists.
                stores_callback = ((w2 & 0xF000) == 0x2000 and
                                   ((w2 >> 3) & 7) == 1 and (w2 & 7) == reg)
                register_link = (w2 & 0xffc0) == 0x4ec0
                if (stores_callback or register_link) and _validate_code_sequence(rom, md, target):
                    callbacks.add(target)
    return callbacks


def _extract_vector_seeds(rom: bytes) -> set[int]:
    """Extract code seeds from the 68020 vector table (excluding SSP and uninitialized vectors)."""
    vectors = struct.unpack(">256I", rom[:1024])
    seeds = set()
    # Vector 1: Reset PC
    reset_pc = vectors[1]
    if 0x400 <= reset_pc < len(rom) and reset_pc % 2 == 0:
        seeds.add(reset_pc)

    # Autovectors (24..31), Trap vectors (32..47), User interrupt vectors (64..255)
    for i in range(2, 256):
        v = vectors[i]
        # Ignore 0, 0xffffffff, and addresses below 0x400 (which are inside the vector table)
        if 0x400 <= v < len(rom) and v % 2 == 0:
            seeds.add(v)

    return seeds


def discover(rom: bytes, config: dict) -> Discovery:
    """Discover instructions, basic blocks, and functions from reset/exception vectors and metadata.

    Supports configurable explicit entry points, hook PCs, and jump tables from config dict,
    while performing conservative automatic jump-table and task-trap scanning.
    Produces a JSON-serializable whole-image coverage report.
    """
    md = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
    if len(rom) < 1024:
        raise ValueError("ROM must contain the complete 68020 vector table.")
    md.detail = True

    discovery_cfg = config.get("discovery", {})
    scan_jump_tables = discovery_cfg.get("scan_jump_tables", True)
    scan_task_traps = discovery_cfg.get("scan_task_traps", True)
    scan_callbacks = discovery_cfg.get("scan_callbacks", True)
    inline_string_helpers = {_parse_int_address(pc) for pc in
                             discovery_cfg.get("inline_string_helpers", [])}

    proven_seeds: set[int] = set()
    speculative_seeds: set[int] = set()

    # 1. Vector table entry points
    vector_seeds = _extract_vector_seeds(rom)
    proven_seeds.update(vector_seeds)

    # 2. Explicit config entry points
    for ep in discovery_cfg.get("entry_points", []):
        addr = _parse_int_address(ep)
        if 0 <= addr < len(rom):
            proven_seeds.add(addr)

    # 3. Hook PCs from config.get("hooks", [])
    for hook in config.get("hooks", []):
        if isinstance(hook, dict) and "address" in hook:
            addr = _parse_int_address(hook["address"])
            if 0 <= addr < len(rom):
                proven_seeds.add(addr)

    # 4. Explicit jump tables from config
    explicit_jump_tables: dict[int, list[int]] = {}
    for jt in discovery_cfg.get("jump_tables", []):
        if isinstance(jt, dict) and "address" in jt:
            addr = _parse_int_address(jt["address"])
            targets = [_parse_int_address(t) for t in jt.get("targets", [])]
            explicit_jump_tables[addr] = targets

    # 5. Task spawn targets (trap #1)
    if scan_task_traps:
        trap1_tasks = _extract_trap1_tasks(rom, md)
        speculative_seeds.update(trap1_tasks)

    # 6. Validated RAM callbacks
    if scan_callbacks:
        cb_seeds = _extract_lea_move_callbacks(rom, md)
        speculative_seeds.update(cb_seeds)
    speculative_seeds.update(_extract_script_callbacks(
        rom, md, discovery_cfg.get("actor_scripts", {})))

    # Combined initial worklist
    all_seeds = sorted(proven_seeds | speculative_seeds)
    worklist: deque[int] = deque(all_seeds)

    instructions: dict[int, CsInsn] = {}
    functions: set[int] = set(all_seeds)
    branch_targets: dict[int, list[int]] = {}
    unresolved_branches: list[dict] = []

    # Map of all jump table targets found
    active_jump_tables: dict[int, list[int]] = dict(explicit_jump_tables)

    while worklist:
        entry = worklist.popleft()
        if entry in instructions or entry < 0 or entry >= len(rom):
            continue

        cur_pc = entry
        while True:
            if cur_pc < 0 or cur_pc >= len(rom) or cur_pc in instructions:
                break

            chunk = rom[cur_pc:min(cur_pc + 24, len(rom))]
            dis = list(md.disasm(chunk, cur_pc, count=1))
            if not dis or dis[0].id == 0:
                unresolved_branches.append({
                    "pc": f"0x{cur_pc:06x}",
                    "mnemonic": "invalid",
                    "op_str": "decode failure",
                    "reason": "decode_failure",
                })
                break

            insn = dis[0]
            instructions[cur_pc] = insn
            base_mnem = insn.mnemonic.lower().split(".")[0]

            # Check explicit or previously discovered jump tables
            if cur_pc in active_jump_tables:
                targets = active_jump_tables[cur_pc]
                branch_targets[cur_pc] = targets
                for t in targets:
                    if 0 <= t < len(rom):
                        if base_mnem in CALL_MNEMONICS:
                            functions.add(t)
                        if t not in instructions:
                            worklist.append(t)
                if base_mnem in CALL_MNEMONICS:
                    cur_pc += insn.size
                    continue
                else:
                    break

            # Subroutine call: bsr, jsr
            if base_mnem in CALL_MNEMONICS:
                fallthrough = cur_pc + insn.size
                target = None
                if insn.operands:
                    op = insn.operands[-1]
                    target = _resolve_target(insn, op)
                    if target is None and scan_jump_tables:
                        # Try conservative jump table scan
                        if op.address_mode in (M68K_AM_PCI_INDEX_8_BIT_DISP, M68K_AM_PCI_INDEX_BASE_DISP):
                            scanned = _scan_pci_index_table(rom, md, insn, op)
                            if scanned:
                                active_jump_tables[cur_pc] = scanned
                                branch_targets[cur_pc] = scanned
                                for t in scanned:
                                    functions.add(t)
                                    if t not in instructions:
                                        worklist.append(t)
                                cur_pc = fallthrough
                                continue
                        elif op.address_mode in (M68K_AM_PC_MEMI_PRE_INDEX, M68K_AM_PC_MEMI_POST_INDEX):
                            scanned = _scan_pc_memi_table(rom, md, insn, op)
                            if scanned:
                                active_jump_tables[cur_pc] = scanned
                                branch_targets[cur_pc] = scanned
                                for t in scanned:
                                    functions.add(t)
                                    if t not in instructions:
                                        worklist.append(t)
                                cur_pc = fallthrough
                                continue

                if target is not None and 0 <= target < len(rom):
                    functions.add(target)
                    branch_targets[cur_pc] = [target]
                    if target not in instructions:
                        worklist.append(target)
                else:
                    unresolved_branches.append({
                        "pc": f"0x{cur_pc:06x}",
                        "mnemonic": insn.mnemonic,
                        "op_str": insn.op_str,
                        "reason": "indirect_call",
                    })

                # Check if this call is an inline string print helper
                if target in inline_string_helpers:
                    ptr = fallthrough
                    while ptr < len(rom) and rom[ptr] != 0:
                        ptr += 1
                    ptr += 1  # Skip null terminator
                    if ptr % 2 != 0:
                        ptr += 1  # Word align
                    fallthrough = ptr

                cur_pc = fallthrough
                continue

            # Conditional branch
            elif base_mnem in COND_BRANCH_MNEMONICS:
                fallthrough = cur_pc + insn.size
                target = None
                if insn.operands:
                    target = _resolve_target(insn, insn.operands[-1])

                if target is not None and 0 <= target < len(rom):
                    branch_targets[cur_pc] = [target]
                    if target not in instructions:
                        worklist.append(target)
                else:
                    unresolved_branches.append({
                        "pc": f"0x{cur_pc:06x}",
                        "mnemonic": insn.mnemonic,
                        "op_str": insn.op_str,
                        "reason": "indirect_branch",
                    })

                if fallthrough not in instructions:
                    worklist.append(fallthrough)
                break

            # Unconditional jump/branch
            elif base_mnem in UNCOND_BRANCH_MNEMONICS:
                target = None
                if insn.operands:
                    op = insn.operands[-1]
                    target = _resolve_target(insn, op)
                    if target is None and scan_jump_tables:
                        if op.address_mode in (M68K_AM_PCI_INDEX_8_BIT_DISP, M68K_AM_PCI_INDEX_BASE_DISP):
                            scanned = _scan_pci_index_table(rom, md, insn, op)
                            if scanned:
                                active_jump_tables[cur_pc] = scanned
                                branch_targets[cur_pc] = scanned
                                for t in scanned:
                                    if t not in instructions:
                                        worklist.append(t)
                                break
                        elif op.address_mode in (M68K_AM_PC_MEMI_PRE_INDEX, M68K_AM_PC_MEMI_POST_INDEX):
                            scanned = _scan_pc_memi_table(rom, md, insn, op)
                            if scanned:
                                active_jump_tables[cur_pc] = scanned
                                branch_targets[cur_pc] = scanned
                                for t in scanned:
                                    if t not in instructions:
                                        worklist.append(t)
                                break

                if target is not None and 0 <= target < len(rom):
                    branch_targets[cur_pc] = [target]
                    if target not in instructions:
                        worklist.append(target)
                else:
                    unresolved_branches.append({
                        "pc": f"0x{cur_pc:06x}",
                        "mnemonic": insn.mnemonic,
                        "op_str": insn.op_str,
                        "reason": "indirect_jump",
                    })
                break

            # Subroutine return / Exception return / Terminal
            elif base_mnem in TERMINAL_MNEMONICS:
                break

            elif base_mnem == "trap":
                cur_pc += insn.size
                continue

            else:
                cur_pc += insn.size

    # Form clean basic blocks with splitting at all entry leaders
    all_pcs = sorted(instructions.keys())
    pc_set = set(all_pcs)

    leader_set = set(pc for pc in all_seeds if pc in pc_set)
    for pc in all_pcs:
        insn = instructions[pc]
        base_mnem = insn.mnemonic.lower().split(".")[0]
        next_pc = pc + insn.size

        if pc in branch_targets:
            for t in branch_targets[pc]:
                if t in pc_set:
                    leader_set.add(t)

        if base_mnem in COND_BRANCH_MNEMONICS or base_mnem in CALL_MNEMONICS:
            if next_pc in pc_set:
                leader_set.add(next_pc)

        if base_mnem in TERMINAL_MNEMONICS or base_mnem in UNCOND_BRANCH_MNEMONICS:
            if next_pc in pc_set:
                leader_set.add(next_pc)

    blocks: dict[int, list[int]] = {}
    current_block: list[int] = []
    current_entry: int | None = None

    for pc in all_pcs:
        if pc in leader_set or current_entry is None:
            if current_block and current_entry is not None:
                blocks[current_entry] = current_block
                current_block = []
            current_entry = pc

        current_block.append(pc)
        insn = instructions[pc]
        base_mnem = insn.mnemonic.lower().split(".")[0]
        if (
            base_mnem in TERMINAL_MNEMONICS
            or base_mnem in UNCOND_BRANCH_MNEMONICS
            or base_mnem in COND_BRANCH_MNEMONICS
        ):
            if current_entry is not None:
                blocks[current_entry] = current_block
            current_block = []
            current_entry = None

    if current_block and current_entry is not None:
        blocks[current_entry] = current_block

    # Calculate coverage statistics
    covered = bytearray(len(rom))
    for pc, insn in instructions.items():
        covered[pc:pc + insn.size] = b"\1" * insn.size
    total_code_bytes = sum(covered)
    coverage_ratio = total_code_bytes / len(rom) if len(rom) > 0 else 0.0

    # Bank-by-bank coverage (64KB banks)
    bank_size = 0x10000
    num_banks = (len(rom) + bank_size - 1) // bank_size
    bank_summary = []
    for b in range(num_banks):
        start = b * bank_size
        end = min(start + bank_size, len(rom))
        bank_pcs = [p for p in instructions if start <= p < end]
        code_bytes = sum(covered[start:end])
        bank_summary.append({
            "bank": b,
            "range": f"0x{start:06x}-0x{end - 1:06x}",
            "instruction_count": len(bank_pcs),
            "code_bytes": code_bytes,
            "classification": "contains_decoded_code" if bank_pcs else "unreached_or_data",
        })

    report = {
        "coverage_basis": "Unique bytes reachable from vector/config and heuristic seeds, not a proof that undiscovered bytes are data.",
        "summary": {
            "rom_size_bytes": len(rom),
            "total_instructions": len(instructions),
            "total_blocks": len(blocks),
            "total_functions": len(functions),
            "code_bytes": total_code_bytes,
            "coverage_pct": round(coverage_ratio * 100.0, 3),
            "proven_seeds_count": len(proven_seeds),
            "speculative_seeds_count": len(speculative_seeds),
            "unresolved_branches_count": len(unresolved_branches),
        },
        "proven_seeds": [f"0x{s:06x}" for s in sorted(proven_seeds)],
        "speculative_seeds": [f"0x{s:06x}" for s in sorted(speculative_seeds)],
        "unresolved_branches": unresolved_branches,
        "bank_summary": bank_summary,
    }

    # Verify JSON serializability of report
    json.dumps(report)

    return Discovery(
        instructions=instructions,
        blocks=blocks,
        functions=functions,
        report=report,
    )
