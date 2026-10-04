#!/usr/bin/env python3
"""Land Maker Japan 68000 sound driver native compiler.

Compiles sound ROM instructions into statically compiled native C blocks
with pinned 68000 instruction-boundary timing.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import asdict
import json
from pathlib import Path
import re
import sys
import zlib
import tomllib

import capstone as cs
import capstone.m68k as m68k

# Ensure project root is in sys.path to access recomp package
ROOT_DIR = Path(__file__).resolve().parents[1]
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from recomp.emitter import lower as emitter_lower, _decode_ea
from recomp.discovery import (
    parse_exclusions, exclusion_at, _resolve_target,
    CALL_MNEMONICS, UNCOND_BRANCH_MNEMONICS, COND_BRANCH_MNEMONICS,
)


def load_68000_base_cycles() -> bytes:
    """Load Musashi 68000 base cycles from recomp/68000_cycles.csv."""
    csv_path = ROOT_DIR / "recomp" / "68000_cycles.csv"

    table = bytearray(65536)
    for line in csv_path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        mask_s, match_s, cyc_s = line.split(",")
        mask, match, cyc = int(mask_s, 16), int(match_s, 16), int(cyc_s)
        free = (~mask) & 0xffff
        sub = free
        while True:
            table[match | sub] = cyc
            if sub == 0:
                break
            sub = (sub - 1) & free
    return bytes(table)


BASE_CYCLES = load_68000_base_cycles()


def sound_instruction_cycles(insn: cs.CsInsn) -> str:
    """Cost evaluated before register mutation; memory operands are handled once."""
    opcode = int.from_bytes(insn.bytes[:2], "big")
    mnem = insn.mnemonic.split(".")[0]
    cycles = BASE_CYCLES[opcode]
    if opcode & 0xf000 == 0x6000 and (opcode >> 8) & 15 >= 2:
        untaken = cycles + (-2 if insn.size == 2 else 2)
        return f"(f3_eval_cond(cpu, {(opcode >> 8) & 15}) ? {cycles}u : {untaken}u)"
    if mnem == "movem":
        count = int.from_bytes(insn.bytes[2:4], "big").bit_count()
        return f"{cycles + count * (8 if opcode & 0x40 else 4)}u"
    if opcode & 0xf000 == 0xe000 and opcode & 0xc0 != 0xc0:
        count = f"(cpu->d[{(opcode >> 9) & 7}] & 63u)" if opcode & 0x20 else f"{((opcode >> 9) & 7) or 8}u"
        return f"({cycles}u + 2u * {count})"
    if mnem in ("bchg", "bclr", "bset") and opcode & 0x38 == 0:
        bit = f"cpu->d[{(opcode >> 9) & 7}]" if opcode & 0x100 else f"{insn.operands[0].imm}u"
        return f"({cycles}u - ((({bit}) & 31u) < 16u ? 2u : 0u))"
    if opcode & 0xf0f8 == 0x50c0 and (opcode >> 8) & 15 >= 2:
        return f"({cycles}u + (f3_eval_cond(cpu, {(opcode >> 8) & 15}) ? 2u : 0u))"
    return "132u" if mnem == "reset" else f"{cycles}u"


def sound_lower(insn: cs.CsInsn) -> list[str] | None:
    """Lower a 68000 instruction to C statements for SoundNative."""
    raw = bytes(insn.bytes)
    opcode = int.from_bytes(raw[:2], "big")
    mnem = insn.mnemonic.split(".")[0].lower()
    next_pc = insn.address + insn.size

    # A-line and F-line coprocessor/emulator traps
    if opcode >> 12 in (10, 15):
        vector = 10 if opcode >> 12 == 10 else 11
        return [
            f"cpu->pc = 0x{insn.address:08x}u;",
            "f3_sound_cc_flush(cpu);",
            f"f3_sound_exception(cpu, {vector}u, 0x{insn.address:08x}u);",
            "return;",
        ]

    # Illegal instruction trap (0x4afc or illegal opcodes)
    if opcode == 0x4afc or (opcode & 0xfff8 == 0x4848):
        return [
            f"cpu->pc = 0x{insn.address:08x}u;",
            "f3_sound_cc_flush(cpu);",
            f"f3_sound_exception(cpu, 4u, 0x{insn.address:08x}u);",
            "return;",
        ]

    # RTE on 68000: 6-byte pop, no 68020 format word
    if mnem == "rte":
        return [
            f"cpu->pc = 0x{insn.address:08x}u;",
            "f3_sound_rte(cpu);",
            "return;",
        ]

    # STOP on 68000: sets SR, evaluates immediate IRQ unmask
    if mnem == "stop":
        if not insn.operands or insn.operands[0].type != m68k.M68K_OP_IMM:
            return None
        imm16 = insn.operands[0].imm & 0xffff
        return [
            f"cpu->pc = 0x{insn.address:08x}u;",
            f"f3_sound_stop(cpu, 0x{imm16:04x}u, 0x{next_pc:08x}u);",
            "return;",
        ]

    # TRAP #n on 68000: 6-byte exception frame
    if mnem == "trap":
        if not insn.operands or insn.operands[0].type != m68k.M68K_OP_IMM:
            return None
        vec = 32 + (insn.operands[0].imm & 0x0f)
        return [
            f"cpu->pc = 0x{insn.address:08x}u;",
            "f3_sound_cc_flush(cpu);",
            f"f3_sound_exception(cpu, {vec}u, 0x{next_pc:08x}u);",
            "return;",
        ]

    # DBcc instruction on 68000
    if opcode & 0xf0f8 == 0x50c8:
        cond = (opcode >> 8) & 0x0f
        reg_num = opcode & 7
        try:
            target = (insn.address + 2 + insn.operands[1].br_disp.disp) & 0xffffffff
        except Exception:
            return None
        return [
            f"cpu->pc = 0x{insn.address:08x}u;",
            f"if (!f3_sound_eval_cond(cpu, {cond})) {{",
            f"    uint16_t count = (uint16_t)(cpu->d[{reg_num}] & 0xffffu);",
            f"    count--;",
            f"    cpu->d[{reg_num}] = (cpu->d[{reg_num}] & 0xffff0000u) | count;",
            f"    if (count != 0xffffu) {{",
            f"        cpu->pc = 0x{target:08x}u;",
            f"        cpu->cycles += 10u;",
            f"        return;",
            f"    }}",
            f"    cpu->pc = 0x{next_pc:08x}u;",
            f"    cpu->cycles += 14u;",
            f"    return;",
            f"}}",
            f"cpu->pc = 0x{next_pc:08x}u;",
            f"cpu->cycles += 12u;",
            f"return;",
        ]
    if mnem in ("mulu", "muls", "divu", "divs"):
        ea = _decode_ea(insn, insn.operands[0], 2, "source", post_inc_on_read=True)
        if not ea:
            return None
        register = (opcode >> 9) & 7
        base = BASE_CYCLES[opcode]
        statements = ea.ea_setup + ea.read_stmts + [
            f"uint16_t source = (uint16_t)({ea.val_expr});",
            f"uint32_t destination = cpu->d[{register}];",
        ]
        if mnem.startswith("mul"):
            cast = "int16_t" if mnem == "muls" else "uint16_t"
            statements += [
                f"cpu->cycles += {base}u + f3_sound_{mnem}_cycles(source);",
                f"cpu->d[{register}] = f3_{mnem}_w(cpu, ({cast})source, ({cast})destination);",
            ]
        else:
            cast = "int16_t" if mnem == "divs" else "uint16_t"
            cost = f"{base - 140}u + f3_sound_divu_cycles(destination, source)" if mnem == "divu" else f"{base}u"
            statements += [
                "int exception = 0;",
                f"uint32_t result = f3_sound_{mnem}_w(cpu, ({cast})source, destination, 0x{next_pc:x}u, &exception);",
                "if (exception) return;",
                f"cpu->cycles += {cost};",
                f"cpu->d[{register}] = result;",
            ]
        statements += [f"cpu->pc = 0x{next_pc:x}u;"]
        return [re.sub(r"\bf3_read(8|16|32)\b", r"f3_sound_read\1", s) for s in statements]
    stmts = emitter_lower(insn)
    if stmts is None:
        return None

    cyc_expr = sound_instruction_cycles(insn)

    remapped = [f"const uint32_t sound_cycles = {cyc_expr};"]
    writes_sr = any("f3_set_sr(" in s for s in stmts)
    reads_sr = (mnem == "move" and insn.operands[0].type == m68k.M68K_OP_REG
                and insn.operands[0].reg == m68k.M68K_REG_SR and not writes_sr)
    for s in stmts:
        if reads_sr and "f3_exception(cpu, 8u," in s:
            continue  # MOVE from SR is unprivileged on 68000, unlike 68010/020.
        if "f3_reset_devices(" in s:
            continue  # Sound CPU RESET output is unconnected in the oracle bridge.
        if writes_sr and s == f"cpu->pc = 0x{next_pc:08x}u;":
            continue
        if "f3_set_sr(" in s:
            remapped.append(f"cpu->pc = 0x{next_pc:08x}u;")
        # Remap function prefixes: f3_ -> f3_sound_
        s = re.sub(r"\bf3_read([0-9]+)\b", r"f3_sound_read\1", s)
        s = re.sub(r"\bf3_write([0-9]+)\b", r"f3_sound_write\1", s)
        s = re.sub(r"\bf3_set_sr\b", r"f3_sound_set_sr", s)
        s = re.sub(r"\bf3_exception\b", r"f3_sound_exception", s)
        s = re.sub(r"\bf3_reset_devices\b", r"f3_sound_reset_devices", s)
        s = re.sub(r"\bf3_div([su])_w\b", r"f3_sound_div\1_w", s)
        s = re.sub(r"\bf3_mul([su])_w\b", r"f3_sound_mul\1_w", s)

        # Replace 68020 cycle cost statement
        if re.search(r"cpu->cycles \+= [^;]+;", s):
            s = re.sub(r"cpu->cycles \+= [^;]+;", "cpu->cycles += sound_cycles;", s)

        remapped.append(s)

    return remapped


def load_sound_rom(rom_dir: Path) -> bytes:
    """Load and validate the 512KiB interleaved sound ROM from rom_dir."""
    rom_dir = Path(rom_dir)
    p14 = rom_dir / "e61-14.32"
    p15 = rom_dir / "e61-15.33"

    if p14.exists() and p15.exists():
        d14 = bytearray(p14.read_bytes())
        d15 = bytearray(p15.read_bytes())
        if len(d14) == 0x20000:
            d14.extend(b"\xff" * 0x20000)
        if len(d15) == 0x20000:
            d15.extend(b"\xff" * 0x20000)
        if len(d14) != 0x40000 or len(d15) != 0x40000:
            raise ValueError(f"Unexpected ROM chip sizes: {len(d14)} / {len(d15)}")
        interleaved = bytearray(0x80000)
        for i in range(0x40000):
            interleaved[i * 2] = d14[i]
            interleaved[i * 2 + 1] = d15[i]
        rom = bytes(interleaved)
    elif (rom_dir / "sound.bin").exists():
        rom = (rom_dir / "sound.bin").read_bytes()
    else:
        raise FileNotFoundError(f"Sound ROM chips e61-14.32 / e61-15.33 not found in {rom_dir}")

    crc = zlib.crc32(rom) & 0xffffffff
    expected_crc = 0x5a7e9117
    if crc != expected_crc:
        raise ValueError(f"Sound ROM CRC mismatch: got 0x{crc:08x}, expected 0x{expected_crc:08x}")

    return rom


def compile_sound_rom(
    rom: bytes,
    output_dir: Path,
    coverage_mode: str = "all_aligned",
    blocks_per_file: int = 1024,
    config: dict | None = None,
    profile_tiers: Path | None = None,
    profile_slim: Path | None = None,
) -> dict:
    """Compile sound ROM instructions into native C sources and CMake configuration."""
    if profile_tiers is not None and profile_slim is not None:
        raise ValueError("profile_tiers and profile_slim are mutually exclusive")
    rom_base = 0xc00000
    # Candidate entries always cover the exact post-exclusion ROM complement.
    exclusions = parse_exclusions(config or {}, len(rom), cpu="sound", base=rom_base)
    entries = [pc for pc in range(rom_base, rom_base + len(rom), 2)
               if exclusion_at(exclusions, pc) is None]
    profile_path = profile_tiers if profile_tiers is not None else profile_slim
    profile_mode = "slim" if profile_slim is not None else "tiers" if profile_tiers is not None else "normal"
    if profile_path is not None:
        from recomp.block_profile import load_hot

        hot_entries = load_hot(profile_path, "sound", rom, base=rom_base,
                               exclusions=exclusions) & set(entries)
        if not hot_entries:
            raise ValueError("Sound profile contains no matching sound hits")
    else:
        hot_entries = set(entries)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    md = cs.Cs(cs.CS_ARCH_M68K, cs.CS_MODE_M68K_000)
    md.detail = True

    # Profile identity still covers the original ROM, even in slim mode.
    start_off, end_off = 0, len(rom)
    excluded_words = sum((region.end - region.start) // 2 for region in exclusions)
    total_words = (end_off - start_off) // 2
    # Reset/vector addresses are code seeds, unlike apparent branch targets
    # found by exhaustive decoding of arbitrary ROM data.
    for offset in range(4, min(len(rom), 0x400) - 3, 4):
        target = int.from_bytes(rom[offset:offset + 4], "big")
        region = exclusion_at(exclusions, target)
        if region is not None:
            raise ValueError(
                f"Sound vector {offset // 4} targets excluded PC 0x{target:08x} "
                f"in [0x{region.start:08x}, 0x{region.end:08x}): {region.reason}"
            )

    table = []
    shards: dict[str, list[list[str]]] = {"hot": [], "cold": []}
    current_shards: dict[str, list[str]] = {"hot": [], "cold": []}
    source_names: list[str] = []
    tier_sources: dict[str, list[str]] = {"hot": [], "cold": []}

    supported = Counter()
    unsupported = Counter()
    unsupported_pcs = []
    deferred_targets = []
    emitted_functions = 0

    for addr in range(start_off, end_off, 2):
        pc = rom_base + addr
        if exclusion_at(exclusions, pc) is not None:
            continue
        if profile_slim is not None and pc not in hot_entries:
            continue
        tier = "hot" if pc in hot_entries else "cold"
        fn_name = f"f3_sound_block_{pc:06x}"
        opcode = int.from_bytes(rom[addr:addr + 2], "big")
        if opcode >> 12 in (10, 15):
            table.append((pc, f"f3_sound_vector_{10 if opcode >> 12 == 10 else 11}"))
            supported["line_exception"] += 1
            continue
        if not BASE_CYCLES[opcode] and opcode != 0x4e70:
            table.append((pc, "f3_sound_vector_4"))
            supported["illegal_exception"] += 1
            continue
        table.append((pc, fn_name))

        code = bytearray(rom[addr : addr + 16])
        # Capstone rejects nonzero ignored upper bytes for CCR/bit immediates.
        # The 68000 consumes the word but uses only its low 8 / 5 / 3 bits.
        if len(code) >= 4 and (opcode in (0x003c, 0x023c, 0x0a3c)
                               or opcode & 0xff00 == 0x0800):
            code[2] = 0
        insns = list(md.disasm(code, pc, 1))

        if insns:
            insn = insns[0]
            mnemonic = insn.mnemonic.split(".")[0].lower()
            if mnemonic in CALL_MNEMONICS | UNCOND_BRANCH_MNEMONICS | COND_BRANCH_MNEMONICS:
                target = _resolve_target(insn, insn.operands[-1]) if insn.operands else None
                region = exclusion_at(exclusions, target & 0x00ffffff) if target is not None else None
                if region is not None:
                    deferred_targets.append({
                        "pc": pc, "target": target, "reason": region.reason,
                        "validation": "deferred_runtime",
                    })
            stmts = sound_lower(insn)
            if stmts is not None:
                supported[insn.mnemonic.split(".")[0].lower()] += 1
                lines = [f"void {fn_name}(f3_cpu *cpu) {{"]
                lines.extend("    " + s for s in stmts)
                lines.append("    f3_sound_cc_flush(cpu);")
                lines.append("}")
            else:
                unsupported[insn.mnemonic.split(".")[0].lower()] += 1
                unsupported_pcs.append(pc)
                # Emit actionable error stub
                lines = [
                    f"void {fn_name}(f3_cpu *cpu) {{",
                    f"    f3_sound_unsupported_pc(cpu, 0x{pc:08x}u);",
                    "}",
                ]
        else:
            opcode = int.from_bytes(rom[addr : addr + 2], "big")
            unsupported[f"raw_0x{opcode:04x}"] += 1
            unsupported_pcs.append(pc)
            # Check for Line-A, Line-F, or illegal vector
            vector = (
                10
                if (opcode >> 12) == 10
                else 11
                if (opcode >> 12) == 15
                else 4
                if not BASE_CYCLES[opcode] and opcode != 0x4e70
                else None
            )
            if vector is not None:
                lines = [
                    f"void {fn_name}(f3_cpu *cpu) {{",
                    f"    cpu->pc = 0x{pc:08x}u;",
                    "    f3_sound_cc_flush(cpu);",
                    f"    f3_sound_exception(cpu, {vector}u, 0x{pc:08x}u);",
                    "}",
                ]
            else:
                lines = [
                    f"void {fn_name}(f3_cpu *cpu) {{",
                    f"    f3_sound_unsupported_pc(cpu, 0x{pc:08x}u);",
                    "}",
                ]

        lines.insert(1, f"    F3_PROFILE_HIT_SOUND(0x{pc:08x}u);")
        emitted_functions += 1
        current_shards[tier].append("\n".join(lines))
        if len(current_shards[tier]) == blocks_per_file:
            shards[tier].append(current_shards[tier])
            current_shards[tier] = []

    preamble = (
        "/* Generated by tools/compile_sound.py. Do not commit. */\n"
        '#include "runtime/sound_native_ops.h"\n'
        '#include <f3rt/block_profile.h>\n'
        '#include "sound_program.h"\n\n'
    )

    for tier in ("hot", "cold"):
        if current_shards[tier]:
            shards[tier].append(current_shards[tier])
        for index, shard in enumerate(shards[tier]):
            prefix = "sound_blocks" if profile_path is None else f"sound_blocks_{tier}"
            filename = f"{prefix}_{index:04d}.c"
            (output_dir / filename).write_text(preamble + "\n\n".join(shard) + "\n")
            source_names.append(filename)
            tier_sources[tier].append(filename)

    # Generate sound_program.h
    header_content = (
        "/* Generated by tools/compile_sound.py. Do not commit. */\n"
        "#ifndef F3_SOUND_GENERATED_PROGRAM_H\n"
        "#define F3_SOUND_GENERATED_PROGRAM_H\n\n"
        "#include <stddef.h>\n"
        "#include <f3rt/cpu_abi.h>\n\n"
        "#if F3RT_ABI_VERSION != 3u\n"
        '#error "Generated sound program requires F3RT_ABI_VERSION 3"\n'
        "#endif\n\n"
        "#ifdef __cplusplus\n"
        'extern "C" {\n'
        "#endif\n\n"
        "extern const f3_block f3_sound_blocks[];\n"
        "extern const size_t f3_sound_block_count;\n\n"
        "extern const f3_excluded_range f3_sound_excluded_ranges[];\n"
        "extern const size_t f3_sound_excluded_count;\n\n"
        "#ifdef __cplusplus\n"
        "}\n"
        "#endif\n\n"
        "#endif /* F3_SOUND_GENERATED_PROGRAM_H */\n"
    )
    (output_dir / "sound_program.h").write_text(header_content)

    # Generate sound_program.c
    program_c = [
        "/* Generated by tools/compile_sound.py. Do not commit. */\n",
        '#include "runtime/sound_native_ops.h"\n',
        '#include <f3rt/block_profile.h>\n',
        '#include "sound_program.h"\n\n',
    ]
    exception_names = {name for _, name in table if name.startswith("f3_sound_vector_")}
    emitted_functions += len(exception_names)
    hot_exception_names = ({name for pc, name in table if pc in hot_entries and name in exception_names}
                           if profile_tiers is not None else set())
    exception_sources = {"hot": [], "cold": []}
    for vector in (4, 10, 11):
        name = f"f3_sound_vector_{vector}"
        if name in exception_names:
            storage = "" if profile_tiers is not None else "static "
            definition = (
                f"{storage}void {name}(f3_cpu *cpu) {{ "
                f"F3_PROFILE_HIT_SOUND(cpu->pc); f3_sound_exception(cpu, {vector}, cpu->pc); }}\n"
            )
            if profile_tiers is None:
                program_c.append(definition)
            else:
                program_c.append(f"extern void {name}(f3_cpu *cpu);\n")
                tier = "hot" if name in hot_exception_names else "cold"
                exception_sources[tier].append(definition)
    for tier, definitions in exception_sources.items():
        if definitions:
            filename = f"sound_exceptions_{tier}.c"
            (output_dir / filename).write_text(preamble + "".join(definitions))
            source_names.append(filename)
            tier_sources[tier].append(filename)
    for name in sorted({name for _, name in table if not name.startswith("f3_sound_vector_")}):
        program_c.append(f"void {name}(f3_cpu *cpu);\n")
    program_c.append("\nconst f3_block f3_sound_blocks[] = {\n")
    for pc, name in table:
        program_c.append(f"    {{ 0x{pc:08x}u, {name} }},\n")
    if not table:
        program_c.append("    { 0u, 0 },\n")
    program_c.append("};\n")
    program_c.append(f"const size_t f3_sound_block_count = {len(table)}u;\n")
    program_c.append("\nconst f3_excluded_range f3_sound_excluded_ranges[] = {\n")
    for region in exclusions:
        program_c.append(
            f"    {{ 0x{region.start:08x}u, 0x{region.end:08x}u, "
            f"{json.dumps(region.reason, ensure_ascii=False)}, {json.dumps(region.evidence, ensure_ascii=False)} }},\n"
        )
    if not exclusions:
        program_c.append("    { 0u, 0u, 0, 0 },\n")
    program_c.append("};\n")
    program_c.append(f"const size_t f3_sound_excluded_count = {len(exclusions)}u;\n")

    (output_dir / "sound_program.c").write_text("".join(program_c))
    source_names.append("sound_program.c")
    tier_sources["hot"].append("sound_program.c")

    # Generate sources.cmake
    cmake_lines = ["set(F3_SOUND_GENERATED_SOURCES\n"]
    for src in source_names:
        cmake_lines.append(f'    "${{CMAKE_CURRENT_LIST_DIR}}/{src}"\n')
    cmake_lines.append(")\n")
    for tier in ("hot", "cold"):
        cmake_lines.append(f"set(F3_SOUND_GENERATED_{tier.upper()}_SOURCES\n")
        for src in tier_sources[tier]:
            cmake_lines.append(f'    "${{CMAKE_CURRENT_LIST_DIR}}/{src}"\n')
        cmake_lines.append(")\n")
    (output_dir / "sources.cmake").write_text("".join(cmake_lines))

    report = {
        "rom_crc32": f"{zlib.crc32(rom) & 0xffffffff:08x}",
        "coverage_mode": coverage_mode,
        "total_words": total_words,
        "compiled_blocks": len(table),
        "compiled_entries": len(table),
        "emitted_functions": emitted_functions,
        "excluded_words": excluded_words,
        "excluded_regions": [asdict(region) for region in exclusions],
        "deferred_excluded_targets": deferred_targets,
        "profile_mode": profile_mode,
        "original_entries": len(entries),
        "retained_entries": len(table),
        "hot_entries": len(hot_entries),
        "cold_entries": len(entries) - len(hot_entries),
        "supported_instructions": sum(supported.values()),
        "unsupported_words": sum(unsupported.values()),
        "unsupported_pcs": unsupported_pcs,
        "source_files": source_names,
        "supported_mnemonics": dict(sorted(supported.items())),
        "unsupported_mnemonics": dict(sorted(unsupported.items())),
    }
    (output_dir / "coverage.json").write_text(json.dumps(report, indent=2) + "\n")
    inventory = {
        "version": 1,
        "region": "sound",
        "rom_crc32": report["rom_crc32"],
        "base": rom_base,
        "size": len(rom),
        "entries": entries,
        "retained_entries": len(table),
        "hot_entries": len(hot_entries),
        "cold_entries": len(entries) - len(hot_entries),
        "generated_c_bytes": sum((output_dir / name).stat().st_size for name in source_names),
    }
    (output_dir / "profile_inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")

    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom-dir", required=True, type=Path, help="Directory containing Land Maker sound ROMs")
    parser.add_argument("--output", required=True, type=Path, help="Output directory for generated sources")
    parser.add_argument("--config", required=True, type=Path, help="Game TOML containing shared exclusions")
    parser.add_argument("--blocks-per-file", type=int, default=1024, help="Number of instruction blocks per shard C file")
    profile = parser.add_mutually_exclusive_group()
    profile.add_argument("--profile-tiers", type=Path, help="Profile for hot/cold full-coverage generation")
    profile.add_argument("--profile-slim", type=Path, help="Profile for explicit hot-only generation")
    parser.add_argument(
        "--coverage",
        default="all_aligned",
        choices=["all_aligned"],
        help="Compile every aligned entry in the complete loaded sound region",
    )
    args = parser.parse_args()
    with args.config.open("rb") as stream:
        config = tomllib.load(stream)

    print(f"Loading sound ROM from {args.rom_dir}...")
    rom = load_sound_rom(args.rom_dir)
    print("Validated Land Maker Japan sound ROM (512KiB, CRC: 5a7e9117)")

    print(f"Compiling sound model (coverage: {args.coverage}) to {args.output}...")
    report = compile_sound_rom(
        rom,
        output_dir=args.output,
        coverage_mode=args.coverage,
        blocks_per_file=args.blocks_per_file,
        config=config,
        profile_tiers=args.profile_tiers,
        profile_slim=args.profile_slim,
    )

    print(
        f"Generated {len(report['source_files'])} sources "
        f"({report['retained_entries']}/{report['original_entries']} entries, {report['profile_mode']}, "
        f"{report['emitted_functions']} functions)"
    )
    print(f"Excluded {report['excluded_words']} words in {len(report['excluded_regions'])} regions")
    print(f"Supported instructions: {report['supported_instructions']}")
    print(f"Actionable error stubs: {report['unsupported_words']}")
    print("Compilation completed successfully.")


if __name__ == "__main__":
    main()
