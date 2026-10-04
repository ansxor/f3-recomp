"""Differential test runner executing lowered C code against Musashi 68EC020."""
from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any

from .generator import (
    RawTestCase,
    generate_boundary_cases,
    generate_random_cases,
    load_external_instructions,
)
from .musashi_build import build_musashi, find_musashi_source


def _get_capstone_disassembler():
    # Ensure build/python is in sys.path if capstone is installed there
    py_build = Path("build/python").resolve()
    if py_build.exists() and str(py_build) not in sys.path:
        sys.path.insert(0, str(py_build))

    try:
        from capstone import Cs, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_020
    except ImportError as exc:
        raise ImportError(
            "Capstone 5.0.9 Python binding not found. Ensure PYTHONPATH=build/python"
        ) from exc

    md = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
    md.detail = True
    return md


@dataclass
class SupportedCase:
    raw: RawTestCase
    mnemonic: str
    op_str: str
    insn_size: int
    statements: list[str]


def run_differential(
    musashi_path: Path | str | None = None,
    output_dir: Path | str = Path("build/differential"),
    cases_count: int = 500,
    seed: int = 42,
    instructions_json: Path | str | None = None,
    filter_pattern: str | None = None,
    keep_temp: bool = False,
    verbose: bool = False,
    compile_only: bool = False,
) -> int:
    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    # 1. Locate and build Musashi reference
    musashi_source = find_musashi_source(musashi_path)
    if verbose:
        print(f"[runner] Using Musashi source at: {musashi_source}")
    musashi_info = build_musashi(musashi_source, output_dir, verbose=verbose)

    # 2. Initialize Capstone
    md = _get_capstone_disassembler()

    # 3. Import recomp.emitter.lower
    try:
        from recomp.emitter import lower
    except ImportError as exc:
        print(f"ERROR: Cannot import recomp.emitter.lower: {exc}", file=sys.stderr)
        return 2

    # 4. Generate test cases
    raw_cases: list[RawTestCase] = []
    # Always include targeted boundary cases
    boundary_cases = generate_boundary_cases()
    raw_cases.extend(boundary_cases)

    # Add randomized cases
    random_count = max(0, cases_count - len(boundary_cases))
    if random_count > 0:
        rand_cases = generate_random_cases(random_count, seed=seed, start_id=len(raw_cases) + 1)
        raw_cases.extend(rand_cases)

    # Add external instruction cases if provided
    if instructions_json:
        ext_cases = load_external_instructions(instructions_json, start_id=len(raw_cases) + 1)
        raw_cases.extend(ext_cases)

    if verbose:
        print(f"[runner] Generated {len(raw_cases)} raw candidate test cases.")

    # 5. Decode and Lower instructions
    supported_cases: list[SupportedCase] = []
    unsupported_accounting: Counter[str] = Counter()
    unsupported_reasons: list[dict[str, Any]] = []

    for raw in raw_cases:
        insns = list(md.disasm(raw.code_bytes, raw.initial_pc))
        if not insns:
            unsupported_accounting["<decode_failed>"] += 1
            unsupported_reasons.append({
                "id": raw.id, "name": raw.name, "bytes": raw.code_bytes.hex(),
                "reason": "Capstone could not decode instruction"
            })
            continue

        insn = insns[0]
        # Match filter if supplied
        if filter_pattern and filter_pattern.lower() not in insn.mnemonic.lower():
            continue

        selected = insns[:raw.instruction_count]
        if len(selected) != raw.instruction_count:
            raise ValueError(f"incomplete instruction sequence: {raw.name}")
        lowered = [lower(part) for part in selected]
        statements = None if any(part is None for part in lowered) else [
            line for part in lowered for line in ["{", *part, "}"]]
        if statements is None:
            unsupported_accounting[insn.mnemonic] += 1
            unsupported_reasons.append({
                "id": raw.id, "name": raw.name, "mnemonic": insn.mnemonic,
                "op_str": insn.op_str, "bytes": raw.code_bytes.hex(),
                "reason": "lower() returned None (unsupported/fallback)"
            })
        else:
            supported_cases.append(SupportedCase(
                raw=raw,
                mnemonic=insn.mnemonic,
                op_str=insn.op_str,
                insn_size=sum(part.size for part in selected),
                statements=statements,
            ))

    # Print Supported vs Unsupported Accounting
    supported_by_mnemonic = Counter(c.mnemonic for c in supported_cases)
    print("======================================================================")
    print("           68EC020 DIFFERENTIAL HARNESS ACCOUNTING                    ")
    print("======================================================================")
    print(f"Total candidate cases generated: {len(raw_cases)}")
    print(f"Supported for native lowering:   {len(supported_cases)}")
    print(f"Unsupported (honest fallback):   {sum(unsupported_accounting.values())}")
    print("\nSupported Mnemonics:")
    for mn, count in sorted(supported_by_mnemonic.items()):
        print(f"  {mn:<12} : {count:>4} cases")

    if unsupported_accounting:
        print("\nUnsupported / Fallback Mnemonics (Skipped, Not Counted Pass):")
        for mn, count in sorted(unsupported_accounting.items()):
            print(f"  {mn:<12} : {count:>4} cases")
    print("======================================================================\n")

    # Write accounting report JSON to output dir
    report_data = {
        "total_cases": len(raw_cases),
        "supported_count": len(supported_cases),
        "unsupported_count": sum(unsupported_accounting.values()),
        "supported_by_mnemonic": dict(sorted(supported_by_mnemonic.items())),
        "unsupported_by_mnemonic": dict(sorted(unsupported_accounting.items())),
        "unsupported_details": unsupported_reasons,
    }
    (output_dir / "differential_accounting.json").write_text(json.dumps(report_data, indent=2) + "\n")

    if not supported_cases:
        print("WARNING: No supported test cases to execute! Emitter lowered 0 instructions.", file=sys.stderr)
        return 1

    # 6. Generate C test runner source
    c_source_path = output_dir / "test_cases.c"
    with open(c_source_path, "w") as f:
        f.write("/* Auto-generated differential test runner. Do not commit. */\n")
        f.write("#include <stdio.h>\n")
        f.write("#include <stdlib.h>\n")
        f.write("#include <stdint.h>\n")
        f.write("#include <stdbool.h>\n")
        f.write("#include <string.h>\n")
        f.write("#include <f3rt/cpu_abi.h>\n")
        f.write('#include "recomp/cpu_ops.h"\n')
        f.write('#include "harness_abi.h"\n')
        f.write('#include "m68k.h"\n\n')

        # Recomp functions
        for i, sc in enumerate(supported_cases):
            f.write(f"static void recomp_case_{i:04d}(f3_cpu *cpu) {{\n")
            for stmt in sc.statements:
                f.write(f"    {stmt}\n")
            f.write("    f3_cc_flush(cpu);\n")
            f.write("}\n\n")

        # Memory init arrays
        for i, sc in enumerate(supported_cases):
            if sc.raw.initial_mem:
                for m_idx, item in enumerate(sc.raw.initial_mem):
                    hex_bytes = ", ".join(f"0x{b:02X}" for b in item.data)
                    f.write(f"static const uint8_t mem_init_data_{i:04d}_{m_idx}[] = {{ {hex_bytes} }};\n")
                f.write(f"static const MemInit mem_inits_{i:04d}[] = {{\n")
                for m_idx, item in enumerate(sc.raw.initial_mem):
                    f.write(f"    {{ 0x{item.address:08X}u, mem_init_data_{i:04d}_{m_idx}, {len(item.data)} }},\n")
                f.write("};\n\n")

        # Code bytes arrays
        for i, sc in enumerate(supported_cases):
            code_hex = ", ".join(f"0x{b:02X}" for b in sc.raw.code_bytes[:sc.insn_size])
            f.write(f"static const uint8_t code_bytes_{i:04d}[] = {{ {code_hex} }};\n")

        # Array of test cases
        f.write("\nstatic const TestCase s_test_cases[] = {\n")
        for i, sc in enumerate(supported_cases):
            name_esc = sc.raw.name.replace('"', '\\"')
            mn_esc = sc.mnemonic.replace('"', '\\"')
            op_esc = sc.op_str.replace('"', '\\"')
            bkind_esc = sc.raw.boundary_kind.replace('"', '\\"')
            mem_ptr = f"mem_inits_{i:04d}" if sc.raw.initial_mem else "NULL"
            mem_cnt = len(sc.raw.initial_mem)
            d_init = ", ".join(f"0x{d:08X}u" for d in sc.raw.initial_d)
            a_init = ", ".join(f"0x{a:08X}u" for a in sc.raw.initial_a)

            f.write("    {\n")
            f.write(f"        .id = {sc.raw.id},\n")
            f.write(f"        .instruction_count = {sc.raw.instruction_count},\n")
            f.write(f'        .name = "{name_esc}",\n')
            f.write(f'        .mnemonic = "{mn_esc}",\n')
            f.write(f'        .op_str = "{op_esc}",\n')
            f.write(f"        .code_bytes = code_bytes_{i:04d},\n")
            f.write(f"        .code_size = {sc.insn_size},\n")
            f.write(f"        .initial_pc = 0x{sc.raw.initial_pc:08X}u,\n")
            f.write(f"        .initial_d = {{ {d_init} }},\n")
            f.write(f"        .initial_a = {{ {a_init} }},\n")
            f.write(f"        .initial_sr = 0x{sc.raw.initial_sr:04X}u,\n")
            f.write(f"        .initial_mem = {mem_ptr},\n")
            f.write(f"        .num_mem_init = {mem_cnt},\n")
            f.write(f"        .recomp_fn = recomp_case_{i:04d},\n")
            f.write(f"        .is_boundary = {1 if sc.raw.is_boundary else 0},\n")
            f.write(f'        .boundary_kind = "{bkind_esc}",\n')
            f.write(f"        .seed = 0x{sc.raw.seed:016X}ULL,\n")
            f.write("    },\n")
        f.write("};\n\n")

        # main() function
        f.write(r"""
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    diff_musashi_setup();

    DiffEnv musashi_env, recomp_env;
    diff_env_init(&musashi_env);
    diff_env_init(&recomp_env);

    size_t total_cases = sizeof(s_test_cases) / sizeof(s_test_cases[0]);
    size_t passed = 0;
    size_t failed = 0;

    printf("Executing %zu differential tests (Lowered C vs Musashi 68EC020)...\n\n", total_cases);

    for (size_t i = 0; i < total_cases; i++) {
        const TestCase *tc = &s_test_cases[i];

        uint32_t m_d[8], m_a[8], m_pc;
        uint16_t m_sr;
        diff_musashi_run_case(tc, &musashi_env, m_d, m_a, &m_pc, &m_sr);

        f3_cpu r_cpu;
        diff_recomp_run_case(tc, &recomp_env, &r_cpu);
        f3_cc_flush(&r_cpu);

        StateDelta delta;
        bool match = diff_compare_states(tc, &musashi_env, m_d, m_a, m_pc, m_sr,
                                         &recomp_env, &r_cpu, &delta);
        if (match) {
            passed++;
        } else {
            failed++;
            printf("======================================================================\n");
            printf("FAIL: Case #%u [%s %s] (test: %s)\n", tc->id, tc->mnemonic, tc->op_str, tc->name);
            printf("Seed: 0x%016llX | Boundary: %s (%s)\n",
                   (unsigned long long)tc->seed,
                   tc->is_boundary ? "YES" : "NO", tc->boundary_kind);
            printf("Initial PC: 0x%08X | Initial SR: 0x%04X\n", tc->initial_pc, tc->initial_sr);
            printf("Differences:\n%s", delta.description);
            printf("======================================================================\n\n");
            if (failed >= 30) {
                printf("Stopping early after %zu failures.\n", failed);
                break;
            }
        }
    }

    diff_env_free(&musashi_env);
    diff_env_free(&recomp_env);

    printf("----------------------------------------------------------------------\n");
    printf("Differential Results: %zu passed, %zu failed (out of %zu tested)\n",
           passed, failed, passed + failed);
    printf("----------------------------------------------------------------------\n");

    return (failed == 0) ? 0 : 1;
}
""")

    if verbose:
        print(f"[runner] Wrote C test runner source: {c_source_path}")

    # 7. Compile test binary
    cc = os.environ.get("CC", "clang")
    bin_path = output_dir / "diff_runner"
    harness_c = Path(__file__).parent / "harness_abi.c"

    inc_flags = [
        "-Iinclude",
        "-I.",
        f"-I{Path(__file__).parent}",
    ]
    for inc in musashi_info.include_dirs:
        inc_flags.append(f"-I{inc}")

    cmd = [
        cc,
        "-O1",
        "-g",
        "-Wall", "-Wextra", "-Werror",
        "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
        "-o",
        str(bin_path),
        str(c_source_path),
        str(harness_c),
        str(musashi_info.library_path),
    ] + inc_flags

    if verbose:
        print(f"[runner] Compiling: {' '.join(cmd)}")

    comp_res = subprocess.run(cmd, capture_output=True, text=True)
    if comp_res.returncode != 0:
        print("ERROR: Compilation of differential test runner failed:", file=sys.stderr)
        print(comp_res.stderr, file=sys.stderr)
        return 1

    if compile_only:
        print(f"[runner] Successfully compiled differential runner at: {bin_path}")
        return 0

    # 8. Execute test binary
    if verbose:
        print(f"[runner] Executing test runner: {bin_path}")

    run_res = subprocess.run([str(bin_path)], text=True)
    return run_res.returncode
