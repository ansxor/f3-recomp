"""Execute hot/cold transitions and shared exceptions using synthetic ROMs."""
from pathlib import Path
from types import SimpleNamespace
import os
import json
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
    path.write_text(r'''
#include <f3rt/cpu_abi.h>
static const f3_excluded_range *registered_exclusions;
static size_t registered_exclusion_count;
int f3_register_exclusions(f3_cpu *cpu, const f3_excluded_range *ranges, size_t count) {
    (void)cpu; registered_exclusions=ranges; registered_exclusion_count=count; return 1;
}
''' + driver)
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

    def test_main_shared_exception_entries_remain_counted_once_in_each_mode(self):
        rom = bytes.fromhex("a000f0007100")
        crc = zlib.crc32(rom)
        discovery = SimpleNamespace(instructions={}, blocks={}, invalid_pcs=[0, 2, 4], report={})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile = root / "entries.profile"
            BlockProfile({("main", crc): (0, len(rom))},
                         {("main", crc, 0): 1, ("main", crc, 4): 1}).write(profile)
            for mode in ("baseline", "tiers", "slim"):
                with self.subTest(mode=mode):
                    output = root / mode
                    options = {} if mode == "baseline" else {"profile_" + mode: profile}
                    report = generate(rom, discovery, output, {"discovery": {"coverage": "all_aligned"}}, **options)
                    execute(output, report, "#define SLIM " + str(int(mode == "slim")) + "\n" + r'''
#include <assert.h>
#include "program.h"
#include <f3rt/block_profile.h>
uint64_t counts[3];
uint64_t *f3_profile_main_counts=counts;
uint64_t *f3_profile_sound_counts;
static const f3_block *blocks;
static size_t n;
static unsigned exception;
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; blocks=table; n=count; return 1;
}
void f3_exception(f3_cpu *cpu, unsigned vector, uint32_t pc) {
    assert(pc==cpu->pc); exception=vector;
}
int main(void) {
    f3_cpu cpu={0}; f3_generated_register(&cpu);
    for(size_t i=0;i<n;++i) {
        cpu.pc=blocks[i].address; exception=0;
        blocks[i].execute(&cpu);
        assert(exception==(cpu.pc==0 ? 10u : cpu.pc==2 ? 11u : 4u));
    }
    assert(counts[0]==1 && counts[1]==(SLIM ? 0u : 1u) && counts[2]==1);
    return 0;
}
''')

    def test_main_exclusions_preserve_tier_complement_and_slim_exception_subset(self):
        rom = bytes.fromhex("70017202a000a000a001f000")
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        instructions = {insn.address: insn for insn in decoder.disasm(rom[:4], 0)}
        discovery = SimpleNamespace(instructions=instructions, blocks={},
                                    invalid_pcs=[6, 8, 10], report={})
        config = {"discovery": {"coverage": "all_aligned"}, "exclude": [
            {"start": 4, "end": 6, "reason": "synthetic data", "evidence": "fixture"}]}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile = root / "entries.profile"
            crc = zlib.crc32(rom)
            BlockProfile({("main", crc): (0, len(rom))},
                         {("main", crc, 0): 1, ("main", crc, 6): 1}).write(profile)
            for mode in ("tiers", "slim"):
                with self.subTest(mode=mode):
                    output = root / mode
                    report = generate(rom, discovery, output, config,
                                      **{"profile_" + mode: profile})
                    retained = 2 if mode == "slim" else 5
                    self.assertEqual((report["original_entries"], report["retained_entries"],
                                      report["registered_entries"], report["hot_entries"],
                                      report["cold_entries"], report["excluded_entries"]),
                                     (5, retained, retained, 2, 3, 1))
                    self.assertEqual(report["exception_entries"],
                                     {10: 1} if mode == "slim" else {10: 2, 11: 1})
                    inventory = json.loads((output / "profile_inventory.json").read_text())
                    self.assertEqual(inventory["entries"], [0, 2, 6, 8, 10])
                    self.assertEqual((inventory["retained_entries"], inventory["hot_entries"],
                                      inventory["cold_entries"]), (retained, 2, 3))
                    execute(output, report, "#define SLIM " + str(int(mode == "slim")) + "\n" + r'''
#include <assert.h>
#include "program.h"
#include <f3rt/block_profile.h>
uint64_t counts[6];
uint64_t *f3_profile_main_counts=counts;
uint64_t *f3_profile_sound_counts;
static const f3_block *blocks;
static size_t n;
static unsigned exception;
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; blocks=table; n=count; return 1;
}
void f3_exception(f3_cpu *cpu, unsigned vector, uint32_t pc) {
    assert(pc==cpu->pc); exception=vector;
}
int main(void) {
    f3_cpu cpu={0}; assert(f3_generated_register(&cpu));
    assert(registered_exclusion_count==1);
    assert(registered_exclusions[0].start==4 && registered_exclusions[0].end==6);
    const uint32_t expected[]={0, SLIM ? 6u : 2u, 6, 8, 10};
    assert(n==(SLIM ? 2u : 5u));
    for(size_t i=0;i<n;++i) {
        assert(blocks[i].address==expected[i]);
        cpu.pc=blocks[i].address; cpu.dispatch_deadline=UINT64_MAX; exception=0;
        blocks[i].execute(&cpu);
        if(expected[i]>=6) assert(exception==(expected[i]==10 ? 11u : 10u));
        else assert(cpu.pc==expected[i]+2);
    }
    assert(counts[0]==1 && counts[3]==1 && !counts[2]);
    assert(counts[1]==(SLIM ? 0u : 1u));
    assert(counts[4]==(SLIM ? 0u : 1u) && counts[5]==(SLIM ? 0u : 1u));
    return 0;
}
''')

    def test_main_rejects_excluded_discovery_and_profile_records_before_emission(self):
        rom = bytes.fromhex("7001a000")
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        instruction = next(decoder.disasm(rom[:2], 0))
        config = {"discovery": {"coverage": "all_aligned"}, "exclude": [
            {"start": 2, "end": 4, "reason": "synthetic data", "evidence": "fixture"}]}
        crc = zlib.crc32(rom)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for kind in ("decoded", "invalid"):
                with self.subTest(kind=kind):
                    discovery = SimpleNamespace(
                        instructions={0: instruction, **({2: instruction} if kind == "decoded" else {})},
                        blocks={}, invalid_pcs=[2] if kind == "invalid" else [], report={})
                    output = root / kind
                    with self.assertRaises(ValueError):
                        generate(rom, discovery, output, config)
                    self.assertFalse((output / "program.c").exists())
            discovery = SimpleNamespace(instructions={0: instruction}, blocks={},
                                        invalid_pcs=[], report={})
            for mode in ("tiers", "slim"):
                for kind in ("hit", "miss"):
                    with self.subTest(mode=mode, kind=kind):
                        profile = BlockProfile({("main", crc): (0, len(rom))},
                                               {("main", crc, 0): 1})
                        rows = profile.hits if kind == "hit" else profile.misses
                        rows["main", crc, 2] = 1
                        path = root / "contradictory.profile"
                        profile.write(path)
                        with self.assertRaises(ValueError):
                            generate(rom, discovery, root / (mode + kind), config,
                                     **{"profile_" + mode: path})

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

    def test_sound_exclusions_preserve_tier_complement_and_slim_shared_exceptions(self):
        rom = bytes.fromhex("4e714e71a000a001a002f000")
        base = 0xc00000
        crc = zlib.crc32(rom)
        config = {"exclude": [{"cpu": "sound", "start": base + 4, "end": base + 6,
                                "reason": "synthetic data", "evidence": "fixture"}]}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "entries.profile"
            BlockProfile({("sound", crc): (base, len(rom))},
                         {("sound", crc, base): 1, ("sound", crc, base + 6): 1}).write(path)
            for mode in ("tiers", "slim"):
                with self.subTest(mode=mode):
                    output = root / mode
                    report = compile_sound_rom(rom, output, config=config,
                                               **{"profile_" + mode: path})
                    retained = 2 if mode == "slim" else 5
                    self.assertEqual((report["original_entries"], report["retained_entries"],
                                      report["compiled_entries"], report["hot_entries"],
                                      report["cold_entries"], report["excluded_words"]),
                                     (5, retained, retained, 2, 3, 1))
                    inventory = json.loads((output / "profile_inventory.json").read_text())
                    self.assertEqual(inventory["entries"],
                                     [base + offset for offset in (0, 2, 6, 8, 10)])
                    self.assertEqual((inventory["retained_entries"], inventory["hot_entries"],
                                      inventory["cold_entries"]), (retained, 2, 3))
                    execute(output, report, "#define SLIM " + str(int(mode == "slim")) + "\n" + r'''
#include <assert.h>
#include "sound_program.h"
#include <f3rt/block_profile.h>
uint64_t counts[6];
uint64_t *f3_profile_main_counts;
uint64_t *f3_profile_sound_counts=counts;
static unsigned exception;
void f3_sound_exception(f3_cpu *cpu,unsigned vector,uint32_t pc) {
    assert(pc==cpu->pc); exception=vector;
}
void f3_sound_unsupported_pc(f3_cpu *cpu,uint32_t pc) { (void)cpu; (void)pc; assert(0); }
int main(void) {
    assert(f3_sound_excluded_count==1);
    assert(f3_sound_excluded_ranges[0].start==0xc00004u);
    assert(f3_sound_excluded_ranges[0].end==0xc00006u);
    const uint32_t expected[]={0, SLIM ? 6u : 2u, 6, 8, 10};
    assert(f3_sound_block_count==(SLIM ? 2u : 5u));
    for(size_t i=0;i<f3_sound_block_count;++i) {
        f3_cpu cpu={0}; cpu.pc=f3_sound_blocks[i].address;
        assert(cpu.pc==0xc00000u+expected[i]); exception=0;
        f3_sound_blocks[i].execute(&cpu);
        if(expected[i]>=6) assert(exception==(expected[i]==10 ? 11u : 10u));
        else assert(cpu.pc==0xc00000u+expected[i]+2 && cpu.cycles==4);
    }
    assert(counts[0]==1 && counts[3]==1 && !counts[2]);
    assert(counts[1]==(SLIM ? 0u : 1u));
    assert(counts[4]==(SLIM ? 0u : 1u) && counts[5]==(SLIM ? 0u : 1u));
    return 0;
}
''')

    def test_sound_rejects_contradictory_profiles_and_excluded_vectors(self):
        rom = bytes.fromhex("4e714e71a000")
        base = 0xc00000
        crc = zlib.crc32(rom)
        config = {"exclude": [{"cpu": "sound", "start": base + 4, "end": base + 6,
                                "reason": "synthetic data", "evidence": "fixture"}]}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for mode in ("tiers", "slim"):
                for kind in ("hit", "miss"):
                    for address in (base + 4, base + 5, 0x01c00004, 0x01c00005):
                        with self.subTest(mode=mode, kind=kind, address=address):
                            profile = BlockProfile({("sound", crc): (base, len(rom))},
                                                   {("sound", crc, base): 1})
                            rows = profile.hits if kind == "hit" else profile.misses
                            rows["sound", crc, address] = 1
                            path = root / "contradictory.profile"
                            profile.write(path)
                            with self.assertRaises(ValueError):
                                compile_sound_rom(rom, root / (mode + kind), config=config,
                                                  **{"profile_" + mode: path})
            for address in (base + 4, base + 5):
                with self.subTest(vector=address):
                    vector_rom = bytes.fromhex("4e714e71") + address.to_bytes(4, "big")
                    with self.assertRaises(ValueError):
                        compile_sound_rom(vector_rom, root / "vector", config=config)


if __name__ == "__main__":
    unittest.main()
