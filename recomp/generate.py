"""Pack literal instruction lowering into interruptible C blocks.

Every decoded instruction is an entry point: a previously unresolved indirect
transfer can enter the middle of a discovered block without interpreting it.
"""
from __future__ import annotations

from collections import Counter
from pathlib import Path
import json
import re

from .emitter import lower
from .discovery import parse_exclusions, exclusion_at
from .timing import BASE_CYCLES

_RUNTIME_ABI_VERSION = 3


def generate(rom: bytes, discovery, output: Path, config: dict,
             max_block_instructions: int = 32, blocks_per_file: int = 128) -> dict:
    if max_block_instructions < 1 or blocks_per_file < 1:
        raise ValueError("block and shard sizes must be positive")
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    exclusions = parse_exclusions(config, len(rom))
    if any(exclusion_at(exclusions, pc) is not None for pc in discovery.instructions):
        raise ValueError("Discovery contains an excluded instruction start")
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

    abi_guard = (f'#if F3RT_ABI_VERSION != {_RUNTIME_ABI_VERSION}\n'
                 '#error "Generated program and f3rt ABI versions differ"\n'
                 '#endif\n')
    preamble = ('/* Generated from user-supplied ROM. Do not commit. */\n'
                '#include <f3rt/cpu_abi.h>\n' + abi_guard +
                '#include "recomp/cpu_ops.h"\n')
    hook_declarations = ''.join(f'extern void {name}(f3_cpu *cpu);\n'
                                for name in sorted(set(hooks.values())))
    table = []
    declarations = []
    source_names = []
    supported, unsupported = Counter(), Counter()
    unsupported_pcs = []
    shard = []
    for index, pcs in enumerate(blocks):
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
                if exhaustive:
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
        shard.append('\n'.join(lines))
        if len(shard) == blocks_per_file or index + 1 == len(blocks):
            filename = f'blocks_{len(source_names):04d}.c'
            (output / filename).write_text(preamble + hook_declarations + '\n'.join(shard))
            source_names.append(filename)
            shard = []

    exception_entries = Counter()
    if exhaustive:
        for pc in discovery.invalid_pcs:
            if exclusion_at(exclusions, pc) is not None:
                raise ValueError(f"Discovery contains an excluded invalid PC: {pc:#x}")
            opcode = int.from_bytes(rom[pc:pc + 2], "big")
            vector = (10 if opcode >> 12 == 10 else
                      11 if opcode >> 12 == 15 else
                      4 if not BASE_CYCLES[opcode] and opcode != 0x4e70 else None)
            if vector is not None:
                # The pinned opcode metadata defaults unassigned primary words
                # to zero. RESET is the only non-F-line valid zero-base opcode.
                # A valid primary word rejected on its extensions is NOT an
                # illegal-opcode proof: leave that decoder failure explicit.
                table.append((pc, f'f3_rom_exception_{vector}'))
                exception_entries[vector] += 1

    table.sort()
    if not table:
        raise ValueError("discovery produced no instructions")
    program = preamble + '#include "program.h"\n' + ''.join(declarations)
    for vector in sorted(exception_entries):
        program += (f'static void f3_rom_exception_{vector}(f3_cpu *cpu) {{\n'
                    '    f3_cc_flush(cpu);\n'
                    f'    f3_exception(cpu, {vector}, cpu->pc);\n'
                    '}\n')
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
    (output / 'sources.cmake').write_text('set(F3_GENERATED_SOURCES\n' + ''.join(
        f'    "${{CMAKE_CURRENT_LIST_DIR}}/{name}"\n' for name in source_names) + ')\n')
    # Used by runtime loader, not embedded in generated C or committed.
    (output / 'program.bin').write_bytes(rom)
    report = {
        "decoded_instructions": len(discovery.instructions),
        "registered_entries": len(table),
        "native_instructions": sum(supported.values()),
        "exception_entries": dict(sorted(exception_entries.items())),
        "undecoded_valid_primary_entries": (
            len(discovery.invalid_pcs) - sum(exception_entries.values()) if exhaustive else 0),
        "fallback_instructions": sum(unsupported.values()), "native_blocks": len(blocks),
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
        "max_block_instructions": max_block_instructions,
        "timing": "68EC020 reference instruction costs; runtime deadlines end native blocks at instruction boundaries",
    }
    (output / 'lowering.json').write_text(json.dumps(report, indent=2) + '\n')
    (output / 'coverage.json').write_text(json.dumps(discovery.report, indent=2) + '\n')
    return report
