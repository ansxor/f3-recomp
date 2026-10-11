# Extending the emitter

An emitter change must preserve instruction semantics, access order, flags, and scheduling. Mnemonic recognition alone does not prove support.

## Find the correct layer

| Change | File |
| --- | --- |
| New instruction family or operand form | `recomp/emitter.py` |
| Shared arithmetic or flag semantics | `recomp/cpu_ops.h` |
| Bitfield window behavior | `recomp/bitfield.h` |
| Instruction cycle adjustment | `recomp/timing.py` |
| Primary opcode timing descriptors | `recomp/68020_cycles.csv` |
| Entry grouping, continuation, report, or sharding | `recomp/generate.py` |
| Discovery and ROM/config input | `recomp/discovery.py` |
| Command-line option | `recomp/__main__.py` |

The package `__init__.py` contains only its description. `pyproject.toml` pins Capstone 5.0.9.

## Inspect a synthetic instruction

Use bytes from a documented instruction encoding, not copied game data. Enable Capstone detail mode before calling `lower`.

```python
from capstone import Cs, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_020
from recomp.emitter import lower

md = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
md.detail = True
insn = next(md.disasm(bytes.fromhex("1001"), 0x1000, count=1))
print(insn.mnemonic, insn.op_str)
print("\n".join(lower(insn) or ["unsupported"]))
```

Inspect primary and extension words when Capstone omits information. Existing handlers use raw words for full EAs, decimal forms, and long division.

## Preserve the lowering contract

1. Decode all required operands before returning statements.
2. Return `None` for unsupported operand forms.
3. Capture aliased source values before destination changes.
4. Use ABI memory callbacks for guest memory.
5. Preserve partial data-register bits.
6. Apply sign extension where the instruction requires it.
7. Keep X current when NZVC remains pending.
8. Flush before calls that require canonical SR.
9. Update PC and cumulative cycles on normal completion.
10. Return after an exception or terminal control operation.

Use unsigned arithmetic for guest wraparound. Guard host division and shift edge cases instead of relying on undefined C behavior.

A helper that changes only Z must first materialize pending flags. A full NZVC producer can replace them without a flush.

## Check consumers

[`tools/compile_sound.py`](https://github.com/ansxor/f3-recomp/blob/main/tools/compile_sound.py) imports `lower` and `_decode_ea`. Main-CPU changes can affect sound generation.

[`tools/differential/generator.py`](https://github.com/ansxor/f3-recomp/blob/main/tools/differential/generator.py) supplies deterministic instruction cases. The runner builds native code and compares the reference state.

Cover boundaries, aliasing, side-effect order, exception behavior, and elapsed cycles. Include multi-instruction cases for lazy flag transitions.

A state comparison that synchronizes clocks cannot establish native device timing. Use actual native execution for scheduling and visible-output claims.

See [Differential testing](/developer/testing/differential) for the existing workflow.

## Generated-library build

[`recomp/CMakeLists.txt`](https://github.com/ansxor/f3-recomp/blob/main/recomp/CMakeLists.txt) requires `F3_GENERATED_DIR/sources.cmake`.

It builds the listed sources into `f3_recompiled`, requires C11, and exposes ABI, repository, and generated-directory include paths.

Clang and GNU builds use `-Wall -Wextra -Werror`. ABI changes require coordinated regeneration and runtime updates.

Never commit ROM-derived generated C or `program.bin`. Keep reusable implementation changes separate from local generated artifacts.
