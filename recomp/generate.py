"""Pack literal instruction lowering into interruptible C blocks.

Every decoded instruction is an entry point: a previously unresolved indirect
transfer can enter the middle of a discovered block without interpreting it.
"""
from __future__ import annotations

from collections import Counter
from pathlib import Path
import json
import re
import zlib

from .emitter import lower
from .discovery import parse_exclusions, exclusion_at
from .timing import BASE_CYCLES

_RUNTIME_ABI_VERSION = 3


def generate(rom: bytes, discovery, output: Path, config: dict,
             max_block_instructions: int = 32, blocks_per_file: int = 128,
             profile_tiers: Path | None = None, profile_slim: Path | None = None) -> dict:
    if profile_tiers is not None and profile_slim is not None:
        raise ValueError("profile tiers and profile slim are mutually exclusive")
    if max_block_instructions < 1 or blocks_per_file < 1:
        raise ValueError("block and shard sizes must be positive")
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    exclusions = parse_exclusions(config, len(rom))
    for kind, pcs in (("instruction start", discovery.instructions),
                      ("invalid PC", discovery.invalid_pcs)):
        for pc in pcs:
            region = exclusion_at(exclusions, pc)
            if region is not None:
                raise ValueError(
                    f"Discovery contains an excluded {kind}: {pc:#x} in "
                    f"[{region.start:#x}, {region.end:#x}): {region.reason}")
    hooks = {}
    for hook in config.get("hooks", []):
        pc, symbol = int(hook["address"]), hook["symbol"]
        if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", symbol):
            raise ValueError(f"invalid hook C identifier: {symbol!r}")
        if pc not in discovery.instructions:
            raise ValueError(f"hook address is not discovered code: {pc:#x}")
        if pc in hooks:
            raise ValueError(f"duplicate hook at {pc:#x}")
        hooks[pc] = symbol

    exhaustive = config.get("discovery", {}).get("coverage") == "all_aligned"
    if exhaustive:
        # Independent decodes overlap: an extension word can also be a computed
        # jump destination. Pack by word-address page, not by assumed instruction
        # boundaries. Every fallthrough below names its actual successor label.
        pages = {}
        page_bytes = max_block_instructions * 2
        for pc in sorted(discovery.instructions):
            pages.setdefault(pc // page_bytes, []).append(pc)
        blocks = list(pages.values())
    else:
        # Discovery can revisit block interiors. Deduplicate and split at any
        # discontinuity; a decoded PC is never silently omitted from the table.
        seen = set()
        blocks = []
        for _, pcs in sorted(discovery.blocks.items()):
            block = []
            for pc in pcs:
                if pc in seen:
                    if block:
                        blocks.append(block)
                        block = []
                    continue
                if block and (len(block) >= max_block_instructions or
                              block[-1] + discovery.instructions[block[-1]].size != pc):
                    blocks.append(block)
                    block = []
                block.append(pc)
                seen.add(pc)
            if block:
                blocks.append(block)
        for pc in sorted(discovery.instructions.keys() - seen):
            blocks.append([pc])
        blocks.sort(key=lambda block: block[0])

    rom_exceptions = {}
    if exhaustive:
        for pc in discovery.invalid_pcs:
            opcode = int.from_bytes(rom[pc:pc + 2], "big")
            vector = (10 if opcode >> 12 == 10 else
                      11 if opcode >> 12 == 15 else
                      4 if not BASE_CYCLES[opcode] and opcode != 0x4e70 else None)
            if vector is not None:
                # A valid primary word rejected on its extensions is not proof
                # of an illegal opcode. RESET is valid despite its zero cost.
                rom_exceptions[pc] = vector
    original_entries = sorted(set(discovery.instructions) | set(rom_exceptions))
    profile_path = profile_tiers if profile_tiers is not None else profile_slim
    profile_mode = ("tiers" if profile_tiers is not None else
                    "slim" if profile_slim is not None else "baseline")
    hot = set()
    if profile_path is not None:
        from .block_profile import load_hot
        hot = load_hot(profile_path, "main", rom, exclusions=exclusions) & set(original_entries)
        if not hot:
            raise ValueError("profile contains no matching main executable hits")
    retained = hot if profile_slim is not None else set(original_entries)
    emission_blocks = []
    for pcs in blocks:
        if profile_path is None:
            emission_blocks.append(("hot", pcs))
        else:
            hot_pcs = [pc for pc in pcs if pc in hot]
            cold_pcs = [pc for pc in pcs if pc not in hot]
            if hot_pcs:
                emission_blocks.append(("hot", hot_pcs))
            if cold_pcs and profile_slim is None:
                emission_blocks.append(("cold", cold_pcs))

    abi_guard = (f'#if F3RT_ABI_VERSION != {_RUNTIME_ABI_VERSION}\n'
                 '#error "Generated program and f3rt ABI versions differ"\n'
                 '#endif\n')
    preamble = ('/* Generated from user-supplied ROM. Do not commit. */\n'
                '#include <f3rt/cpu_abi.h>\n' + abi_guard +
                '#include <f3rt/block_profile.h>\n'
                '#include "recomp/cpu_ops.h"\n')
    hook_declarations = ''.join(f'extern void {name}(f3_cpu *cpu);\n'
                                for name in sorted(set(hooks.values())))
    table = []
    declarations = []
    source_names = []
    tier_sources = {"hot": [], "cold": []}
    supported, unsupported = Counter(), Counter()
    unsupported_pcs = []
    shards = {"hot": [], "cold": []}
    remaining = Counter(tier for tier, _ in emission_blocks)
    for tier, pcs in emission_blocks:
        name = f'f3_native_{pcs[0]:06x}'
        declarations.append(f'extern void {name}(f3_cpu *cpu);\n')
        table.extend((pc, name) for pc in pcs)
        block_pcs = set(pcs)
        lines = [f'void {name}(f3_cpu *cpu) {{', '    switch (cpu->pc) {']
        lines.extend(f'    case 0x{pc:08x}u: goto L_{pc:06x};' for pc in pcs)
        lines.extend(['    default: return;', '    }'])
        for position, pc in enumerate(pcs):
            insn = discovery.instructions[pc]
            lines.append(f'L_{pc:06x}: {{')
            lines.append(f'    F3_PROFILE_HIT_MAIN(0x{pc:08x}u);')
            if pc in hooks:
                lines.extend(['    f3_cc_flush(cpu);', f'    {hooks[pc]}(cpu);',
                              f'    if (cpu->pc != 0x{pc:08x}u || cpu->stopped || cpu->halted) return;'])
            statements = lower(insn)
            if statements is None:
                unsupported[insn.mnemonic] += 1
                unsupported_pcs.append(pc)
                lines.extend(['    f3_cc_flush(cpu);',
                              '    if (!f3_fallback(cpu)) cpu->halted = 1;',
                              '    return;'])
            else:
                supported[insn.mnemonic] += 1
                lines.extend('    ' + statement for statement in statements)
                if exhaustive or profile_path is not None:
                    next_pc = pc + insn.size
                    if next_pc in block_pcs:
                        lines.append(f'    if (cpu->pc != 0x{next_pc:08x}u || cpu->stopped || cpu->halted || cpu->cycles >= cpu->dispatch_deadline) {{ f3_cc_flush(cpu); return; }}')
                        lines.append(f'    goto L_{next_pc:06x};')
                    else:
                        lines.extend(['    f3_cc_flush(cpu);', '    return;'])
                elif position + 1 < len(pcs):
                    next_pc = pcs[position + 1]
                    # Yield at the first instruction boundary reaching a runtime
                    # event, and never fall through after a control transfer.
                    lines.append(f'    if (cpu->pc != 0x{next_pc:08x}u || cpu->stopped || cpu->halted || cpu->cycles >= cpu->dispatch_deadline) {{ f3_cc_flush(cpu); return; }}')
            lines.append('}')
        lines.extend(['    f3_cc_flush(cpu);', '}\n'])
        shard = shards[tier]
        shard.append('\n'.join(lines))
        remaining[tier] -= 1
        if len(shard) == blocks_per_file or remaining[tier] == 0:
            filename = (f'blocks_{len(source_names):04d}.c' if profile_path is None else
                        f'blocks_{tier}_{len(tier_sources[tier]):04d}.c')
            (output / filename).write_text(preamble + hook_declarations + '\n'.join(shard))
            source_names.append(filename)
            tier_sources[tier].append(filename)
            shards[tier] = []

    exception_entries = Counter()
    for pc, vector in rom_exceptions.items():
        if pc in retained:
            table.append((pc, f'f3_rom_exception_{vector}'))
            exception_entries[vector] += 1

    table.sort()
    if not table:
        raise ValueError("discovery produced no instructions")
    program = preamble + '#include "program.h"\n' + ''.join(declarations)
    hot_exception_vectors = ({rom_exceptions[pc] for pc in hot if pc in rom_exceptions}
                             if profile_tiers is not None else set())
    exception_sources = {"hot": [], "cold": []}
    for vector in sorted(exception_entries):
        storage = '' if profile_tiers is not None else 'static '
        definition = (f'{storage}void f3_rom_exception_{vector}(f3_cpu *cpu) {{\n'
                      '    F3_PROFILE_HIT_MAIN(cpu->pc);\n'
                      '    f3_cc_flush(cpu);\n'
                      f'    f3_exception(cpu, {vector}, cpu->pc);\n'
                      '}\n')
        if profile_tiers is None:
            program += definition
        else:
            program += f'extern void f3_rom_exception_{vector}(f3_cpu *cpu);\n'
            tier = "hot" if vector in hot_exception_vectors else "cold"
            exception_sources[tier].append(definition)
    for tier, definitions in exception_sources.items():
        if definitions:
            filename = f'exceptions_{tier}.c'
            (output / filename).write_text(preamble + ''.join(definitions))
            source_names.append(filename)
            tier_sources[tier].append(filename)
    program += 'static const f3_block translated_blocks[] = {\n'
    program += ''.join(f'    {{ 0x{pc:08x}u, {name} }},\n' for pc, name in table)
    program += '};\n'
    if exclusions:
        program += 'static const f3_excluded_range excluded_ranges[] = {\n'
        program += ''.join(
            f'    {{ 0x{region.start:08x}u, 0x{region.end:08x}u, '
            f'{json.dumps(region.reason)}, {json.dumps(region.evidence)} }},\n'
            for region in exclusions)
        program += '};\n'
    program += ('int f3_generated_register(f3_cpu *cpu) {\n'
                f'    if (!f3_validate_main_rom(cpu, {len(rom)}u, 0x{zlib.crc32(rom):08x}u)) return 0;\n'
                '    if (!f3_register_blocks(cpu, translated_blocks,\n'
                '        sizeof(translated_blocks) / sizeof(translated_blocks[0]))) return 0;\n')
    if exclusions:
        program += ('    return f3_register_exclusions(cpu, excluded_ranges,\n'
                    '        sizeof(excluded_ranges) / sizeof(excluded_ranges[0]));\n}\n')
    else:
        program += '    return f3_register_exclusions(cpu, NULL, 0);\n}\n'
    (output / 'program.c').write_text(program)
    (output / 'program.h').write_text(
        '#ifndef F3_GENERATED_PROGRAM_H\n#define F3_GENERATED_PROGRAM_H\n'
        '#include <f3rt/cpu_abi.h>\n' + abi_guard +
        '#ifdef __cplusplus\nextern "C" {\n#endif\n'
        'int f3_generated_register(f3_cpu *cpu);\n'
        '#ifdef __cplusplus\n}\n#endif\n#endif\n')
    source_names.append('program.c')
    tier_sources["hot"].append('program.c')
    source_variables = {"F3_GENERATED_SOURCES": source_names,
                        "F3_GENERATED_HOT_SOURCES": tier_sources["hot"],
                        "F3_GENERATED_COLD_SOURCES": tier_sources["cold"]}
    (output / 'sources.cmake').write_text(''.join(
        f'set({variable}\n' + ''.join(
            f'    "${{CMAKE_CURRENT_LIST_DIR}}/{name}"\n' for name in names) + ')\n'
        for variable, names in source_variables.items()))
    # Used by runtime loader, not embedded in generated C or committed.
    (output / 'program.bin').write_bytes(rom)
    inventory = {
        "version": 1, "region": "main",
        "rom_crc32": f"{zlib.crc32(rom) & 0xffffffff:08x}",
        "base": 0, "size": len(rom), "entries": original_entries,
        "retained_entries": len(table), "hot_entries": len(hot),
        "cold_entries": len(original_entries) - len(hot),
        "generated_c_bytes": sum((output / name).stat().st_size for name in source_names),
    }
    (output / 'profile_inventory.json').write_text(json.dumps(inventory, indent=2) + '\n')
    report = {
        "decoded_instructions": len(discovery.instructions),
        "registered_entries": len(table),
        "native_instructions": sum(supported.values()),
        "exception_entries": dict(sorted(exception_entries.items())),
        "undecoded_valid_primary_entries": (
            len(discovery.invalid_pcs) - len(rom_exceptions) if exhaustive else 0),
        "fallback_instructions": sum(unsupported.values()), "native_blocks": len(emission_blocks),
        "native_mnemonics": dict(sorted(supported.items())),
        "fallback_mnemonics": dict(sorted(unsupported.items())),
        "fallback_pcs": unsupported_pcs, "source_files": source_names,
        "runtime_abi_version": _RUNTIME_ABI_VERSION,
        "coverage_mode": "all_aligned" if exhaustive else "recursive",
        "exclusions": [
            {"start": region.start, "end": region.end,
             "reason": region.reason, "evidence": region.evidence}
            for region in exclusions
        ],
        "excluded_entries": sum((region.end - region.start) // 2 for region in exclusions),
        "profile_mode": profile_mode,
        "original_entries": len(original_entries),
        "retained_entries": len(table),
        "hot_entries": len(hot),
        "cold_entries": len(original_entries) - len(hot),
        "max_block_instructions": max_block_instructions,
        "timing": "68EC020 reference instruction costs; runtime deadlines end native blocks at instruction boundaries",
    }
    (output / 'lowering.json').write_text(json.dumps(report, indent=2) + '\n')
    (output / 'coverage.json').write_text(json.dumps(discovery.report, indent=2) + '\n')
    return report
