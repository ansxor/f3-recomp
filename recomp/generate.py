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


def generate(rom: bytes, discovery, output: Path, config: dict,
             max_block_instructions: int = 32, blocks_per_file: int = 128) -> dict:
    if max_block_instructions < 1 or blocks_per_file < 1:
        raise ValueError("block and shard sizes must be positive")
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
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

    preamble = ('/* Generated from user-supplied ROM. Do not commit. */\n'
                '#include <f3rt/cpu_abi.h>\n#include "recomp/cpu_ops.h"\n')
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
                if position + 1 < len(pcs):
                    next_pc = pcs[position + 1]
                    # Includes exceptions and conditional control transfers;
                    # never execute fallthrough after an unexpected new PC.
                    lines.append(f'    if (cpu->pc != 0x{next_pc:08x}u || cpu->stopped || cpu->halted) {{ f3_cc_flush(cpu); return; }}')
            lines.append('}')
        lines.extend(['    f3_cc_flush(cpu);', '}\n'])
        shard.append('\n'.join(lines))
        if len(shard) == blocks_per_file or index + 1 == len(blocks):
            filename = f'blocks_{len(source_names):04d}.c'
            (output / filename).write_text(preamble + hook_declarations + '\n'.join(shard))
            source_names.append(filename)
            shard = []

    table.sort()
    if not table:
        raise ValueError("discovery produced no instructions")
    program = preamble + '#include "program.h"\n' + ''.join(declarations)
    program += 'static const f3_block translated_blocks[] = {\n'
    program += ''.join(f'    {{ 0x{pc:08x}u, {name} }},\n' for pc, name in table)
    program += ('};\nint f3_generated_register(f3_cpu *cpu) {\n'
                '    return f3_register_blocks(cpu, translated_blocks,\n'
                '        sizeof(translated_blocks) / sizeof(translated_blocks[0]));\n}\n')
    (output / 'program.c').write_text(program)
    (output / 'program.h').write_text(
        '#ifndef F3_GENERATED_PROGRAM_H\n#define F3_GENERATED_PROGRAM_H\n'
        '#include <f3rt/cpu_abi.h>\n#ifdef __cplusplus\nextern "C" {\n#endif\n'
        'int f3_generated_register(f3_cpu *cpu);\n'
        '#ifdef __cplusplus\n}\n#endif\n#endif\n')
    source_names.append('program.c')
    (output / 'sources.cmake').write_text('set(F3_GENERATED_SOURCES\n' + ''.join(
        f'    "${{CMAKE_CURRENT_LIST_DIR}}/{name}"\n' for name in source_names) + ')\n')
    # Used by runtime loader, not embedded in generated C or committed.
    (output / 'program.bin').write_bytes(rom)
    report = {
        "decoded_instructions": len(table), "native_instructions": sum(supported.values()),
        "fallback_instructions": sum(unsupported.values()), "native_blocks": len(blocks),
        "native_mnemonics": dict(sorted(supported.items())),
        "fallback_mnemonics": dict(sorted(unsupported.items())),
        "fallback_pcs": unsupported_pcs, "source_files": source_names,
        "max_block_instructions": max_block_instructions,
        "timing": "cycle-agnostic nominal instruction costs; not cycle-exact",
    }
    (output / 'lowering.json').write_text(json.dumps(report, indent=2) + '\n')
    (output / 'coverage.json').write_text(json.dumps(discovery.report, indent=2) + '\n')
    return report
