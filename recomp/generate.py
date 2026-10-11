"""Pack literal instruction lowering into interruptible C blocks.

Every decoded instruction is an entry point: a previously unresolved indirect
transfer can enter the middle of a discovered block without interpreting it.
"""
from __future__ import annotations

from collections import Counter
from pathlib import Path
import json
import zlib

from .emitter import lower, static_flow
from .liveness import analyse
from .discovery import parse_exclusions, exclusion_at
from .sprite_units import guard_lines, parse_sprite_units, digest as sprite_units_digest
from .timing import BASE_CYCLES

_RUNTIME_ABI_VERSION = 5


def _count_flag_stores(statements: list[str]) -> tuple[int, int]:
    """(lazy condition-code producers, eager X writes) among lowered statements."""
    lazy = sum(1 for statement in statements
               if 'cpu->cc_op = F3_CC_OP_' in statement and 'F3_CC_OP_NONE' not in statement)
    eager_x = sum(1 for statement in statements if 'cpu->sr = (cpu->sr & ~0x10u) |' in statement)
    return lazy, eager_x


def write_if_changed(path: Path, data: str | bytes, encoding: str = "utf-8") -> bool:
    """Write data to path only if the file does not exist or its content differs."""
    payload = data.encode(encoding) if isinstance(data, str) else data
    if path.is_file():
        try:
            if path.read_bytes() == payload:
                return False
        except OSError:
            pass
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    return True


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
    units = parse_sprite_units(config)
    unit_enter, unit_exit = units.hook_pcs()
    for unit in units.units:
        for kind, pcs in (("start", unit.starts), ("end", unit.ends)):
            for pc in pcs:
                region = exclusion_at(exclusions, pc)
                if pc not in discovery.instructions or region is not None:
                    raise ValueError(
                        f"Emit unit {unit.name!r} {kind} {pc:#x} is not a decoded "
                        "instruction" + (f" (excluded: {region.reason})" if region else ""))
                if pc not in retained:
                    raise ValueError(
                        f"Emit unit {unit.name!r} {kind} {pc:#x} is not retained by the "
                        "slim profile; add it to the profile or use tiers")
    for first, last in units.frame_writers:
        if last >= len(rom):
            raise ValueError(f"Frame writer range [{first:#x}, {last:#x}] lies outside the ROM")
    unit_guard = guard_lines(units)
    unit_hooks = Counter()
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
    table = []
    declarations = []
    pc_to_name = {}
    plan = []
    for tier, pcs in emission_blocks:
        name = f'f3_native_{pcs[0]:06x}'
        plan.append((tier, name, pcs))
        declarations.append(f'extern void {name}(f3_cpu *cpu);\n')
        table.extend((pc, name) for pc in pcs)
        for pc in pcs:
            pc_to_name[pc] = name
    # Backward flag liveness over the instructions emitted natively. Hook PCs flush
    # for a sandbox and fallback stubs run in the interpreter: both observe sr.
    # Lowering is independent of liveness for instructions without flag stores,
    # so those results are reused by the emission pass below.
    flag_producers = Counter()
    fallback_entries = set()
    unflagged = {}
    for pc in pc_to_name:
        statements = lower(discovery.instructions[pc])
        if statements is None:
            fallback_entries.add(pc)
            continue
        lazy, eager_x = _count_flag_stores(statements)
        flag_producers["lazy"] += lazy
        flag_producers["eager_x"] += eager_x
        if not lazy and not eager_x:
            unflagged[pc] = statements
    liveness = analyse(discovery.instructions, pc_to_name,
                       pinned=fallback_entries | unit_enter.keys() | unit_exit.keys())
    flag_emitted = Counter()
    source_names = []
    tier_sources = {"hot": [], "cold": []}
    supported, unsupported = Counter(), Counter()
    unsupported_pcs = []
    shards = {"hot": [], "cold": []}
    shard_calls = {"hot": set(), "cold": set()}
    shard_hooked = {"hot": False, "cold": False}
    # Codegen-only resolved targets for computed jmp/jsr; absent on hand-built
    # discovery objects, where no indirect chain is emitted.
    indirect_targets = getattr(discovery, "indirect_targets", None) or {}
    indirect_case_cap = 256
    indirect_sites = {"hot": 0, "cold": 0}
    indirect_cases = {"hot": 0, "cold": 0}
    indirect_capped = []
    remaining = Counter(tier for tier, _, _ in plan)
    for tier, name, pcs in plan:
        lines = [f'void {name}(f3_cpu *cpu) {{', '    switch (cpu->pc) {']
        lines.extend(f'    case 0x{pc:08x}u: goto L_{pc:06x};' for pc in pcs)
        lines.extend(['    default: return;', '    }'])
        for pc in pcs:
            insn = discovery.instructions[pc]
            lines.append(f'L_{pc:06x}: {{')
            # Exit before enter: a PC that ends one unit and starts another
            # closes the first invocation before opening the next.
            for unit_id in unit_exit.get(pc, ()):
                lines.append(f'    F3_UNIT_EXIT(cpu, {unit_id});')
                unit_hooks[(unit_id, "exit")] += 1
                shard_hooked[tier] = True
            for unit_id in unit_enter.get(pc, ()):
                lines.append(f'    F3_UNIT_ENTER(cpu, {unit_id});')
                unit_hooks[(unit_id, "enter")] += 1
                shard_hooked[tier] = True
            lines.append(f'    F3_PROFILE_HIT_MAIN(0x{pc:08x}u);')
            if pc in fallback_entries:
                statements = None
            else:
                statements = (unflagged.get(pc) or
                              lower(insn, liveness.live_out[pc]))
            if statements is None:
                unsupported[insn.mnemonic] += 1
                unsupported_pcs.append(pc)
                lines.extend(['    if (!f3_fallback(cpu)) cpu->halted = 1;',
                              '    return;'])
            else:
                supported[insn.mnemonic] += 1
                lazy, eager_x = _count_flag_stores(statements)
                flag_emitted["lazy"] += lazy
                flag_emitted["eager_x"] += eager_x
                lines.extend('    ' + statement for statement in statements)
                # Chain into a statically known successor instead of returning
                # to the dispatcher. One guard per successor proves the pending
                # boundary() would only republish the same deadline; anything
                # else (deadline reached, STOP/halt, trace bits, redirected pc,
                # untranslated target) falls back to the runtime unchanged.
                if statements[-1].strip() != 'return;':
                    fallthrough = pc + insn.size
                    flow = static_flow(insn)
                    successors = {}
                    if flow.falls_through:
                        # A sequential successor inside this function is what
                        # the old fallthrough check reached directly; only a
                        # cross-function transfer went through the dispatcher.
                        successors[fallthrough] = pc_to_name.get(fallthrough) != name
                    for target in flow.targets:
                        successors[target] = (target != fallthrough or
                                              pc_to_name.get(fallthrough) != name)
                    for target, dispatched in successors.items():
                        owner = pc_to_name.get(target)
                        if owner is None or target >= 0x200000:
                            continue
                        guard = (f'cpu->pc == 0x{target:08x}u && !cpu->stopped && '
                                 '!cpu->halted && ')
                        if dispatched:
                            # f3_dispatch refuses native blocks while trace bits
                            # are set; a chained transfer must refuse them too.
                            guard += '!(cpu->sr & 0xc000u) && '
                        guard += 'cpu->cycles < cpu->dispatch_deadline'
                        if owner == name:
                            lines.append(f'    if ({guard}) goto L_{target:06x};')
                        else:
                            shard_calls[tier].add(owner)
                            lines.append(f'    if ({guard}) {{ '
                                         f'F3_CHAIN({owner}, cpu); }}')
                    base_mnem = insn.mnemonic.split('.')[0].lower()
                    if base_mnem in ('jmp', 'jsr') and not flow.targets:
                        lines.append(f'    F3_PROFILE_INDIRECT_MAIN(0x{pc:08x}u, cpu->pc);')
                    candidates = indirect_targets.get(pc)
                    if candidates:
                        # A computed jmp/jsr always went through f3_dispatch, so
                        # the whole guard (trace bits included) must hold before
                        # chaining; every unmatched pc still returns there.
                        cases = []
                        for target in sorted(set(candidates)):
                            owner = pc_to_name.get(target)
                            if owner is None or target >= 0x200000:
                                continue
                            cases.append((target, owner))
                        if len(cases) > indirect_case_cap:
                            indirect_capped.append(
                                {"tier": tier, "pc": f"0x{pc:06x}", "cases": len(cases)})
                            cases = cases[:indirect_case_cap]
                        if cases:
                            indirect_sites[tier] += 1
                            indirect_cases[tier] += len(cases)
                            lines.append('    if (!cpu->stopped && !cpu->halted && '
                                         '!(cpu->sr & 0xc000u) && '
                                         'cpu->cycles < cpu->dispatch_deadline) {')
                            lines.append('        switch (cpu->pc) {')
                            for target, owner in cases:
                                if owner == name:
                                    lines.append(f'        case 0x{target:08x}u: '
                                                 f'goto L_{target:06x};')
                                else:
                                    shard_calls[tier].add(owner)
                                    lines.append(f'        case 0x{target:08x}u: '
                                                 f'F3_CHAIN({owner}, cpu);')
                            lines.append('        default: break;')
                            lines.append('        }')
                            lines.append('    }')
                    lines.append('    return;')
            lines.append('}')
        lines.append('}\n')
        shard = shards[tier]
        shard.append('\n'.join(lines))
        remaining[tier] -= 1
        if len(shard) == blocks_per_file or remaining[tier] == 0:
            filename = (f'blocks_{len(source_names):04d}.c' if profile_path is None else
                        f'blocks_{tier}_{len(tier_sources[tier]):04d}.c')
            # Other generated blocks reached by a chain may live in a later
            # file (or a different tier), so declare them before the bodies.
            externs = ''.join(f'extern void {callee}(f3_cpu *cpu);\n'
                              for callee in sorted(shard_calls[tier]))
            hooked = shard_hooked[tier]
            write_if_changed(output / filename,
                             preamble + (unit_guard if hooked else '') + externs + '\n'.join(shard))
            source_names.append(filename)
            tier_sources[tier].append(filename)
            shards[tier] = []
            shard_calls[tier] = set()
            shard_hooked[tier] = False

    for unit in units.units:
        for kind, pcs in (("enter", unit.starts), ("exit", unit.ends)):
            if unit_hooks[(unit.id, kind)] != len(pcs):
                raise ValueError(
                    f"Emit unit {unit.name!r}: {unit_hooks[(unit.id, kind)]} of "
                    f"{len(pcs)} {kind} hooks emitted")
    unit_report = {
        "digest": f"0x{sprite_units_digest(units):08x}",
        "units": {unit.name: {"id": unit.id, "enter_hooks": unit_hooks[(unit.id, "enter")],
                              "exit_hooks": unit_hooks[(unit.id, "exit")]}
                  for unit in units.units},
        "frame_writer_ranges": len(units.frame_writers),
    }

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
            write_if_changed(output / filename, preamble + ''.join(definitions))
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
    write_if_changed(output / 'program.c', program)
    write_if_changed(
        output / 'program.h',
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
    write_if_changed(output / 'sources.cmake', ''.join(
        f'set({variable}\n' + ''.join(
            f'    "${{CMAKE_CURRENT_LIST_DIR}}/{name}"\n' for name in names) + ')\n'
        for variable, names in source_variables.items()))
    # Used by runtime loader, not embedded in generated C or committed.
    write_if_changed(output / 'program.bin', rom)
    inventory = {
        "version": 1, "region": "main",
        "rom_crc32": f"{zlib.crc32(rom) & 0xffffffff:08x}",
        "base": 0, "size": len(rom), "entries": original_entries,
        "retained_entries": len(table), "hot_entries": len(hot),
        "cold_entries": len(original_entries) - len(hot),
        "generated_c_bytes": sum((output / name).stat().st_size for name in source_names),
    }
    write_if_changed(output / 'profile_inventory.json', json.dumps(inventory, indent=2) + '\n')
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
        "emit_units": unit_report,
        "flag_liveness": {
            "lazy_producers": flag_producers["lazy"],
            "lazy_producers_emitted": flag_emitted["lazy"],
            "eager_x_writes": flag_producers["eager_x"],
            "eager_x_writes_emitted": flag_emitted["eager_x"],
        },
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
        "indirect_chain_sites": indirect_sites["hot"] + indirect_sites["cold"],
        "indirect_chain_cases": indirect_cases["hot"] + indirect_cases["cold"],
        "indirect_chain_hot": {"sites": indirect_sites["hot"], "cases": indirect_cases["hot"]},
        "indirect_chain_cold": {"sites": indirect_sites["cold"], "cases": indirect_cases["cold"]},
        "indirect_chain_capped_sites": indirect_capped,
        "timing": "68EC020 reference instruction costs; runtime deadlines end native blocks at instruction boundaries",
    }
    write_if_changed(output / 'lowering.json', json.dumps(report, indent=2) + '\n')
    write_if_changed(output / 'coverage.json', json.dumps(discovery.report, indent=2) + '\n')
    emitted = set(source_names) | {
        "program.h",
        "sources.cmake",
        "program.bin",
        "profile_inventory.json",
        "lowering.json",
        "coverage.json",
    }
    for item in output.iterdir():
        if (item.is_file() and item.name not in emitted
                and not item.name.startswith(".") and not item.name.endswith(".stamp")):
            item.unlink()
    return report
