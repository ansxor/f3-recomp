"""Execute hot/cold transitions and shared exceptions using synthetic ROMs."""
from pathlib import Path
from types import SimpleNamespace
import os
import subprocess
import tempfile
import unittest
import zlib

from capstone import Cs, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_020
from recomp.block_profile import BlockProfile
from recomp.generate import generate
from tools.compile_sound import compile_sound_rom

ROOT = Path(__file__).resolve().parents[1]


def execute(output, report, driver):
    path = output / "driver.c"
    path.write_text(driver)
    binary = output / "driver"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-DF3_PROFILE_INSTRUMENT=1", "-I", str(ROOT), "-I", str(ROOT / "include"),
                    "-I", str(output), str(path), *(str(output / name) for name in report["source_files"]),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


class CodegenProfileTests(unittest.TestCase):
    def test_cross_tier_successors_deadlines_indirect_entries_and_slim_removal(self):
        code = bytes.fromhex("700172027403")
        rom = bytes(0x400) + code
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        instructions = {insn.address: insn for insn in decoder.disasm(code, 0x400)}
        discovery = SimpleNamespace(instructions=instructions, blocks={}, invalid_pcs=[], report={})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile = root / "entries.profile"
            crc = zlib.crc32(rom)
            BlockProfile({("main", crc): (0, len(rom))},
                         {("main", crc, 0x400): 1, ("main", crc, 0x404): 1}).write(profile)
            for mode in ("baseline", "tiers", "slim"):
                with self.subTest(mode=mode):
                    output = root / mode
                    options = {} if mode == "baseline" else {"profile_" + mode: profile}
                    report = generate(rom, discovery, output, {"discovery": {"coverage": "all_aligned"}}, **options)
                    driver = r'''
#include <assert.h>
#include "program.h"
#include <f3rt/block_profile.h>
uint64_t counts[0x1000];
uint64_t *f3_profile_main_counts = counts;
uint64_t *f3_profile_sound_counts;
static const f3_block *blocks;
static size_t n;
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; blocks=table; n=count; return 1;
}
static int dispatch(f3_cpu *cpu) {
    for(size_t i=0;i<n;++i) if(blocks[i].address==cpu->pc) { blocks[i].execute(cpu); return 1; }
    return 0;
}
int main(void) {
    f3_cpu cpu={0}; f3_generated_register(&cpu);
    cpu.pc=0x400; cpu.dispatch_deadline=1;
    assert(dispatch(&cpu)); assert(cpu.pc==0x402 && cpu.d[0]==1 && !cpu.d[1]);
    assert(counts[0x400/2]==1 && !counts[0x402/2]);
    cpu.dispatch_deadline=UINT64_MAX;
#if SLIM
    assert(n==2 && !dispatch(&cpu));
#else
    while(cpu.pc!=0x406) assert(dispatch(&cpu));
    assert(cpu.d[1]==2 && cpu.d[2]==3);
    assert(counts[0x402/2]==1 && counts[0x404/2]==1);
#endif
    cpu.pc=0x404; cpu.d[2]=99; cpu.dispatch_deadline=cpu.cycles+1;
    assert(dispatch(&cpu)); assert(cpu.pc==0x406 && cpu.d[2]==3 && !cpu.cc_op);
    assert(counts[0x404/2]==(SLIM ? 1u : 2u));
    counts[0x404/2]=UINT64_MAX; cpu.pc=0x404;
    assert(dispatch(&cpu)); assert(counts[0x404/2]==UINT64_MAX);
    return 0;
}
'''
                    execute(output, report, "#define SLIM " + str(int(mode == "slim")) + "\n" + driver)

    def test_redirecting_hook_entry_is_recorded_before_guest_instruction_is_skipped(self):
        rom = bytes(0x400) + bytes.fromhex("7001")
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        instructions = {insn.address: insn for insn in decoder.disasm(rom[0x400:], 0x400)}
        discovery = SimpleNamespace(instructions=instructions, blocks={}, invalid_pcs=[], report={})
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(rom, discovery, output, {
                "discovery": {"coverage": "all_aligned"},
                "hooks": [{"address": 0x400, "symbol": "redirect_hook"}],
            })
            execute(output, report, r'''
#include <assert.h>
#include "program.h"
#include <f3rt/block_profile.h>
uint64_t counts[0x1000];
uint64_t *f3_profile_main_counts = counts;
uint64_t *f3_profile_sound_counts;
static const f3_block *blocks;
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; (void)count; blocks=table; return 1;
}
void redirect_hook(f3_cpu *cpu) { cpu->pc=0x600; }
int main(void) {
    f3_cpu cpu={0}; f3_generated_register(&cpu);
    cpu.pc=0x400; cpu.dispatch_deadline=UINT64_MAX;
    blocks[0].execute(&cpu);
    assert(cpu.pc==0x600 && cpu.d[0]==0 && cpu.cycles==0);
    assert(counts[0x400/2]==1);
    return 0;
}
''')

    def test_sound_shared_exceptions_are_counted_once_and_slim_table_is_sparse(self):
        rom = bytes.fromhex("4e714e71a000f0004afc")
        crc = zlib.crc32(rom)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile = root / "entries.profile"
            BlockProfile({("sound", crc): (0xc00000, len(rom))},
                         {("sound", crc, 0xc00000): 1, ("sound", crc, 0xc00004): 1}).write(profile)
            for mode in ("baseline", "tiers", "slim"):
                with self.subTest(mode=mode):
                    output = root / mode
                    options = {} if mode == "baseline" else {"profile_" + mode: profile}
                    report = compile_sound_rom(rom, output, **options)
                    driver = r'''
#include <assert.h>
#include "sound_program.h"
#include <f3rt/block_profile.h>
uint64_t counts[5];
uint64_t *f3_profile_main_counts;
uint64_t *f3_profile_sound_counts=counts;
static unsigned exception;
void f3_sound_exception(f3_cpu *cpu,unsigned vector,uint32_t pc) {
    (void)cpu; (void)pc; exception=vector;
}
void f3_sound_unsupported_pc(f3_cpu *cpu,uint32_t pc) { (void)cpu; (void)pc; assert(0); }
int main(void) {
    assert(f3_sound_block_count==(SLIM ? 2u : 5u));
    for(size_t i=0;i<f3_sound_block_count;++i) {
        f3_cpu cpu={0}; cpu.pc=f3_sound_blocks[i].address;
        unsigned index=(cpu.pc-0xc00000u)/2;
        exception=0; f3_sound_blocks[i].execute(&cpu);
        assert(counts[index]==1);
        if(index<2) assert(cpu.pc==0xc00000u+(index+1)*2 && cpu.cycles==4);
        else assert(exception==(index==2 ? 10u : index==3 ? 11u : 4u));
    }
    if(SLIM) assert(!counts[1] && !counts[3] && !counts[4]);
    return 0;
}
'''
                    execute(output, report, "#define SLIM " + str(int(mode == "slim")) + "\n" + driver)


if __name__ == "__main__":
    unittest.main()
