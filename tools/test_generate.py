"""Execute generated blocks across runtime deadlines using synthetic code only."""
from pathlib import Path
from types import SimpleNamespace
import os
import subprocess
import tempfile
import unittest

from capstone import Cs, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_020

from recomp.generate import generate


class DeadlineTests(unittest.TestCase):
    def test_deadline_yields_with_materialized_flags_and_resumes_inside_block(self):
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        # MOVEQ #-1,D0; ADDQ.W #1,D0; MOVEQ #1,D1.
        code = bytes.fromhex("70ff52407201")
        instructions = {insn.address: insn for insn in decoder.disasm(code, 0x400)}
        discovery = SimpleNamespace(instructions=instructions,
                                    blocks={0x400: list(instructions)}, report={})
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(bytes(0x400) + code, discovery, output, {})
            driver = output / "deadline.c"
            driver.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include "program.h"
static const f3_block *blocks;
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; (void)count;
    blocks = table;
    return 1;
}
int main(void) {
    f3_cpu cpu = {0};
    assert(f3_generated_register(&cpu));
    cpu.pc = 0x400; cpu.sr = 0x10; cpu.d[1] = 0x12345678;
    cpu.dispatch_deadline = 2;
    blocks[0].execute(&cpu);
    assert(cpu.pc == 0x402 && cpu.cycles == 2);
    assert(cpu.d[0] == UINT32_MAX && cpu.d[1] == 0x12345678);
    assert(cpu.sr == 0x18 && cpu.cc_op == 0);

    cpu.dispatch_deadline = 3;
    blocks[0].execute(&cpu);
    assert(cpu.pc == 0x404 && cpu.cycles == 4);
    assert(cpu.d[0] == 0xffff0000 && cpu.d[1] == 0x12345678);
    assert(cpu.sr == 0x15 && cpu.cc_op == 0);

    cpu.dispatch_deadline = UINT64_MAX;
    blocks[0].execute(&cpu);
    assert(cpu.pc == 0x406 && cpu.cycles == 6);
    assert(cpu.d[0] == 0xffff0000 && cpu.d[1] == 1);
    assert(cpu.sr == 0x10 && cpu.cc_op == 0);
    return 0;
}
''')
            executable = output / "deadline"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-Wall", "-Wextra", "-Werror", "-I", str(root),
                            "-I", str(root / "include"), "-I", str(output),
                            str(driver), *(str(output / name) for name in report["source_files"]),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
