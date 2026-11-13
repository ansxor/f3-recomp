import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from functools import cache
from pathlib import Path
from types import SimpleNamespace

from capstone import CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_020, Cs
from capstone.m68k import M68K_OP_BR_DISP

from recomp.discovery import discover, load_rom
from recomp.generate import generate, write_if_changed
from recomp.sprite_trace import (
    LANE_X,
    LANE_XY,
    LANE_Y,
    RootField,
    TraceResult,
    load_writer_seeds,
    render_header,
    trace,
    write_outputs,
)

ROOT = Path(__file__).resolve().parents[2]
LANDMAKRJ_CONFIG = ROOT / "games" / "landmakrj" / "config.toml"
LANDMAKRJ_ROMS = ROOT / "roms" / "landmakrj"
COMMANDW_CONFIG = ROOT / "games" / "commandw" / "config.toml"

LANDMAKRJ_WRITERS = {
    0xA8F5E: (LANE_X, 0x0C, 0xFFFF),
    0xA8F62: (LANE_Y, 0xFFFF, 0x0E),
    0xA8F98: (LANE_X, 0x0C, 0xFFFF),
    0xA8F9E: (LANE_Y, 0xFFFF, 0x0E),
    0xA8FB2: (LANE_X, 0x0C, 0xFFFF),
    0xA8FB4: (LANE_Y, 0xFFFF, 0x0E),
    0xA8FC4: (LANE_X, 0x0C, 0xFFFF),
    0xA8FC6: (LANE_Y, 0xFFFF, 0x0E),
    0xA9052: (LANE_X, 0x0C, 0xFFFF),
    0xA9056: (LANE_Y, 0xFFFF, 0x0E),
    0xA9100: (LANE_X, 0x0C, 0xFFFF),
    0xA9102: (LANE_Y, 0xFFFF, 0x0E),
    0xA9112: (LANE_X, 0x0C, 0xFFFF),
    0xA9114: (LANE_Y, 0xFFFF, 0x0E),
    0xA9124: (LANE_X, 0x0C, 0xFFFF),
    0xA9126: (LANE_Y, 0xFFFF, 0x0E),
    0xA9132: (LANE_X, 0x0C, 0xFFFF),
    0xA9134: (LANE_Y, 0xFFFF, 0x0E),
    0xA91EA: (LANE_XY, 0x0C, 0x0E),
    0xA926A: (LANE_XY, 0x0C, 0x0E),
    0xA92E4: (LANE_XY, 0x0C, 0x0E),
    0xA9368: (LANE_XY, 0x0C, 0x0E),
}
LANDMAKRJ_SCALED = (0xA91EA, 0xA926A, 0xA92E4, 0xA9368)

COMMANDW_WRITERS = {
    0x9C90: (LANE_XY, 0x44, 0x46),
    0x9E9A: (LANE_XY, 0x06, 0x08),
    0xA0BE: (LANE_XY, 0x06, 0x08),
}


def commandw_roms() -> Path | None:
    for candidate in (ROOT / "roms" / "commandw", Path.home() / "Downloads" / "commandw"):
        if (candidate / "cw_mpr0.bin").is_file():
            return candidate
    return None


@cache
def landmakrj_trace() -> TraceResult:
    rom, config = load_rom(LANDMAKRJ_CONFIG, LANDMAKRJ_ROMS)
    return trace(rom, discover(rom, config), config)


@cache
def commandw_trace() -> TraceResult | None:
    rom_dir = commandw_roms()
    if rom_dir is None:
        return None
    rom, config = load_rom(COMMANDW_CONFIG, rom_dir)
    return trace(rom, discover(rom, config), config)


def fixture_discovery(code: bytes, entry: int = 0x400):
    decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
    decoder.detail = True
    instructions = {insn.address: insn for insn in decoder.disasm(code, entry)}
    branch_targets = {}
    for pc, insn in instructions.items():
        operands = insn.operands
        if operands and operands[-1].type == M68K_OP_BR_DISP:
            branch_targets[pc] = [pc + 2 + operands[-1].br_disp.disp]
    return SimpleNamespace(
        instructions=instructions,
        blocks={entry: sorted(instructions)},
        functions={entry},
        branch_targets=branch_targets,
        indirect_targets={},
        invalid_pcs=[],
        report={},
    )


FIXTURE_CODE = bytes.fromhex("49f900600340425c425c322b000c7404080300006704d2426002924238c14e75")


class LandmakrjTraceTests(unittest.TestCase):
    def test_exact_writer_table(self):
        result = landmakrj_trace()
        writers = {writer.pc: writer for writer in result.writers}
        self.assertEqual(sorted(writers), sorted(LANDMAKRJ_WRITERS))
        for pc, (lane, x_offset, y_offset) in LANDMAKRJ_WRITERS.items():
            writer = writers[pc]
            self.assertEqual((writer.lane, writer.x_offset, writer.y_offset), (lane, x_offset, y_offset), hex(pc))
            self.assertEqual(writer.base, 11, f"{pc:#x} base")
            self.assertEqual(writer.cursor_reg, 9, f"{pc:#x} cursor")
            self.assertEqual(writer.group_class, 1)
            self.assertEqual(
                writer.root_offsets, tuple(RootField(11, offset) for offset in (x_offset, y_offset) if offset != 0xFFFF)
            )

    def test_scaled_handler_is_not_rigid(self):
        writers = {writer.pc: writer for writer in landmakrj_trace().writers}
        for pc, writer in writers.items():
            self.assertEqual(writer.rigid, pc not in LANDMAKRJ_SCALED, hex(pc))

    def test_coverage(self):
        result = landmakrj_trace()
        self.assertEqual(result.stats["resolved_writers"], 22)
        self.assertGreater(result.stats["x_y_writers"], result.stats["resolved_writers"])
        self.assertEqual(result.stats["unresolved_writers"], len(result.unresolved))
        unresolved = {writer.pc: writer for writer in result.unresolved}
        self.assertIn(0x469A, unresolved)
        self.assertIn(0x46A2, unresolved)
        self.assertIn("stream cursor", unresolved[0x469A].reason)
        self.assertIn("(a6)+", unresolved[0x469A].destination)

    def test_continuity_classification(self):
        result = landmakrj_trace()
        kinds = {(item.pc, item.offset): item.kind for item in result.classifications}
        self.assertEqual(kinds[(0x95866, 0x0C)], "reset")
        self.assertEqual(kinds[(0x95DAC, 0x0C)], "reset")
        self.assertEqual(kinds[(0x9550E, 0x0C)], "reset")
        self.assertEqual(kinds[(0xA04FA, 0x0C)], "reset")
        self.assertEqual(kinds[(0x96A88, 0x0C)], "continuation")
        self.assertEqual(kinds[(0x9FADE, 0x0E)], "continuation")
        self.assertEqual(kinds[(0xA0200, 0x0C)], "continuation")
        resets = {(reset.pc, reset.offset) for reset in result.resets}
        self.assertIn((0x95866, 0x0C), resets)
        self.assertIn((0xA04FA, 0x0C), resets)
        self.assertIn((0x95DAC, 0x0C), resets)
        self.assertNotIn((0x96A88, 0x0C), resets)
        self.assertNotIn((0x9FADE, 0x0E), resets)


class CommandwTraceTests(unittest.TestCase):
    def setUp(self):
        if commandw_trace() is None:
            self.skipTest("commandw ROMs not available (roms/commandw or ~/Downloads/commandw)")

    def test_origin_writer_table(self):
        result = commandw_trace()
        writers = {writer.pc: writer for writer in result.writers}
        self.assertEqual(sorted(writers), sorted(COMMANDW_WRITERS))
        for pc, (lane, x_offset, y_offset) in COMMANDW_WRITERS.items():
            writer = writers[pc]
            self.assertEqual((writer.lane, writer.x_offset, writer.y_offset), (lane, x_offset, y_offset), hex(pc))
            self.assertEqual(writer.base, 14, f"{pc:#x} base a6")
            self.assertEqual(writer.cursor_reg, 12, f"{pc:#x} cursor a4")
            self.assertTrue(writer.rigid)
        self.assertEqual(writers[0x9C90].group_class, 2)
        self.assertEqual(writers[0x9E9A].group_class, 1)
        self.assertEqual(writers[0xA0BE].group_class, 1)

    def test_continuity_classification(self):
        result = commandw_trace()
        kinds = {(item.pc, item.offset): item.kind for item in result.classifications}
        self.assertEqual(kinds[(0xA25A, 0x06)], "continuation")
        self.assertEqual(kinds[(0xA2E8, 0x44)], "continuation")
        self.assertEqual(kinds[(0xA2FE, 0x44)], "reset")
        self.assertEqual(kinds[(0xC7FE, 0x08)], "continuation")
        self.assertEqual(kinds[(0xC818, 0x08)], "continuation")
        self.assertEqual(kinds[(0xC830, 0x06)], "continuation")

    def test_unresolved_writers_are_reported(self):
        result = commandw_trace()
        unresolved = {writer.pc: writer for writer in result.unresolved}
        self.assertEqual(len(unresolved), result.stats["unresolved_writers"])
        self.assertIn(0x9AAA, unresolved)
        self.assertIn("constant", unresolved[0x9AAA].reason)
        self.assertEqual(unresolved[0x9AAA].lane, LANE_XY)


class HeaderTests(unittest.TestCase):
    WRITE_ROW = re.compile(r"\{0x([0-9a-f]+)u, (\d+)u, (\d+)u, (\d+)u, (\d+)u, 0x([0-9a-f]+)u, 0x([0-9a-f]+)u\}")
    RESET_ROW = re.compile(r"\{0x([0-9a-f]+)u, (\d+)u, 0u, 0x([0-9a-f]+)u\}")

    def test_renders_and_parses(self):
        result = landmakrj_trace()
        header = render_header(result)
        self.assertTrue(header.startswith("/* Generated by recomp/sprite_trace.py. Do not commit. */\n#pragma once\n"))
        self.assertIn('#include "f3rt/sprite_group.hpp"', header)
        self.assertIn("namespace f3rt::sprite_groups {", header)
        writes = [
            (int(a, 16), int(b), int(c), int(d), int(e), int(f, 16), int(g, 16))
            for a, b, c, d, e, f, g in self.WRITE_ROW.findall(header)
        ]
        self.assertEqual(
            writes,
            [(w.pc, w.base, w.lane, w.group_class, 8, w.x_offset, w.y_offset) for w in result.writers],
        )
        resets = [(int(a, 16), int(b), int(c, 16)) for a, b, c in self.RESET_ROW.findall(header)]
        self.assertEqual(resets, [(r.pc, r.base, r.offset) for r in result.resets])
        self.assertIn("inline constexpr SpriteGroupTable table{", header)
        self.assertNotIn(", }", header)

    @unittest.skipUnless((ROOT / "include" / "f3rt" / "sprite_group.hpp").is_file(), "runtime header not present")
    def test_compiles_with_cxx20(self):
        compiler = os.environ.get("CXX") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no C++ compiler")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            write_outputs(landmakrj_trace(), output, write_if_changed)
            (output / "check.cpp").write_text(
                '#include "sprite_groups.hpp"\n'
                "static_assert(f3rt::sprite_groups::table.writes.size() == 22);\n"
                "int main() { return f3rt::sprite_groups::table.resets.size() == 0; }\n",
                encoding="utf-8",
            )
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "include"),
                    "-I",
                    str(output),
                    str(output / "check.cpp"),
                    "-o",
                    str(output / "check"),
                ],
                check=True,
            )

    def test_empty_table_uses_empty_spans(self):
        header = render_header(TraceResult("empty", (), (), (), {}))
        self.assertNotIn("SpriteGroupWrite writes[]", header)
        self.assertNotIn("SpriteGroupReset resets[]", header)
        self.assertIn(
            "inline constexpr SpriteGroupTable table{std::span<const SpriteGroupWrite>{}, "
            "std::span<const SpriteGroupReset>{}};",
            header,
        )


class FixtureTests(unittest.TestCase):
    def test_branch_union_cursor_and_lane_offsets(self):
        discovery = fixture_discovery(FIXTURE_CODE)
        rom = bytes(0x400) + FIXTURE_CODE
        result = trace(rom, discovery, {})
        self.assertEqual([writer.pc for writer in result.writers], [0x41C])
        writer = result.writers[0]
        self.assertEqual((writer.base, writer.lane, writer.x_offset, writer.y_offset), (11, LANE_X, 0x0C, 0xFFFF))
        self.assertEqual(writer.cursor_reg, 12)
        self.assertEqual(writer.group_class, 1)
        self.assertTrue(writer.rigid)

    def test_explicit_writer_seed_forces_lane(self):
        code = bytes.fromhex("322b000c32c14e75")
        discovery = fixture_discovery(code)
        rom = bytes(0x400) + code
        self.assertEqual(trace(rom, discovery, {}).writers, ())
        result = trace(rom, discovery, {}, writer_seeds=[0x404], writer_lanes={0x404: LANE_X})
        self.assertEqual([writer.pc for writer in result.writers], [0x404])
        self.assertEqual((result.writers[0].base, result.writers[0].x_offset), (11, 0x0C))

    def test_generate_writes_sprite_groups(self):
        discovery = fixture_discovery(FIXTURE_CODE)
        rom = bytes(0x400) + FIXTURE_CODE
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            report = generate(rom, discovery, output, {})
            self.assertEqual(report["sprite_groups"]["resolved_writers"], 1)
            header = (output / "sprite_groups.hpp").read_text(encoding="utf-8")
            self.assertIn("0x0000041cu", header)
            emitted = json.loads((output / "sprite_groups.json").read_text(encoding="utf-8"))
            self.assertEqual([write["pc"] for write in emitted["writes"]], ["0x00041c"])


class WriterSeedTests(unittest.TestCase):
    def test_array_and_object_forms(self):
        with tempfile.TemporaryDirectory() as directory:
            array = Path(directory) / "array.json"
            array.write_text("[1052, 1054]", encoding="utf-8")
            self.assertEqual(load_writer_seeds(array)[0], [0x41C, 0x41E])
            obj = Path(directory) / "object.json"
            obj.write_text('{"writers": [{"pc": 1052, "lane": "xy"}]}', encoding="utf-8")
            self.assertEqual(load_writer_seeds(obj), ([0x41C], {0x41C: LANE_XY}))
            odd = Path(directory) / "odd.json"
            odd.write_text("[1049]", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "even"):
                load_writer_seeds(odd)
            bad = Path(directory) / "bad.json"
            bad.write_text('{"writers": ["x"]}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "invalid sprite writer"):
                load_writer_seeds(bad)


if __name__ == "__main__":
    unittest.main()
