"""Execute generated blocks across runtime deadlines using synthetic code only."""
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


class DeadlineTests(unittest.TestCase):
    def test_deadline_yields_with_materialized_flags_and_resumes_inside_block(self):
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        # MOVEQ #-1,D0; ADDQ.W #1,D0; MOVEQ #1,D1.
        code = bytes.fromhex("70ff52407201")
        instructions = {insn.address: insn for insn in decoder.disasm(code, 0x400)}
        discovery = SimpleNamespace(instructions=instructions,
                                    blocks={0x400: list(instructions)}, invalid_pcs=[], report={})
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
int f3_validate_main_rom(f3_cpu *cpu, size_t size, uint32_t crc) {
    (void)cpu; (void)size; (void)crc; return 1;
}
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; (void)count;
    blocks = table;
    return 1;
}
int f3_register_exclusions(f3_cpu *cpu, const f3_excluded_range *ranges, size_t count) {
    (void)cpu; (void)ranges; (void)count; return 1;
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

    def test_overlapping_entries_skip_extensions_and_preserve_deadline_resumption(self):
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        # The MOVE.L immediate contains two independently executable MOVEQs.
        code = bytes.fromhex("203c700172024e717403")
        instructions = {
            pc: next(decoder.disasm(code[pc - 0x400:], pc, count=1))
            for pc in range(0x400, 0x40a, 2)
        }
        discovery = SimpleNamespace(instructions=instructions, blocks={}, report={}, invalid_pcs=[])
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(bytes(0x400) + code, discovery, output,
                              {"discovery": {"coverage": "all_aligned"}})
            driver = output / "overlap.c"
            driver.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include "program.h"
static const f3_block *blocks;
static size_t block_count;
int f3_validate_main_rom(f3_cpu *cpu, size_t size, uint32_t crc) {
    (void)cpu; (void)size; (void)crc; return 1;
}
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; blocks = table; block_count = count; return 1;
}
int f3_register_exclusions(f3_cpu *cpu, const f3_excluded_range *ranges, size_t count) {
    (void)cpu; (void)ranges; (void)count; return 1;
}
static void dispatch(f3_cpu *cpu) {
    for (size_t i = 0; i < block_count; ++i)
        if (blocks[i].address == cpu->pc) { blocks[i].execute(cpu); return; }
    assert(0 && "missing computed entry");
}
int main(void) {
    f3_cpu cpu = {0};
    assert(f3_generated_register(&cpu));
    cpu.pc = 0x400; cpu.d[1] = 99; cpu.d[2] = 98;
    cpu.dispatch_deadline = 1;
    dispatch(&cpu);
    assert(cpu.pc == 0x406 && cpu.d[0] == 0x70017202);
    assert(cpu.d[1] == 99 && cpu.d[2] == 98 && cpu.cc_op == 0);
    cpu.dispatch_deadline = UINT64_MAX;
    while (cpu.pc != 0x40a) dispatch(&cpu);
    assert(cpu.d[1] == 99 && cpu.d[2] == 3);
    cpu.pc = 0x402; cpu.d[0] = 0; cpu.d[1] = 99;
    cpu.dispatch_deadline = cpu.cycles + 1;
    dispatch(&cpu);
    assert(cpu.pc == 0x404 && cpu.d[0] == 1 && cpu.d[1] == 99);
    assert(cpu.cc_op == 0 && (cpu.sr & 15) == 0);
    cpu.dispatch_deadline = UINT64_MAX;
    while (cpu.pc != 0x40a) dispatch(&cpu);
    assert(cpu.d[0] == 1 && cpu.d[1] == 2 && cpu.d[2] == 3);
    return 0;
}
''')
            executable = output / "overlap"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-Wall", "-Wextra", "-Werror", "-I", str(root),
                            "-I", str(root / "include"), "-I", str(output),
                            str(driver), *(str(output / name) for name in report["source_files"]),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_cmpm_consumes_eeprom_signature_and_aliased_operands(self):
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        # EEPROM validation in commandw uses CMPM.B (A2)+,(A0)+; DBNE D0.
        # Also exercise word/long operands and byte stack-register aliasing.
        code = bytes.fromhex("b10a56c8fffcb149b188bf0f")
        instructions = {insn.address: insn for insn in decoder.disasm(code, 0x400)}
        discovery = SimpleNamespace(instructions=instructions,
                                    blocks={pc: [pc] for pc in instructions},
                                    invalid_pcs=[], report={})
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(bytes(0x400) + code, discovery, output, {})
            driver = output / "cmpm.c"
            driver.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "program.h"
static const f3_block *blocks;
static size_t block_count, reads;
static uint8_t memory[0x4000];
static uint32_t addresses[32];
static uint8_t widths[32];
int f3_validate_main_rom(f3_cpu *cpu, size_t size, uint32_t crc) {
    (void)cpu; (void)size; (void)crc; return 1;
}
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; blocks = table; block_count = count; return 1;
}
int f3_register_exclusions(f3_cpu *cpu, const f3_excluded_range *ranges, size_t count) {
    (void)cpu; (void)ranges; (void)count; return 1;
}
static uint32_t read_memory(uint32_t address, unsigned width) {
    assert(reads < 32 && address + width <= sizeof(memory));
    addresses[reads] = address; widths[reads++] = width;
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) value = (value << 8) | memory[address + i];
    return value;
}
uint8_t f3_read8(f3_cpu *cpu, uint32_t address) {
    (void)cpu; return (uint8_t)read_memory(address, 1);
}
uint16_t f3_read16(f3_cpu *cpu, uint32_t address) {
    (void)cpu; return (uint16_t)read_memory(address, 2);
}
uint32_t f3_read32(f3_cpu *cpu, uint32_t address) {
    (void)cpu; return read_memory(address, 4);
}
static void dispatch(f3_cpu *cpu) {
    for (size_t i = 0; i < block_count; ++i)
        if (blocks[i].address == cpu->pc) { blocks[i].execute(cpu); return; }
    assert(0 && "missing CMPM entry");
}
int main(void) {
    f3_cpu cpu = {0};
    assert(f3_generated_register(&cpu));
    memcpy(memory + 0x1000, "TAITO", 5);
    memcpy(memory + 0x2000, "TAITO", 5);
    cpu.pc = 0x400; cpu.sr = 0x201b; cpu.d[0] = 4;
    cpu.a[0] = 0x1000; cpu.a[2] = 0x2000;
    while (cpu.pc != 0x406) dispatch(&cpu);
    assert(cpu.a[0] == 0x1005 && cpu.a[2] == 0x2005);
    assert(cpu.d[0] == 0xffff && cpu.sr == 0x2014 && reads == 10);
    for (unsigned i = 0; i < 5; ++i) {
        assert(addresses[i * 2] == 0x2000 + i && addresses[i * 2 + 1] == 0x1000 + i);
        assert(widths[i * 2] == 1 && widths[i * 2 + 1] == 1);
    }
    /* A changed EEPROM signature must stop at the first differing byte. */
    memory[0x2001] = 'X'; reads = 0;
    cpu.pc = 0x400; cpu.d[0] = 4; cpu.a[0] = 0x1000; cpu.a[2] = 0x2000;
    while (cpu.pc != 0x406) dispatch(&cpu);
    assert(cpu.a[0] == 0x1002 && cpu.a[2] == 0x2002 && cpu.d[0] == 3 && reads == 4);
    assert(cpu.sr == 0x2019); /* 'A' - 'X': N/C, preserved X. */

    reads = 0; cpu.pc = 0x406; cpu.sr = 0x2010;
    cpu.a[0] = 0x1000; cpu.a[1] = 0x2000;
    memory[0x1000] = 0x80; memory[0x1001] = 0;
    memory[0x2000] = 0x7f; memory[0x2001] = 0xff;
    dispatch(&cpu);
    assert(cpu.a[0] == 0x1002 && cpu.a[1] == 0x2002 && cpu.sr == 0x2012);
    assert(reads == 2 && addresses[0] == 0x2000 && addresses[1] == 0x1000);
    assert(widths[0] == 2 && widths[1] == 2);

    reads = 0; cpu.pc = 0x408; cpu.a[0] = 0x1000;
    memcpy(memory + 0x1000, "\0\0\0\1\0\0\0\2", 8);
    dispatch(&cpu);
    assert(cpu.a[0] == 0x1008 && cpu.sr == 0x2010 && reads == 2);
    assert(addresses[0] == 0x1000 && addresses[1] == 0x1004 && widths[0] == 4 && widths[1] == 4);

    reads = 0; cpu.pc = 0x40a; cpu.a[7] = 0x3000;
    memory[0x3000] = 0x80; memory[0x3002] = 0x7f;
    dispatch(&cpu);
    assert(cpu.a[7] == 0x3004 && cpu.sr == 0x201b && reads == 2);
    assert(addresses[0] == 0x3000 && addresses[1] == 0x3002 && widths[0] == 1 && widths[1] == 1);
    return 0;
}
''')
            executable = output / "cmpm"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-Wall", "-Wextra", "-Werror", "-I", str(root),
                            "-I", str(root / "include"), "-I", str(output),
                            str(driver), *(str(output / name) for name in report["source_files"]),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


ROOT = Path(__file__).resolve().parent.parent
DRIVER_PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include "program.h"
static const f3_block *blocks;
static size_t block_count;
static uint8_t memory[0x20000];
int f3_validate_main_rom(f3_cpu *cpu, size_t size, uint32_t crc) {
    (void)cpu; (void)size; (void)crc; return 1;
}
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; blocks = table; block_count = count; return 1;
}
int f3_register_exclusions(f3_cpu *cpu, const f3_excluded_range *ranges, size_t count) {
    (void)cpu; (void)ranges; (void)count; return 1;
}
uint8_t f3_read8(f3_cpu *cpu, uint32_t address) { (void)cpu; return memory[address & 0x1ffffu]; }
uint16_t f3_read16(f3_cpu *cpu, uint32_t address) {
    (void)cpu;
    return (uint16_t)((memory[address & 0x1ffffu] << 8) | memory[(address + 1u) & 0x1ffffu]);
}
uint32_t f3_read32(f3_cpu *cpu, uint32_t address) {
    return ((uint32_t)f3_read16(cpu, address) << 16) | f3_read16(cpu, address + 2u);
}
void f3_write8(f3_cpu *cpu, uint32_t address, uint8_t value) {
    (void)cpu; memory[address & 0x1ffffu] = value;
}
void f3_write16(f3_cpu *cpu, uint32_t address, uint16_t value) {
    (void)cpu; memory[address & 0x1ffffu] = (uint8_t)(value >> 8);
    memory[(address + 1u) & 0x1ffffu] = (uint8_t)value;
}
void f3_write32(f3_cpu *cpu, uint32_t address, uint32_t value) {
    f3_write16(cpu, address, (uint16_t)(value >> 16));
    f3_write16(cpu, address + 2u, (uint16_t)value);
}
static int dispatch(f3_cpu *cpu) {
    for (size_t i = 0; i < block_count; ++i)
        if (blocks[i].address == cpu->pc) { blocks[i].execute(cpu); return 1; }
    return 0;
}
static f3_cpu fresh_cpu(void) {
    f3_cpu cpu = {0};
    assert(f3_generated_register(&cpu));
    return cpu;
}
'''


def _run_driver(output: Path, report: dict, body: str) -> None:
    source = output / "chaining.c"
    source.write_text(DRIVER_PRELUDE + body)
    executable = output / "chaining"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra",
                    "-Werror", "-I", str(ROOT), "-I", str(ROOT / "include"),
                    "-I", str(output), str(source),
                    *(str(output / name) for name in report["source_files"]),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


def _synthetic(decoder, chunks):
    """Decode (address, hex) chunks and pack them into one discovery object."""
    instructions = {}
    blocks = {}
    for address, code in chunks:
        decoded = list(decoder.disasm(bytes.fromhex(code), address))
        for insn in decoded:
            instructions[insn.address] = insn
        blocks[address] = [insn.address for insn in decoded]
    rom = bytearray(max(address + len(code) // 2 for address, code in chunks))
    for address, code in chunks:
        rom[address:address + len(code) // 2] = bytes.fromhex(code)
    return bytes(rom), SimpleNamespace(instructions=instructions, blocks=blocks,
                                       invalid_pcs=[], report={})


def _indirect_fixture(decoder):
    """Two computed jmp forms, their target blocks, and both jump tables.

    PC-index form mirrors the measured ROM idiom (load a 16-bit offset from a
    table, then jump with it): the offset table at 0x3f0 maps 0x16 -> 0x406
    (same block function), 0x110 -> 0x500, 0x212 -> 0x602 (deliberately left
    unresolved). The register-staged form stages absolute longwords from a
    second table: 0x606 (same function) and 0x500.
    """
    code = {
        0x3FE: "303b00f0" + "4efb00ec" + "72214e714e71",
        0x600: "207b00084ed0" + "72214e71",
        0x500: "7427",
        0x602: "7629",
    }
    tables = {0x3F0: "001601100000", 0x60A: "0000060600000500"}
    instructions, blocks = {}, {}
    for address, hexcode in code.items():
        decoded = list(decoder.disasm(bytes.fromhex(hexcode), address))
        for insn in decoded:
            instructions[insn.address] = insn
        blocks[address] = [insn.address for insn in decoded]
    rom = bytearray(0x700)
    for address, hexcode in {**tables, **code}.items():
        rom[address:address + len(hexcode) // 2] = bytes.fromhex(hexcode)
    discovery = SimpleNamespace(
        instructions=instructions, blocks=blocks, invalid_pcs=[], report={},
        indirect_targets={0x402: [0x406, 0x500], 0x604: [0x606, 0x500]})
    return bytes(rom), discovery


class BlockChainingTests(unittest.TestCase):
    def test_backward_loop_chains_in_one_call_with_unchained_cycle_sum(self):
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        # MOVEQ #4,D0; NOP; DBRA D0,-4 (5 iterations); MOVEQ #7,D1.
        rom, discovery = _synthetic(decoder, [(0x400, "70044e7151c8fffc7207")])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(rom, discovery, output, {})
            _run_driver(output, report, r'''
int main(void) {
    f3_cpu chained = fresh_cpu(), stepped = fresh_cpu();
    chained.pc = 0x400; chained.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&chained));                    /* one call runs the whole loop */
    assert(chained.pc == 0x40a);
    assert(chained.d[0] == 0x0000ffffu && chained.d[1] == 7 && chained.cc_op == 0);
    stepped.pc = 0x400; stepped.dispatch_deadline = 0;
    int calls = 0;
    while (stepped.pc != 0x40a) { assert(dispatch(&stepped)); ++calls; }
    assert(calls == 12);                           /* 5 NOP + 5 DBRA + 2 MOVEQ */
    assert(stepped.d[0] == chained.d[0] && stepped.d[1] == chained.d[1]);
    assert(chained.cycles == stepped.cycles);
    return 0;
}
''')

    def test_chain_yields_at_first_deadline_boundary_and_resumes_identically(self):
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        rom, discovery = _synthetic(decoder, [(0x400, "70044e7151c8fffc7207")])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(rom, discovery, output, {})
            _run_driver(output, report, r'''
int main(void) {
    f3_cpu probe = fresh_cpu(), yielding = fresh_cpu(), resumed = fresh_cpu();
    probe.pc = 0x400; probe.dispatch_deadline = 0;
    uint32_t stop_pc = 0; uint64_t stop_cycles = 0;
    while (probe.pc != 0x40a) {
        assert(dispatch(&probe));                  /* one instruction per call */
        if (probe.cycles >= 16) { stop_pc = probe.pc; stop_cycles = probe.cycles; break; }
    }
    assert(stop_pc != 0);
    yielding.pc = 0x400; yielding.dispatch_deadline = 16;
    assert(dispatch(&yielding));                    /* chains until the deadline */
    assert(yielding.pc == stop_pc && yielding.cycles == stop_cycles);
    assert(yielding.cc_op == 0);                    /* flags materialized at yield */
    yielding.dispatch_deadline = UINT64_MAX;
    while (yielding.pc != 0x40a) assert(dispatch(&yielding));
    resumed.pc = 0x400; resumed.dispatch_deadline = 0;
    while (resumed.pc != 0x40a) assert(dispatch(&resumed));
    assert(yielding.d[0] == resumed.d[0] && yielding.d[1] == resumed.d[1]);
    assert(yielding.cycles == resumed.cycles);
    return 0;
}
''')

    def test_cross_function_transfer_chains_but_trace_or_stop_refuses(self):
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        # BSR 0x400 -> 0x500 and BRA 0x600 -> 0x500 reach a different block
        # function; 0x500/0x502 (MOVEQ #7,D1; NOP) proves the transfer ran.
        rom, discovery = _synthetic(decoder, [
            (0x400, "610000fe7409"),
            (0x500, "72074e71"),
            (0x600, "6000fefe"),
        ])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(rom, discovery, output, {})
            _run_driver(output, report, r'''
int main(void) {
    f3_cpu cpu = fresh_cpu();
    cpu.pc = 0x400; cpu.a[7] = 0x10000; cpu.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&cpu));
    assert(cpu.pc == 0x504 && cpu.d[1] == 7 && cpu.a[7] == 0xfffcu);

    f3_cpu traced = fresh_cpu();
    traced.pc = 0x400; traced.sr = 0x8000; traced.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&traced));
    assert(traced.pc == 0x500 && traced.d[1] == 0);

    f3_cpu stopped = fresh_cpu();
    stopped.pc = 0x400; stopped.stopped = 1; stopped.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&stopped));
    assert(stopped.pc == 0x500 && stopped.d[1] == 0);

    f3_cpu jumped = fresh_cpu();
    jumped.pc = 0x600; jumped.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&jumped));
    assert(jumped.pc == 0x504 && jumped.d[1] == 7);
    return 0;
}
''')

    def test_unretained_chain_target_returns_to_dispatcher(self):
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        rom, discovery = _synthetic(decoder, [
            (0x400, "610000fe7409"),
            (0x500, "72074e71"),
        ])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # Baseline: 0x500 is not decoded at all, so it is not a table entry.
            undecoded = root / "undecoded"
            undecoded_discovery = SimpleNamespace(
                instructions={pc: insn for pc, insn in discovery.instructions.items()
                              if pc < 0x500},
                blocks={0x400: [0x400, 0x404]}, invalid_pcs=[], report={})
            report = generate(rom, undecoded_discovery, undecoded, {})
            _run_driver(undecoded, report, r'''
int main(void) {
    f3_cpu cpu = fresh_cpu();
    cpu.pc = 0x400; cpu.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&cpu));
    assert(cpu.pc == 0x500 && cpu.d[1] == 0);
    return 0;
}
''')
            # Slim: 0x500 exists in discovery but is not a retained entry.
            profile = root / "entries.profile"
            crc = zlib.crc32(rom)
            BlockProfile({("main", crc): (0, len(rom))},
                         {("main", crc, 0x400): 1}).write(profile)
            slim = root / "slim"
            report = generate(rom, discovery, slim, {}, profile_slim=profile)
            self.assertEqual(report["retained_entries"], 1)
            _run_driver(slim, report, r'''
int main(void) {
    f3_cpu cpu = fresh_cpu();
    cpu.pc = 0x400; cpu.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&cpu));
    assert(cpu.pc == 0x500 && cpu.d[1] == 0);
    return 0;
}
''')


class IndirectChainTests(unittest.TestCase):
    def test_table_indexes_chain_in_one_call_with_dispatcher_cycle_sum(self):
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        rom, discovery = _indirect_fixture(decoder)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(rom, discovery, output, {})
            assert report["indirect_chain_sites"] == 2
            assert report["indirect_chain_cases"] == 4
            _run_driver(output, report, r'''
static void seed_tables(f3_cpu *cpu) {
    /* Word-offset table read by MOVE.W at 0x3fe, longword table by MOVEA.L. */
    f3_write16(cpu, 0x3f0, 0x0016);
    f3_write16(cpu, 0x3f2, 0x0110);
    f3_write16(cpu, 0x3f4, 0x0212);
    f3_write32(cpu, 0x60a, 0x00000606);
    f3_write32(cpu, 0x60e, 0x00000500);
}
int main(void) {
    /* MOVE.W/JMP $3f0(pc,d0.w): index 0 -> 0x406 (same function), 2 -> 0x500. */
    f3_cpu chained = fresh_cpu(), stepped = fresh_cpu();
    seed_tables(&chained);
    chained.pc = 0x3fe; chained.d[0] = 0; chained.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&chained));                  /* one call runs the whole chain */
    assert(chained.pc == 0x40c);
    assert(chained.d[1] == 0x21 && chained.d[0] == 0x16 && chained.cc_op == 0);
    seed_tables(&stepped);
    stepped.pc = 0x3fe; stepped.d[0] = 0; stepped.dispatch_deadline = 0;
    int calls = 0;
    while (stepped.pc != 0x40c && calls < 32) { assert(dispatch(&stepped)); ++calls; }
    assert(stepped.pc == 0x40c && calls > 1);
    assert(stepped.d[1] == chained.d[1] && stepped.d[0] == chained.d[0]);
    assert(stepped.cycles == chained.cycles);

    f3_cpu crossed = fresh_cpu();
    seed_tables(&crossed);
    crossed.pc = 0x3fe; crossed.d[0] = 2; crossed.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&crossed));
    assert(crossed.pc == 0x502 && crossed.d[2] == 0x27);
    assert(crossed.d[1] == 0);                   /* 0x406 never ran */

    /* MOVEA.L $60a(pc,d0.w),A0; JMP (A0) stages the same two targets. */
    f3_cpu staged = fresh_cpu(), staged_stepped = fresh_cpu();
    seed_tables(&staged);
    staged.pc = 0x600; staged.d[0] = 0; staged.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&staged));
    assert(staged.pc == 0x60a && staged.a[0] == 0x606 && staged.d[1] == 0x21);
    seed_tables(&staged_stepped);
    staged_stepped.pc = 0x600; staged_stepped.d[0] = 0;
    staged_stepped.dispatch_deadline = 0;
    calls = 0;
    while (staged_stepped.pc != 0x60a && calls < 32) {
        assert(dispatch(&staged_stepped)); ++calls;
    }
    assert(staged_stepped.pc == 0x60a && staged_stepped.a[0] == staged.a[0]);
    assert(staged_stepped.d[1] == staged.d[1] && staged_stepped.cycles == staged.cycles);

    f3_cpu staged_cross = fresh_cpu();
    seed_tables(&staged_cross);
    staged_cross.pc = 0x600; staged_cross.d[0] = 4;
    staged_cross.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&staged_cross));
    assert(staged_cross.pc == 0x502 && staged_cross.a[0] == 0x500);
    assert(staged_cross.d[2] == 0x27 && staged_cross.d[1] == 0);
    return 0;
}
''')

    def test_unknown_index_and_trace_or_stop_refuse_the_chain(self):
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        rom, discovery = _indirect_fixture(decoder)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(rom, discovery, output, {})
            _run_driver(output, report, r'''
static void seed_tables(f3_cpu *cpu) {
    f3_write16(cpu, 0x3f0, 0x0016);
    f3_write16(cpu, 0x3f2, 0x0110);
    f3_write16(cpu, 0x3f4, 0x0000);
    f3_write32(cpu, 0x60a, 0x00000606);
    f3_write32(cpu, 0x60e, 0x00000500);
}
int main(void) {
    /* Index 4 loads offset 0: pc 0x3f0 is not a resolved case, so the transfer
       returns to the dispatcher with the MOVE.W flags materialised. */
    f3_cpu fallback = fresh_cpu();
    seed_tables(&fallback);
    fallback.pc = 0x3fe; fallback.d[0] = 4; fallback.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&fallback));
    assert(fallback.pc == 0x3f0 && fallback.d[0] == 0);
    assert(fallback.cc_op == 0 && (fallback.sr & 0xfu) == 4);   /* Z, no lazy flags */

    /* Trace (T0) and STOP each refuse the chained indirect transfer: entering
       at the JMP directly leaves the switch guard as the only decision. */
    f3_cpu traced = fresh_cpu();
    traced.pc = 0x402; traced.d[0] = 0x16; traced.sr = 0x8000;
    traced.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&traced));
    assert(traced.pc == 0x406 && traced.cc_op == 0);

    f3_cpu stopped = fresh_cpu();
    stopped.pc = 0x402; stopped.d[0] = 0x16; stopped.stopped = 1;
    stopped.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&stopped));
    assert(stopped.pc == 0x406 && stopped.cc_op == 0);
    return 0;
}
''')


if __name__ == "__main__":
    unittest.main()
