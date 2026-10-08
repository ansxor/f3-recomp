"""Emit-unit config parsing, header rendering, digest and generator hooks."""
from pathlib import Path
from types import SimpleNamespace
import json
import tempfile
import unittest

from recomp.sprite_units import (
    canonical, digest, guard_lines, parse_sprite_units, render_c_header, render_cpp_header)

ROOT = Path(__file__).resolve().parent.parent


def cfg(units=None, ranges=None):
    video = {}
    if units is not None:
        video["emit_units"] = units
    if ranges is not None:
        video["frame_writers"] = {"ranges": ranges}
    return {"video": video}


def unit(**overrides):
    base = {"name": "objects", "start": 0x400, "end": 0x40a, "unit": "a6", "size": 0x80}
    base.update(overrides)
    return base


class ParseTests(unittest.TestCase):
    def rejects(self, config, text):
        with self.assertRaisesRegex(ValueError, text):
            parse_sprite_units(config)

    def test_defaults_and_scalar_lists(self):
        spec = parse_sprite_units(cfg([unit(), {"name": "q", "start": [2, 4], "end": [6, 8],
                                                "unit": "d3", "owner": "writer"}]))
        self.assertEqual([u.id for u in spec.units], [0, 1])
        self.assertEqual(spec.units[0].starts, (0x400,))
        self.assertEqual((spec.units[1].size, spec.units[1].owner), (0, "writer"))
        self.assertEqual(spec.hook_pcs()[0], {0x400: [0], 2: [1], 4: [1]})
        self.assertEqual(spec.hook_pcs()[1], {0x40a: [0], 6: [1], 8: [1]})

    def test_empty_config(self):
        spec = parse_sprite_units({})
        self.assertEqual((spec.units, spec.frame_writers), ((), ()))

    def test_errors(self):
        self.rejects(cfg([unit(start=0x401)]), "must be even")
        self.rejects(cfg([unit(end=0x40b)]), "must be even")
        self.rejects(cfg([unit(start=[0x400, 0x500], end=[0x40a])]), "start has 2 entries, end has 1")
        self.rejects(cfg([unit(), unit()]), "duplicate emit unit name")
        self.rejects(cfg([unit(unit="a7")]), "unit register")
        self.rejects(cfg([unit(unit="sp")]), "unit register")
        self.rejects(cfg([unit(name="1bad")]), "invalid unit name")
        self.rejects(cfg([unit(name="all")]), "invalid unit name")
        self.rejects(cfg([unit(start=0x40a, end=0x400)]), "must be below")
        self.rejects(cfg([unit(size=-1)]), "size")
        self.rejects(cfg([unit(owner="both")]), "owner")
        self.rejects(cfg([unit(extra=1)]), "unknown keys")
        self.rejects(cfg([{"name": "x", "start": 2, "end": 4}]), "missing 'unit'")
        self.rejects(cfg([unit(start=[2, 2], end=[4, 6])]), "duplicate")
        self.rejects(cfg(ranges=[[0x41, 0x50]]), "must be even")
        self.rejects(cfg(ranges=[[0x50, 0x40]]), "invalid")
        self.rejects(cfg(ranges=[[0x40]]), r"\[first, last\]")

    def test_frame_writer_last_may_be_odd(self):
        self.assertEqual(parse_sprite_units(cfg(ranges=[[0x4400, 0x4421]])).frame_writers,
                         ((0x4400, 0x4421),))


class DigestTests(unittest.TestCase):
    def test_stable_and_sensitive(self):
        base = parse_sprite_units(cfg([unit()], [[0x10, 0x20]]))
        again = parse_sprite_units(cfg([unit()], [[0x10, 0x20]]))
        self.assertEqual(digest(base), digest(again))
        for changed in (cfg([unit(size=0x40)], [[0x10, 0x20]]),
                        cfg([unit(end=0x40c)], [[0x10, 0x20]]),
                        cfg([unit(unit="a5")], [[0x10, 0x20]]),
                        cfg([unit(owner="writer")], [[0x10, 0x20]]),
                        cfg([unit(name="renamed")], [[0x10, 0x20]]),
                        cfg([unit()], [[0x10, 0x22]]),
                        cfg([unit()], None)):
            self.assertNotEqual(digest(base), digest(parse_sprite_units(changed)))

    def test_digest_independent_of_unrelated_config(self):
        a = cfg([unit()])
        b = dict(a, discovery={"coverage": "all_aligned"})
        self.assertEqual(digest(parse_sprite_units(a)), digest(parse_sprite_units(b)))

    def test_pinned_value(self):
        # Guards the canonical form: a change here invalidates every generated tree.
        self.assertEqual(canonical(parse_sprite_units({})), "units:\nframe_writers:\n")


class RenderTests(unittest.TestCase):
    def test_empty(self):
        spec = parse_sprite_units({})
        header = render_c_header(spec)
        self.assertIn("#define F3_SPRITE_UNIT_COUNT 0\n", header)
        self.assertIn(f"#define F3_SPRITE_UNITS_DIGEST 0x{digest(spec):08x}u\n", header)
        text = render_cpp_header(spec)
        self.assertIn("std::array<const EmitUnit *, 0> all{}", text)
        self.assertIn("std::array<PcRange, 0> frame_writers{}", text)
        self.assertIn("namespace f3rt::sprite_units {", text)

    def test_units(self):
        spec = parse_sprite_units(cfg(
            [unit(), {"name": "queue", "start": [2, 4], "end": [6, 8], "unit": "d0",
                      "owner": "writer", "size": 18}], [[0x10, 0x21]]))
        header = render_c_header(spec)
        self.assertIn("#define F3_SPRITE_UNIT_COUNT 2\n", header)
        text = render_cpp_header(spec)
        for expected in (
                'EmitUnit objects{0u, "objects", detail::objects_starts, detail::objects_ends, '
                'UnitRegister::A6, 128u, UnitOwner::Unit}',
                'EmitUnit queue{1u, "queue", detail::queue_starts, detail::queue_ends, '
                'UnitRegister::D0, 18u, UnitOwner::Writer}',
                "queue_starts{0x2u, 0x4u}", "queue_ends{0x6u, 0x8u}",
                "all{&objects, &queue}", "PcRange{0x10u, 0x21u}",
                "static_assert(valid_units()"):
            self.assertIn(expected, text)

    def test_guard_lines(self):
        spec = parse_sprite_units(cfg([unit()]))
        self.assertIn(f"0x{digest(spec):08x}u", guard_lines(spec))

    def test_shipped_configs_parse(self):
        try:
            import tomllib
        except ImportError:
            self.skipTest("tomllib needs Python 3.11")
        counts = {}
        for path in sorted((ROOT / "games").glob("*/config.toml")):
            spec = parse_sprite_units(tomllib.loads(path.read_text("utf-8")))
            counts[path.parent.name] = [u.name for u in spec.units]
        self.assertEqual(counts["commandw"], ["objects"])
        self.assertEqual(counts["landmakrj"], ["display_objects", "queue"])
        self.assertEqual(counts["landmakr"], [])


class GeneratorHookTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import tomllib  # noqa: F401  (recomp.generate needs Python 3.11)
            from capstone import Cs, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_020
        except ImportError:
            raise unittest.SkipTest("tomllib/capstone unavailable")
        decoder = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_020)
        decoder.detail = True
        # 0x400: MOVEQ #-1,D0; ADDQ.W #1,D0; MOVEQ #1,D1; MOVEQ #2,D2; MOVEQ #3,D3
        cls.code = bytes.fromhex("70ff5240720174027603")
        cls.rom = bytes(0x400) + cls.code
        cls.instructions = {i.address: i for i in decoder.disasm(cls.code, 0x400)}

    def discovery(self):
        return SimpleNamespace(instructions=dict(self.instructions),
                               blocks={0x400: list(self.instructions)}, invalid_pcs=[], report={})

    def generate(self, config, **kwargs):
        from recomp.generate import generate
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            report = generate(self.rom, self.discovery(), out, config, **kwargs)
            files = {p.name: p.read_text() for p in out.glob("*.c")}
            return report, files, json.loads((out / "lowering.json").read_text())

    def test_hooks_at_labels(self):
        config = cfg([unit(start=0x402, end=0x406, unit="a1"),
                      unit(name="second", start=0x406, end=0x408, unit="d1")], [[0x400, 0x404]])
        # Unit 0 ends at 0x406 where unit 1 starts: exit must precede enter.
        report, files, lowering = self.generate(config)
        spec = parse_sprite_units(config)
        block = files["blocks_0000.c"]
        self.assertIn(guard_lines(spec), block)
        self.assertNotIn("sprite_units.h", files["program.c"])
        label = block.index("L_000406: {")
        body = block[label:block.index("F3_PROFILE_HIT_MAIN", label)]
        self.assertLess(body.index("F3_UNIT_EXIT(cpu, 0);"), body.index("F3_UNIT_ENTER(cpu, 1);"))
        start = block[block.index("L_000402: {"):]
        self.assertLess(start.index("F3_UNIT_ENTER(cpu, 0);"), start.index("F3_PROFILE_HIT_MAIN"))
        self.assertEqual(lowering["emit_units"]["units"]["objects"],
                         {"id": 0, "enter_hooks": 1, "exit_hooks": 1})
        self.assertEqual(lowering["emit_units"]["frame_writer_ranges"], 1)
        self.assertEqual(lowering["runtime_abi_version"], 4)

    def test_no_units_emits_no_hooks(self):
        _, files, lowering = self.generate({})
        self.assertFalse(any("sprite_units.h" in text or "F3_UNIT_" in text
                             for text in files.values()))
        self.assertEqual(lowering["emit_units"]["units"], {})

    def test_undecoded_pc_is_an_error(self):
        with self.assertRaisesRegex(ValueError, "not a decoded instruction"):
            self.generate(cfg([unit(start=0x402, end=0x500)]))

    def test_frame_writer_outside_rom(self):
        with self.assertRaisesRegex(ValueError, "outside the ROM"):
            self.generate(cfg(ranges=[[0x400, 0x10000]]))

    def test_slim_profile_must_retain_hooks(self):
        # Retention is checked before emission, so the profile loader is stubbed.
        import recomp.block_profile as block_profile
        original = block_profile.load_hot
        block_profile.load_hot = lambda *args, **kwargs: {0x400, 0x402}
        try:
            with tempfile.TemporaryDirectory() as directory:
                profile = Path(directory) / "p"
                profile.write_text("")
                from recomp.generate import generate
                with self.assertRaisesRegex(ValueError, "not retained"):
                    generate(self.rom, self.discovery(), Path(directory) / "out",
                             cfg([unit(start=0x402, end=0x406)]), profile_slim=profile)
        finally:
            block_profile.load_hot = original


if __name__ == "__main__":
    unittest.main()
