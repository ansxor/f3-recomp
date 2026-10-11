"""Execute generated blocks across runtime deadlines using synthetic code only."""
from pathlib import Path
from types import SimpleNamespace
import os
import random
import re
import subprocess
import tempfile
import unittest
import zlib

from capstone import Cs, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_020

from recomp.block_profile import BlockProfile
from recomp.emitter import CCR_ALL, CCR_N, CCR_NZVC, CCR_V, CCR_X, CCR_Z, lower
from recomp.generate import generate
from recomp.liveness import analyse, condition_flags, flag_effects


class DeadlineTests(unittest.TestCase):
    def test_deadline_yields_with_materialized_flags_and_resumes_inside_block(self):
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        # MOVEQ #-1,D0; ADDQ.W #1,D0; MOVEQ #1,D1, each followed by MOVE CCR,Dn so
        # that the flags a deadline can leave pending are live and must be produced.
        code = bytes.fromhex("70ff42c3524042c4720142c5")
        instructions = {insn.address: insn for insn in decoder.disasm(code, 0x400)}
        discovery = SimpleNamespace(instructions=instructions,
                                    blocks={0x400: list(instructions)}, invalid_pcs=[], report={})
        root = Path(__file__).resolve().parents[2]
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
    f3_cc_flush(&cpu);
    assert(cpu.sr == 0x18 && cpu.cc_op == 0);

    cpu.dispatch_deadline = cpu.cycles + 1;            /* MOVE CCR,D3 */
    blocks[0].execute(&cpu);
    assert(cpu.pc == 0x404 && (cpu.d[3] & 0x1f) == 0x18);

    cpu.dispatch_deadline = cpu.cycles + 1;            /* ADDQ.W #1,D0 */
    blocks[0].execute(&cpu);
    assert(cpu.pc == 0x406 && cpu.d[0] == 0xffff0000);
    f3_cc_flush(&cpu);
    assert(cpu.sr == 0x15 && cpu.cc_op == 0);

    uint64_t resumed = cpu.cycles;
    cpu.dispatch_deadline = UINT64_MAX;
    blocks[0].execute(&cpu);
    assert(cpu.pc == 0x40c && cpu.cycles > resumed);
    assert(cpu.d[0] == 0xffff0000 && cpu.d[1] == 1);
    assert((cpu.d[4] & 0x1f) == 0x15 && (cpu.d[5] & 0x1f) == 0x10);
    f3_cc_flush(&cpu);
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
        root = Path(__file__).resolve().parents[2]
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
    f3_cc_flush(&cpu);
    assert(cpu.d[1] == 99 && cpu.d[2] == 98 && cpu.cc_op == 0);
    cpu.dispatch_deadline = UINT64_MAX;
    while (cpu.pc != 0x40a) dispatch(&cpu);
    assert(cpu.d[1] == 99 && cpu.d[2] == 3);
    cpu.pc = 0x402; cpu.d[0] = 0; cpu.d[1] = 99;
    cpu.dispatch_deadline = cpu.cycles + 1;
    dispatch(&cpu);
    assert(cpu.pc == 0x404 && cpu.d[0] == 1 && cpu.d[1] == 99);
    f3_cc_flush(&cpu);
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
        # Also exercise word/long operands and byte stack-register aliasing; each
        # is followed by MOVE CCR,D6 so its flags are live and must be produced.
        code = bytes.fromhex("b10a56c8fffcb14942c6b18842c6bf0f42c6")
        instructions = {insn.address: insn for insn in decoder.disasm(code, 0x400)}
        discovery = SimpleNamespace(instructions=instructions,
                                    blocks={pc: [pc] for pc in instructions},
                                    invalid_pcs=[], report={})
        root = Path(__file__).resolve().parents[2]
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
    f3_cc_flush(&cpu);
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
    f3_cc_flush(&cpu);
    assert(cpu.sr == 0x2019); /* 'A' - 'X': N/C, preserved X. */

    reads = 0; cpu.pc = 0x406; cpu.sr = 0x2010;
    cpu.a[0] = 0x1000; cpu.a[1] = 0x2000;
    memory[0x1000] = 0x80; memory[0x1001] = 0;
    memory[0x2000] = 0x7f; memory[0x2001] = 0xff;
    dispatch(&cpu);
    f3_cc_flush(&cpu);
    assert(cpu.a[0] == 0x1002 && cpu.a[1] == 0x2002 && cpu.sr == 0x2012);
    assert(reads == 2 && addresses[0] == 0x2000 && addresses[1] == 0x1000);
    assert(widths[0] == 2 && widths[1] == 2);

    reads = 0; cpu.pc = 0x40a; cpu.a[0] = 0x1000;
    memcpy(memory + 0x1000, "\0\0\0\1\0\0\0\2", 8);
    dispatch(&cpu);
    f3_cc_flush(&cpu);
    assert(cpu.a[0] == 0x1008 && cpu.sr == 0x2010 && reads == 2);
    assert(addresses[0] == 0x1000 && addresses[1] == 0x1004 && widths[0] == 4 && widths[1] == 4);

    reads = 0; cpu.pc = 0x40e; cpu.a[7] = 0x3000;
    memory[0x3000] = 0x80; memory[0x3002] = 0x7f;
    dispatch(&cpu);
    f3_cc_flush(&cpu);
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


ROOT = Path(__file__).resolve().parents[2]
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
    f3_cc_flush(&chained);
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
    f3_cc_flush(&yielding);
    assert(yielding.cc_op == 0);                    /* flags materialized at flush */
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
    f3_cc_flush(&chained);
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
    f3_cc_flush(&fallback);
    assert(fallback.cc_op == 0 && (fallback.sr & 0xfu) == 4);   /* Z, no lazy flags */

    /* Trace (T0) and STOP each refuse the chained indirect transfer: entering
       at the JMP directly leaves the switch guard as the only decision. */
    f3_cpu traced = fresh_cpu();
    traced.pc = 0x402; traced.d[0] = 0x16; traced.sr = 0x8000;
    traced.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&traced));
    f3_cc_flush(&traced);
    assert(traced.pc == 0x406 && traced.cc_op == 0);

    f3_cpu stopped = fresh_cpu();
    stopped.pc = 0x402; stopped.d[0] = 0x16; stopped.stopped = 1;
    stopped.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&stopped));
    f3_cc_flush(&stopped);
    assert(stopped.pc == 0x406 && stopped.cc_op == 0);
    return 0;
}
''')


def _decoder():
    decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
    decoder.detail = True
    return decoder


class FlagLivenessTests(unittest.TestCase):
    """CCR liveness: which flag producers lower() may drop, and which it must keep."""

    def setUp(self):
        self.decoder = _decoder()

    def analyse(self, *chunks, pinned=()):
        _, discovery = _synthetic(self.decoder, list(chunks))
        return discovery.instructions, analyse(discovery.instructions, pinned=pinned)

    @staticmethod
    def lowered(instructions, liveness, pc):
        return "\n".join(lower(instructions[pc], liveness.live_out[pc]))

    def test_producer_overwritten_before_any_read_is_elided(self):
        # MOVEQ #1,D0; MOVEQ #2,D1; RTS. The RTS keeps every flag live, so the
        # second MOVEQ must produce N/Z/V/C; the first one's are overwritten.
        instructions, liveness = self.analyse((0x400, "700172024e75"))
        self.assertEqual(liveness.live_out[0x400], CCR_X)
        self.assertEqual(liveness.live_out[0x402], CCR_ALL)
        self.assertNotIn("cc_op", self.lowered(instructions, liveness, 0x400))
        self.assertIn("F3_CC_OP_LOGIC", self.lowered(instructions, liveness, 0x402))
        # The elided instruction still executes: only the flag stores are gone.
        self.assertIn("cpu->d[0] = 0x00000001u;", self.lowered(instructions, liveness, 0x400))

    def test_producer_feeding_a_conditional_branch_is_kept(self):
        # CMP.L D1,D0; BEQ.S 0x406; MOVEQ #0,D3; MOVEQ #0,D4; RTS. Both paths
        # overwrite N/Z/V/C before the RTS, so only BEQ's Z (plus the X that
        # every MOVEQ preserves) is live after the compare.
        instructions, liveness = self.analyse((0x400, "b081" "6702" "7600" "7800" "4e75"))
        self.assertEqual(liveness.live_out[0x400], CCR_Z | CCR_X)
        self.assertIn("F3_CC_OP_CMP", self.lowered(instructions, liveness, 0x400))
        # A flagless instruction between producer and consumer changes nothing.
        instructions, liveness = self.analyse((0x400, "b081" "2040" "6702" "7600" "7800" "4e75"))
        self.assertEqual(liveness.live_out[0x400], CCR_Z | CCR_X)
        self.assertIn("F3_CC_OP_CMP", self.lowered(instructions, liveness, 0x400))

    def test_only_x_live_after_add_keeps_eager_x_and_clears_pending_state(self):
        # ADD.L D0,D1; MOVEQ #0,D2; ADDX.L D3,D4; MOVE CCR,D5. ADDX reads X and Z,
        # MOVEQ rewrites Z (but not X), so only X survives the ADD.
        instructions, liveness = self.analyse((0x400, "d280" "7400" "d983" "42c5"))
        self.assertEqual(liveness.live_out[0x400], CCR_X)
        text = self.lowered(instructions, liveness, 0x400)
        self.assertIn("cpu->sr = (cpu->sr & ~0x10u)", text)
        self.assertNotIn("F3_CC_OP_ADD", text)
        # A stale ADD/SUB left pending would recompute X at the next flush from
        # its own old operands, so the pending state is cleared instead.
        self.assertIn("cpu->cc_op = F3_CC_OP_NONE;", text)

    def test_x_only_add_is_not_clobbered_by_stale_pending_state(self):
        # Enter at the ADD with a pending SUB whose flush would set X.
        rom, discovery = _synthetic(self.decoder, [(0x400, "d280" "7400" "d983" "42c5")])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(rom, discovery, output, {})
            self.assertEqual(report["flag_liveness"]["eager_x_writes_emitted"], 1)
            _run_driver(output, report, r'''
int main(void) {
    f3_cpu cpu = fresh_cpu();
    cpu.pc = 0x400; cpu.dispatch_deadline = UINT64_MAX; cpu.sr = 0x10;
    cpu.cc_op = F3_CC_OP_SUB; cpu.cc_src = 2; cpu.cc_dst = 1;
    cpu.cc_result = 0xffffffffu; cpu.cc_width = 4;     /* flushes to borrow: X=1 */
    cpu.d[0] = 1; cpu.d[1] = 2; cpu.d[3] = 5; cpu.d[4] = 10;
    while (dispatch(&cpu)) {}
    assert(cpu.d[4] == 15);                            /* 1+2 does not carry */
    cpu.pc = 0x400; cpu.sr = 0; cpu.cc_op = 0;
    cpu.d[0] = 0xffffffffu; cpu.d[1] = 1; cpu.d[4] = 10;
    while (dispatch(&cpu)) {}
    assert(cpu.d[4] == 16);                            /* carry reaches ADDX */
    return 0;
}
''')

    def test_partial_writers_that_preserve_x_keep_an_earlier_x_producer(self):
        # ADD.W D0,D1; CMP.W D3,D2; ROXL.W #1,D4; RTS. CMP overwrites N/Z/V/C,
        # which ROXL does not read, but preserves X, which it does.
        chunk = (0x400, "d240" "b443" "e354" "4e75")
        instructions, liveness = self.analyse(chunk)
        self.assertEqual(liveness.live_out[0x402], CCR_X)
        self.assertEqual(liveness.live_out[0x400], CCR_X)
        self.assertNotIn("F3_CC_OP_CMP", self.lowered(instructions, liveness, 0x402))
        add = self.lowered(instructions, liveness, 0x400)
        self.assertIn("cpu->sr = (cpu->sr & ~0x10u)", add)
        self.assertNotIn("F3_CC_OP_ADD", add)
        # The same holds for LOGIC producers, which also preserve X from sr.
        instructions, liveness = self.analyse((0x400, "d240" "2602" "e354" "4e75"))
        self.assertEqual(liveness.live_out[0x400], CCR_X)
        rom, discovery = _synthetic(self.decoder, [chunk])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(rom, discovery, output, {})
            _run_driver(output, report, r'''
int main(void) {
    f3_cpu cpu = fresh_cpu();
    cpu.pc = 0x400; cpu.a[7] = 0x1000; cpu.dispatch_deadline = UINT64_MAX;
    cpu.d[0] = 0xffff; cpu.d[1] = 1; cpu.d[4] = 0;
    memory[0x1000] = memory[0x1001] = memory[0x1002] = memory[0x1003] = 0;
    cpu.cc_op = F3_CC_OP_LOGIC;                        /* pending state from before */
    assert(dispatch(&cpu));
    assert(cpu.d[4] == 1);                             /* X from the ADD rotated in */
    return 0;
}
''')

    def test_boundaries_keep_every_flag_live(self):
        # RTS, a computed JMP and falling off the emitted code all leave the graph.
        for tail in ("4e75", "4ed0", ""):
            _, liveness = self.analyse((0x400, "b081" + tail))
            self.assertEqual(liveness.live_out[0x400], CCR_ALL, tail)
        # BRA to an emitted instruction is an ordinary edge; to unknown code it is not.
        _, liveness = self.analyse((0x400, "b081" "6004" "4e71" "4e71" "7600" "4e75"))
        self.assertEqual(liveness.live_out[0x400], CCR_X)
        _, liveness = self.analyse((0x400, "b081" "6100" "0100"))
        self.assertEqual(liveness.live_out[0x400], CCR_ALL)

    def test_known_call_flows_into_callee_and_not_to_the_return_point(self):
        # CMP.L; BSR.W 0x4fe; MOVEQ #0,D3 (return point); RTS.
        caller = (0x400, "b081" "610000fa" "7600" "4e75")
        for callee, expected in (("7600" "4e75", CCR_X),        # callee rewrites N/Z/V/C
                                 ("4e71" "4e75", CCR_ALL)):     # callee leaves them to its RTS
            _, liveness = self.analyse(caller, (0x4fe, callee))
            self.assertEqual(liveness.succs[0x402], (0x4fe,))
            self.assertEqual(liveness.live_out[0x400], expected)

    def test_loop_back_edge_reaches_fixpoint(self):
        # MOVEQ #0,D3; L: BEQ.S out; CMP.W D1,D2; BRA.S L; NOP; out: MOVEQ #0,D5; RTS.
        # The compare feeds the BEQ of the next iteration through the back edge.
        instructions, liveness = self.analyse(
            (0x400, "7600" "6706" "b441" "60fa" "4e71" "7a00" "4e75"))
        self.assertEqual(liveness.live_out[0x404], CCR_Z | CCR_X)
        self.assertIn("F3_CC_OP_CMP", self.lowered(instructions, liveness, 0x404))
        self.assertEqual(liveness.live_out[0x400], CCR_Z | CCR_X)

    def test_pinned_pcs_observe_every_flag(self):
        # Emit-unit hooks and interpreter fallbacks read a flushed sr.
        _, liveness = self.analyse((0x400, "700172024e75"), pinned=[0x402])
        self.assertEqual(liveness.live_out[0x400], CCR_ALL)

    def test_reaching_writers(self):
        # MOVEQ #0,D0; NOP; BEQ.S 0x408; MOVEQ #1,D1; NOP.
        _, liveness = self.analyse((0x400, "7000" "4e71" "6702" "7201" "4e71"))
        self.assertEqual(liveness.reaching_writers(0x404, CCR_Z), (frozenset({0x400}), False))
        # The NOP at 0x408 is reached by the branch and by MOVEQ #1: both write Z.
        self.assertEqual(liveness.reaching_writers(0x408, CCR_Z), (frozenset({0x400, 0x406}), False))
        # X is never written here; the walk leaves the graph at the first MOVEQ.
        self.assertEqual(liveness.reaching_writers(0x404, CCR_X), (frozenset(), True))

    def test_effects_follow_the_instruction_semantics(self):
        cases = {
            "7001": (0, CCR_NZVC),                   # MOVEQ
            "d240": (0, CCR_ALL),                    # ADD.W D0,D1
            "5240": (0, CCR_ALL),                    # ADDQ.W #1,D0
            "5288": (0, 0),                          # ADDQ.L #1,A0: no flags
            "b041": (0, CCR_NZVC),                   # CMP.W D1,D0: X preserved
            "d983": (CCR_X | CCR_Z, CCR_ALL),        # ADDX.L D3,D4
            "c101": (CCR_X | CCR_Z, CCR_ALL),        # ABCD D1,D0
            "81410000": (0, 0),                      # PACK: no flags
            "e348": (0, CCR_ALL),                    # LSL.W #1,D0
            "e3a8": (CCR_X, CCR_NZVC),               # LSL.L D1,D0: count may be 0
            "e358": (0, CCR_NZVC),                   # ROL.W #1,D0: X unaffected
            "e354": (CCR_X, CCR_ALL),                # ROXL.W #1,D4
            "6702": (CCR_Z, 0),                      # BEQ
            "6e02": (CCR_N | CCR_V | CCR_Z, 0),      # BGT
            "56c8fffe": (CCR_Z, 0),                  # DBNE
            "51c8fffe": (0, 0),                      # DBRA tests condition F
            "57c0": (CCR_Z, 0),                      # SEQ
            "5dc0": (CCR_N | CCR_V, 0),              # SLT
            "42c2": (CCR_ALL, 0),                    # MOVE CCR,D2
            "44c0": (0, CCR_ALL),                    # MOVE D0,CCR
            "46c0": (CCR_ALL, 0),                    # MOVE D0,SR: privileged, may trap
            "2602": (0, CCR_NZVC),                   # MOVE.L D2,D3
            "2040": (0, 0),                          # MOVEA.L D0,A0
            "023c00ef": (0x0f, CCR_X),               # ANDI #$EF,CCR clears X
            "003c0010": (0x0f, CCR_X),               # ORI #$10,CCR sets X
            "0a3c0010": (CCR_X, 0),                  # EORI #$10,CCR toggles X
            "80c1": (CCR_ALL, 0),                    # DIVU.W: divide by zero stacks SR
            "4afc": (CCR_ALL, 0),                    # ILLEGAL
            "a000": (CCR_ALL, 0),                    # line A
            "4e71": (0, 0),                          # NOP
        }
        for code, (reads, writes) in cases.items():
            insn = next(self.decoder.disasm(bytes.fromhex(code) + bytes(6), 0x400, count=1))
            effects = flag_effects(insn)
            self.assertEqual((effects.reads, effects.writes), (reads, writes), code)

    def test_lowering_never_stores_dead_flags_and_effects_cover_what_lower_emits(self):
        # Every opcode word, with extension words that decode the long EA forms:
        # a lazy producer implies N/Z/V/C writes (X as well for ADD/SUB), an
        # exception implies reading every flag, and a dead producer emits nothing.
        for extension in (bytes(10), bytes.fromhex("0c3a00000000800012345678")):
            for opcode in range(0x10000):
                code = opcode.to_bytes(2, "big") + extension
                insn = next(self.decoder.disasm(code, 0x1000, count=1), None)
                statements = insn and lower(insn)
                if not statements:
                    continue
                text = "\n".join(statements)
                effects = flag_effects(insn)
                label = f"{opcode:04x} {insn.mnemonic} {insn.op_str}"
                ops = set(re.findall(r"F3_CC_OP_(LOGIC|ADD|SUB|CMP)", text))
                if ops:
                    self.assertEqual(effects.writes & CCR_NZVC, CCR_NZVC, label)
                if ops & {"ADD", "SUB"}:
                    self.assertTrue(effects.writes & CCR_X, label)
                if "~0x10u) |" in text:
                    self.assertTrue(effects.writes & CCR_X, label)
                if "f3_exception" in text:
                    self.assertEqual(effects.reads, CCR_ALL, label)
                for condition in re.findall(r"f3_eval_cond\(cpu, (\d+)\)", text):
                    flags = condition_flags(int(condition))
                    self.assertEqual(effects.reads & flags, flags, label)
                if not ops:
                    continue
                for live in (0, CCR_X, CCR_NZVC):
                    elided = "\n".join(lower(insn, live))
                    kept = re.search(r"F3_CC_OP_(LOGIC|ADD|SUB|CMP)", elided)
                    if not live & CCR_NZVC and not insn.mnemonic.startswith(("cas", "tas")):
                        self.assertIsNone(kept, label)
                    if not live & CCR_X:
                        self.assertNotIn("~0x10u) |", elided, label)


class FlagLivenessEquivalenceTests(unittest.TestCase):
    """Random straight-line/forward-branch programs behave identically with and
    without flag elision, from arbitrary entry points and pending flag states."""

    PROGRAMS = 120
    STATES = 40

    @staticmethod
    def _items(rng):
        """One random instruction as a list of words, or ("b", cond) / ("db", cond)."""
        def dn():
            return rng.randrange(8)

        def size():
            return rng.randrange(3)

        def imm(sz):
            return [rng.randrange(1 << 16)] if sz < 2 else [rng.randrange(1 << 16),
                                                           rng.randrange(1 << 16)]

        def ea(sz):
            kind = rng.choice(("d", "d", "m", "i"))
            if kind == "d":
                return dn(), []
            if kind == "m":
                return 0x10 | dn(), []
            return 0x3c, imm(sz)

        choice = rng.randrange(36)
        if choice == 0:
            return [0x7000 | dn() << 9 | rng.randrange(256)]
        if choice <= 6:       # ALU <ea>,Dn: add, sub, and, or, cmp
            base = (0xd000, 0x9000, 0xc000, 0x8000, 0xb000, 0xd000, 0x9000)[choice]
            sz = size()
            mode, ext = ea(sz)
            return [base | dn() << 9 | sz << 6 | mode] + ext
        if choice == 7:       # ALU Dn,<ea> with a memory or EOR destination
            base, sz = rng.choice((0xd100, 0x9100, 0xc100, 0x8100, 0xb100)), size()
            return [base | dn() << 9 | sz << 6 | rng.choice((0x10 | dn(), 0x10 | dn(), dn()))]
        if choice == 8:       # ADDA/SUBA/CMPA
            base = rng.choice((0xd0c0, 0x90c0, 0xb0c0))
            sz = rng.choice((0, 0x100))
            mode, ext = ea(1 + sz // 0x100)
            return [base | sz | dn() << 9 | mode] + ext
        if choice <= 11:      # ADDI/SUBI/CMPI/ANDI/ORI/EORI
            base = rng.choice((0x0600, 0x0400, 0x0c00, 0x0200, 0x0000, 0x0a00))
            sz = size()
            return ([base | sz << 6 | rng.choice((dn(), 0x10 | dn()))]
                    + ([rng.randrange(256)] if sz == 0 else imm(sz)))
        if choice == 12:      # ADDQ/SUBQ, including the flagless address-register form
            sz = size()
            mode = rng.choice((dn(), dn(), 0x10 | dn(), 0x08 | dn() if sz else dn()))
            return [0x5000 | rng.randrange(8) << 9 | rng.choice((0, 0x100)) | sz << 6 | mode]
        if choice == 13:      # NEG NEGX NOT CLR TST
            base = rng.choice((0x4400, 0x4000, 0x4600, 0x4200, 0x4a00))
            return [base | size() << 6 | rng.choice((dn(), dn(), 0x10 | dn()))]
        if choice == 14:      # EXT EXTB SWAP NBCD TAS
            return [rng.choice((0x4880, 0x48c0, 0x49c0, 0x4840, 0x4800, 0x4ac0)) | dn()]
        if choice == 15:      # ADDX SUBX ABCD SBCD
            base = rng.choice((0xd100, 0x9100)) | size() << 6
            if rng.random() < 0.3:
                base = rng.choice((0xc100, 0x8100))
            return [base | rng.choice((0x0, 0x8)) | dn() << 9 | dn()]
        if choice <= 18:     # shifts and rotates
            kind = rng.randrange(4)
            if rng.random() < 0.15:
                return [0xe0c0 | kind << 9 | rng.choice((0, 0x100)) | 0x10 | dn()]
            return [0xe000 | rng.randrange(8) << 9 | rng.choice((0, 0x100)) | size() << 6
                    | rng.choice((0, 0x20)) | kind << 3 | dn()]
        if choice == 19:     # MULU.W MULS.W DIVU.W DIVS.W
            return [rng.choice((0xc0c0, 0xc1c0, 0x80c0, 0x81c0)) | dn() << 9 | dn()]
        if choice == 20:     # MULU.L / MULS.L, 32-bit product
            return [0x4c00 | dn(), dn() << 12 | rng.choice((0, 0x800))]
        if choice <= 22:     # BTST BCHG BCLR BSET
            kind = rng.randrange(4) << 6
            if rng.random() < 0.5:
                return [0x0100 | dn() << 9 | kind | dn()]
            return [0x0800 | kind | dn(), rng.randrange(32)]
        if choice == 23:     # Scc
            return [0x50c0 | rng.randrange(16) << 8 | dn()]
        if choice == 24:     # bit-field operations on a data register
            return [0xe8c0 | rng.randrange(8) << 8 | dn(),
                    dn() << 12 | rng.randrange(32) << 6 | rng.randrange(32)]
        if choice <= 27:     # MOVE
            sz = rng.choice((1, 2, 3))
            mode, ext = ea(0 if sz == 1 else 2 if sz == 2 else 1)
            dest = rng.choice((0, 0, 2, 3, 4))
            return [sz << 12 | dn() << 9 | dest << 6 | mode] + ext
        if choice == 28:     # MOVE to/from CCR, and the CCR immediates
            return rng.choice(([0x44c0 | dn()], [0x42c0 | dn()],
                               [0x023c, rng.randrange(256)], [0x003c, rng.randrange(256)],
                               [0x0a3c, rng.randrange(256)]))
        if choice == 29:     # EXG / MOVEA / LEA
            return rng.choice(([0xc140 | dn() << 9 | dn()], [0xc148 | dn() << 9 | dn()],
                               [0x2040 | dn() << 9 | dn()], [0x41d0 | dn() << 9 | dn()]))
        if choice == 30:     # CMPM
            return [0xb108 | dn() << 9 | size() << 6 | dn()]
        if choice == 31:     # TRAPcc / TRAPV / CHK
            return rng.choice(([0x50fc | rng.randrange(16) << 8], [0x4e76],
                               [0x4180 | dn() << 9 | dn()], [0x4100 | dn() << 9 | dn()]))
        if choice == 32:
            return [0x4e71]
        if choice <= 34:
            return ("b", rng.randrange(2, 16))
        return ("db", rng.randrange(16))

    def _program(self, rng, base):
        for _ in range(100):
            items = [self._items(rng) for _ in range(rng.randrange(8, 15))] + [[0x4e40]]
            words = [len(item) if isinstance(item, list) else (1 if item[0] == "b" else 2)
                     for item in items]
            starts = [base]
            for count in words:
                starts.append(starts[-1] + 2 * count)
            code = bytearray()
            for index, item in enumerate(items):
                if isinstance(item, list):
                    code += b"".join(word.to_bytes(2, "big") for word in item)
                elif index > len(items) - 3:
                    code += (0x4e71).to_bytes(2, "big") * words[index]
                else:
                    target = starts[rng.randrange(index + 2, len(items))]
                    if item[0] == "b":
                        code += (0x6000 | item[1] << 8 | (target - starts[index] - 2)).to_bytes(2, "big")
                    else:
                        code += (0x50c8 | item[1] << 8 | rng.randrange(8)).to_bytes(2, "big")
                        code += (target - starts[index] - 2).to_bytes(2, "big")
            instructions = {insn.address: insn for insn in _decoder().disasm(bytes(code), base)}
            if (list(instructions) == starts[:-1] and
                    all(lower(insn) is not None for insn in instructions.values())):
                return instructions
        raise AssertionError("could not build a decodable program")

    @staticmethod
    def _function(name, instructions, live_out):
        lines = [f"static void {name}(f3_cpu *cpu) {{", "    for (;;) {", "    switch (cpu->pc) {"]
        lines += [f"    case 0x{pc:x}u: goto L_{pc:x};" for pc in instructions]
        lines += ["    default: return;", "    }"]
        for pc, insn in instructions.items():
            lines.append(f"L_{pc:x}: {{")
            lines += ["    " + statement for statement in lower(insn, live_out(pc))]
            lines += ["    continue;", "}"]
        return "\n".join(lines + ["    }", "}"])

    def test_elided_and_exact_flags_agree_from_any_entry_and_pending_state(self):
        rng = random.Random(0xf3)
        programs = [self._program(rng, 0x1000 * (index + 1)) for index in range(self.PROGRAMS)]
        merged = {pc: insn for program in programs for pc, insn in program.items()}
        liveness = analyse(merged)
        source = [r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <f3rt/cpu_abi.h>
#include "recomp/cpu_ops.h"
static uint64_t wsum;
static uint32_t hash32(uint32_t value, unsigned width) {
    value = (value + width) * 2654435761u; return value ^ (value >> 15);
}
uint8_t f3_read8(f3_cpu *cpu, uint32_t a) { (void)cpu; return (uint8_t)hash32(a, 1); }
uint16_t f3_read16(f3_cpu *cpu, uint32_t a) { (void)cpu; return (uint16_t)hash32(a, 2); }
uint32_t f3_read32(f3_cpu *cpu, uint32_t a) { (void)cpu; return hash32(a, 4); }
void f3_write8(f3_cpu *cpu, uint32_t a, uint8_t v) { (void)cpu; wsum = wsum * 31 + a * 7 + v + 1; }
void f3_write16(f3_cpu *cpu, uint32_t a, uint16_t v) { (void)cpu; wsum = wsum * 37 + a * 7 + v + 2; }
void f3_write32(f3_cpu *cpu, uint32_t a, uint32_t v) { (void)cpu; wsum = wsum * 41 + a * 7 + v + 4; }
void f3_exception(f3_cpu *cpu, unsigned vector, uint32_t return_pc) {
    f3_cc_flush(cpu);                       /* the real entry flushes before stacking SR */
    cpu->usp = return_pc; cpu->pc = 0xdead0000u + vector;
}
''']
        source.append(self._function("run_exact", merged, lambda pc: CCR_ALL))
        source.append(self._function("run_elided", merged, liveness.live_out.__getitem__))
        entries = ",\n".join("    {" + ", ".join(f"0x{pc:x}u" for pc in program) + "}"
                             for program in programs)
        source.append(f'''
#define PROGRAMS {len(programs)}
#define STATES {self.STATES}
static const uint32_t starts[PROGRAMS][{max(map(len, programs))}] = {{
{entries}
}};
static const unsigned lengths[PROGRAMS] = {{{", ".join(str(len(p)) for p in programs)}}};
static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint32_t next(void) {{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}}
static uint32_t value(void) {{
    static const uint32_t edge[] = {{0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff,
                                    0x7fffffffu, 0x80000000u, 0xffffffffu}};
    return next() % 3 ? edge[next() % 11] : next();
}}
static int same(const f3_cpu *a, const f3_cpu *b) {{
    return a->pc == b->pc && a->sr == b->sr && a->usp == b->usp && a->cycles == b->cycles &&
        !memcmp(a->d, b->d, sizeof a->d) && !memcmp(a->a, b->a, sizeof a->a);
}}
int main(void) {{
    for (unsigned p = 0; p < PROGRAMS; ++p) for (unsigned k = 0; k < STATES; ++k) {{
        f3_cpu a = {{0}};
        for (int i = 0; i < 8; ++i) {{ a.d[i] = value(); a.a[i] = value(); }}
        a.sr = 0x2000u | (next() & 0x1fu);
        a.cc_op = (uint8_t)(next() % 5); a.cc_width = (uint8_t)(1u << (next() % 3));
        a.cc_src = value(); a.cc_dst = value();
        uint32_t mask = a.cc_width == 1 ? 0xffu : a.cc_width == 2 ? 0xffffu : 0xffffffffu;
        a.cc_result = a.cc_op == F3_CC_OP_ADD ? (a.cc_src + a.cc_dst) & mask :
                      a.cc_op == F3_CC_OP_LOGIC ? value() : (a.cc_dst - a.cc_src) & mask;
        if (a.cc_op == F3_CC_OP_ADD || a.cc_op == F3_CC_OP_SUB) {{
            f3_cpu probe = a; f3_cc_flush(&probe);      /* eager X agrees with pending ADD/SUB */
            a.sr = (uint16_t)((a.sr & ~F3_CCR_X) | (probe.sr & F3_CCR_X));
        }}
        a.pc = starts[p][next() % lengths[p]]; uint32_t entry = a.pc;
        f3_cpu b = a; uint64_t sum_a, sum_b;
        wsum = 0; run_exact(&a); sum_a = wsum; f3_cc_flush(&a);
        wsum = 0; run_elided(&b); sum_b = wsum; f3_cc_flush(&b);
        if (!same(&a, &b) || sum_a != sum_b) {{
            printf("program %u entry %x\\n", p, entry);
            printf("exact  pc=%x sr=%x d0=%x\\n", a.pc, a.sr, a.d[0]);
            printf("elided pc=%x sr=%x d0=%x\\n", b.pc, b.sr, b.d[0]);
            return 1;
        }}
    }}
    return 0;
}}
''')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "equivalence.c"
            path.write_text("\n".join(source))
            executable = Path(directory) / "equivalence"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-Wall", "-Wextra",
                            "-Werror", "-I", str(ROOT), "-I", str(ROOT / "include"),
                            str(path), "-o", str(executable)], check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
        if result.returncode:
            index = int(result.stdout.split()[1])
            listing = "\n".join(f"{insn.address:x}: {insn.mnemonic} {insn.op_str}  "
                                f"live_out={liveness.live_out[insn.address]:#x}"
                                for insn in programs[index].values())
            self.fail(f"{result.stdout}\n{listing}")


if __name__ == "__main__":
    unittest.main()
