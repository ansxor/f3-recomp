"""Regression suite for f3a reverse-engineering answers and invariants.

Pins static analysis queries and runtime facts that AI agents and human analysts
rely on, so changes to the static tracker or commands do not silently alter them.
"""
from pathlib import Path
import os
import shutil
import sys
import tempfile
import unittest

from tools.analysis import f3a, cli  # noqa: E402


COIN_START_MASH_SCRIPT = """# coin, then start every 90 frames, then mash
700+20 coin
800-2400 mash keys=start period=45 seed=7
1200-end mash seed=3
"""

TEN_WINS_POKE_SCRIPT = """# Reaches sub_08e9c6 (charram fill) by poking the VS round counter -$60ac (0x401f54) to 10 from frame 3000 onward; base = 2p-versus-rounds.txt (2P VS, P2 idle, P1 mashes b1 every 45).
# Experiment B: 2P VS, P2 idle, P1 mashes b1 period 45 seed 5.
700+20 coin
720+20 p2 coin
800-1300 mash keys=start period=45
1400+5 right
1500+5 start
2000-end mash keys=b1 period=45 seed=5
3000-end poke 0x401f54.w=10
"""


def has_game_roms(game_id: str) -> bool:
    try:
        f3a.game(game_id)
        return True
    except (Exception, SystemExit):
        return False


def has_game_executable(game_id: str, instrumented: bool = False) -> bool:
    if not has_game_roms(game_id):
        return False
    try:
        g = f3a.game(game_id)
        cli.game_executable(g.core, None, instrumented=instrumented)
        return True
    except (Exception, SystemExit):
        return False


# ---------------------------------------------------------------- static: commandw

@unittest.skipUnless(has_game_roms("commandw"), "commandw ROMs missing")
class CommandwStaticFactsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.g = f3a.game("commandw")

    def test_sub_00a334_reads_rom_table(self):
        # sub_00a334 lea.l $a372(pc), a1 followed by move.b (a1), d1
        # ROM table at 0xa372 consists of 4 bytes [0x0, 0x80, 0xc0, 0xe0],
        # which form 2 big-endian words [0x0080, 0xc0e0].
        # Code of sub_00a376 begins immediately after at 0xa376.
        rom_bytes = list(self.g.rom(0xa372, 4))
        self.assertEqual(rom_bytes, [0x00, 0x80, 0xc0, 0xe0])
        words = self.g.words(0xa372, 2)
        self.assertEqual(words, [0x80, 0xc0e0])

        res = self.g.dis(0xa372, data="words")
        self.assertEqual(len(res), 1)
        self.assertEqual(res[0].values, [0x80, 0xc0e0])
        self.assertTrue(any("table ends at 0x00a376" in note for note in res.notes))

    def test_dis_2ff42_data_words_stops_at_code(self):
        # dis 0x2ff42 --data words decodes 17 words and terminates at 0x02ff64
        # where code of sub_02ff64 begins.
        res = self.g.dis(0x2ff42, data="words")
        total_words = sum(len(r.values) for r in res)
        self.assertEqual(total_words, 17)
        flattened = [v for r in res for v in r.values]
        expected_words = [
            0x40, 0x40, 0x40, 0x40, 0x50, 0x80, 0x80, 0x1,
            0x1, 0x10, 0x1, 0x1, 0x1, 0x1, 0x18, 0x1, 0x40,
        ]
        self.assertEqual(flattened, expected_words)
        self.assertEqual(self.g.words(0x2ff42, 17), expected_words)
        self.assertTrue(any("table ends at 0x02ff64" in note for note in res.notes))
        self.assertIn(0x2ff64, self.g.core.routines)

    def test_vectors(self):
        # commandw 68k vector table: 26 non-default vectors
        res = self.g.vectors()
        self.assertEqual(len(res), 26)
        by_num = {r.vector: r for r in res}
        self.assertEqual(by_num[1].name, "reset")
        self.assertEqual(by_num[1].address, 0x400)
        self.assertEqual(by_num[26].name, "irq2")
        self.assertEqual(by_num[26].address, 0x673a)
        self.assertEqual(by_num[27].name, "irq3")
        self.assertEqual(by_num[27].address, 0x6716)

    def test_xref_field_with_bit(self):
        # xref --field 0x1.2 --role write: bit ops or andi/ori/eori masks
        # touching bit 2 of byte +1 through an address register
        res = self.g.xref(field="0x1.2", role="write")
        # 4 sites touch bit 2 of byte +1 via an address register (3 in sub_02fed6, 1 in sub_0349b2)
        self.assertEqual(len(res), 4)
        pcs = [r.pc for r in res]
        self.assertEqual(pcs, [0x2feda, 0x2ff10, 0x2ff36, 0x349de])
        for r in res:
            self.assertEqual(r.role, "rmw")
        self.assertEqual([r.routine for r in res], ["sub_02fed6", "sub_02fed6", "sub_02fed6", "sub_0349b2"])
    def test_writes_region_sprites_ranges(self):
        # writes --region sprites --ranges: store-PC ranges per routine
        res = self.g.writes(region=["sprites"], ranges=True)
        sprite_ranges = [r for r in res if r.layer == "sprites"]
        self.assertEqual(len(sprite_ranges), 6)
        by_routine = {r.routine: r for r in sprite_ranges}
        # sub_009b32 has 21 placed stores between 0x009c8a and 0x00a156
        self.assertIn("sub_009b32", by_routine)
        r_9b32 = by_routine["sub_009b32"]
        self.assertEqual(r_9b32.start, "0x009c8a")
        self.assertEqual(r_9b32.end, "0x00a156")
        self.assertEqual(r_9b32.placed, 21)
        # sub_000400 has 7 placed stores between 0x00045a and 0x0005b6
        self.assertIn("sub_000400", by_routine)
        r_400 = by_routine["sub_000400"]
        self.assertEqual(r_400.start, "0x00045a")
        self.assertEqual(r_400.end, "0x0005b6")
        self.assertEqual(r_400.placed, 7)

    def test_yield_traps(self):
        # commandw trap handlers classified as cooperative task switches: traps 4, 5, 9
        self.assertEqual(sorted(self.g.core.yield_traps.keys()), [4, 5, 9])

    def test_struct_sub_009b32_a4(self):
        # struct sub_009b32 a4: 15 fields accessed via a4, span +0x00..+0x5a
        res = self.g.struct("sub_009b32", "a4")
        self.assertEqual(len(res), 15)
        offsets = [r.offset for r in res]
        self.assertEqual(offsets, [0, 4, 8, 16, 24, 32, 36, 40, 48, 56, 64, 68, 72, 80, 88])
        sizes = [r.size for r in res]
        self.assertEqual(sizes, [4, 4, 2, 2, 2, 4, 4, 2, 2, 2, 4, 4, 2, 2, 2])
        self.assertTrue(any("span: +0x00..+0x5a" in note for note in res.notes))
        self.assertEqual(min(r.offset for r in res), 0x00)
        self.assertEqual(max(r.offset + r.size for r in res), 0x5a)
        r0 = res[0]
        self.assertEqual(r0.offset, 0)
        self.assertEqual(r0.size, 4)
        self.assertEqual(r0.writes, 1)
        self.assertEqual(r0.pcs, ["0x009c8a"])
        self.assertEqual(r0.routines, ["sub_009b32"])
        r_last = res[-1]
        self.assertEqual(r_last.offset, 88)
        self.assertEqual(r_last.size, 2)
        self.assertEqual(r_last.writes, 1)
        self.assertEqual(r_last.pcs, ["0x00a156"])


# ---------------------------------------------------------------- static: landmakrj

@unittest.skipUnless(has_game_roms("landmakrj"), "landmakrj ROMs missing")
class LandmakrjStaticFactsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.g = f3a.game("landmakrj")

    def test_dis_40aa_data_offsets_jump_table(self):
        # landmakrj 0x40a6 has `jmp $40aa(pc, d0.w)`.
        # dis 0x40aa --data offsets decodes 24 offset-table entries.
        res = self.g.dis(0x40aa, data="offsets")
        self.assertEqual(len(res), 24)
        # Verify specific index -> target pairs observed from real output
        self.assertEqual(res[0].index, 0)
        self.assertEqual(res[0].target, 0x40da)
        self.assertEqual(res[0].value, 0x30)

        self.assertEqual(res[1].index, 1)
        self.assertEqual(res[1].target, 0x40ea)
        self.assertEqual(res[1].value, 0x40)

        self.assertEqual(res[5].index, 5)
        self.assertEqual(res[5].target, 0x4122)
        self.assertEqual(res[5].value, 0x78)

        self.assertEqual(res[17].index, 17)
        self.assertEqual(res[17].target, 0x4180)
        self.assertEqual(res[17].value, 0xd6)

        self.assertEqual(res[23].index, 23)
        self.assertEqual(res[23].target, 0x41b4)
        self.assertEqual(res[23].value, 0x10a)

        self.assertTrue(any("table ends at 0x0040da" in note for note in res.notes))

    def test_dis_3ee6_shared_by_35_routines(self):
        # 0x003ee6 is code inside sub_0a2118, shared by 35 routines
        res = self.g.dis(0x3ee6)
        self.assertTrue(any("shared by 35 routines" in note for note in res.notes))
        self.assertEqual(len(self.g.core.owners[0x3ee6]), 35)

    def test_writes_sub_00431a_placed_stores(self):
        # sub_00431a has 16 stores to sprites (8 to 0x600000, 8 to 0x608000), all placed
        res = self.g.writes("sub_00431a")
        self.assertEqual(len(res), 16)
        for r in res:
            self.assertEqual(r.region, "sprites")
            self.assertIn("sprites", r.regions)

    def test_flow_dispatch_repeated_candidate_sets(self):
        # flow ?dispatch: repeated candidate sets are referenced ("the same N candidates as 0x...")
        # and candidate rows carry hit counts.
        res = self.g.flow("?dispatch")
        site_rows = [r for r in res if r.get("direction") == "site"]
        self.assertGreater(len(site_rows), 0)

        # Check for reference to identical candidate sets
        repeats = [r for r in site_rows if "the same " in r.line and "candidates as 0x" in r.line]
        self.assertGreaterEqual(len(repeats), 1)

        # Check candidate hit counts exist on candidate rows
        site_953f6 = next((r for r in site_rows if r.site == 0x0953f6), None)
        self.assertIsNotNone(site_953f6)
        self.assertIn("sub_095e7c", site_953f6.candidate_hits)
        self.assertGreater(site_953f6.candidate_hits["sub_095e7c"], 0)

    def test_xref_a5_global_and_writes(self):
        # xref a5-0x60ac resolves to 0x401f54 and includes writes in sub_08e20c
        target_addr = self.g.address("a5-0x60ac")
        self.assertEqual(target_addr, 0x401f54)

        res = self.g.xref("a5-0x60ac")
        sub_8e20c_addr = self.g.address("sub_08e20c")
        writes_in_8e20c = [
            r for r in res
            if self.g.core.routine_of(r.pc) == sub_8e20c_addr and r.role in ("write", "rmw")
        ]
        # Pinned write PCs in sub_08e20c
        write_pcs = sorted(r.pc for r in writes_in_8e20c)
        self.assertEqual(write_pcs, [0x8e414, 0x8e484, 0x8e59e, 0x8e5be, 0x8e5da])

    def test_flow_path_to_sub_08e9c6(self):
        # flow sub_08e9c6 --path 4 reaches it from sub_08e20c via sub_08e8b8 and sub_08e9c0
        res = self.g.flow("sub_08e9c6", path=4)
        self.assertEqual(len(res), 1)
        row = res[0]
        sub_8e20c_addr = self.g.address("sub_08e20c")
        # Root routine containing first call site 0x8e62e is sub_08e20c
        self.assertEqual(self.g.core.routine_of(row.chain[0]["site"]), sub_8e20c_addr)

        to_addrs = [self.g.address(step["to"]) for step in row.chain]
        self.assertEqual(to_addrs, [0x8e8b8, 0x8e9c0, 0x8e9c6])

        # Step 0: call @ 0x8e62e -> sub_08e8b8
        self.assertEqual(row.chain[0]["site"], 0x8e62e)
        self.assertEqual(row.chain[0]["kind"], "call")
        # Step 1: call @ 0x8e8e6 -> sub_08e9c0
        self.assertEqual(row.chain[1]["site"], 0x8e8e6)
        self.assertEqual(row.chain[1]["kind"], "call")
        # Step 2: fall @ 0x8e9c0 -> sub_08e9c6
        self.assertEqual(row.chain[2]["site"], 0x8e9c0)
        self.assertEqual(row.chain[2]["kind"], "fall")

    def test_yield_tasks_and_resume_points(self):
        # landmakrj yield traps: 4, 5, 6, 7
        self.assertEqual(sorted(self.g.core.yield_traps.keys()), [4, 5, 6, 7])
        # sub_08e20c (address 0x08e20c) has kinds task, seed, ref
        r = self.g.core.routines[0x08e20c]
        self.assertIn("task", r.kinds)
        self.assertEqual(r.kinds, {"task", "seed", "ref"})
        # 14 resume points belonging to sub_08e20c, including 0x08e4f4, 0x08e332, 0x08e788
        resume_points = sorted(pc for pc, target in self.g.core.resume_points.items() if target == 0x08e20c)
        self.assertEqual(len(resume_points), 14)
        expected = [0x8e20e, 0x8e276, 0x8e306, 0x8e332, 0x8e3e4, 0x8e402, 0x8e404,
                    0x8e442, 0x8e4be, 0x8e4f4, 0x8e532, 0x8e788, 0x8e7b8, 0x8e7c2]
        self.assertEqual(resume_points, expected)

    def test_writes_stack_arg_stores(self):
        # 7 stores in 4 routines without static callers go through incoming stack arguments
        res = self.g.writes("sub_005614", "sub_0056ae", "sub_005726", "sub_00581c")
        self.assertEqual(len(res), 7)
        pcs = [r.pc for r in res]
        expected_pcs = [0x5646, 0x56d6, 0x5756, 0x5758, 0x583a, 0x5840, 0x5846]
        self.assertEqual(pcs, expected_pcs)
        for r in res:
            self.assertEqual(r.region, "stack-arg")
        arg_offsets = {r.pc: r.arg_offset for r in res}
        self.assertEqual(arg_offsets, {
            0x5646: 8,
            0x56d6: 8,
            0x5756: 8,
            0x5758: 8,
            0x583a: 10,
            0x5840: 10,
            0x5846: 10,
        })
        # writes --region video --ranges reports the count of stack argument stores
        res_ranges = self.g.writes(video=True, ranges=True)
        self.assertTrue(any("7 stores in 4 routines go through stack arguments" in note for note in res_ranges.notes))


# ---------------------------------------------------------------- runtime: landmakrj

@unittest.skipUnless(has_game_executable("landmakrj"), "landmakrj executable missing")
class LandmakrjRuntimeFactsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.g = f3a.game("landmakrj")

    def test_run_coin_start_mash_until_sub_09d72a(self):
        # run --frames 6000 --inputs coin-start-mash.txt --until sub_09d72a reaches at frame 1414
        with tempfile.TemporaryDirectory() as tmpdir:
            script_file = Path(tmpdir) / "coin-start-mash.txt"
            script_file.write_text(COIN_START_MASH_SCRIPT)

            out_dir = Path(tmpdir) / "run_out"
            run = self.g.run(frames=6000, inputs=str(script_file), until=["sub_09d72a"], out=str(out_dir))
            self.assertEqual(len(run.log), 1)
            row = run.log[0]
            self.assertEqual(row.target, "sub_09d72a")
            self.assertTrue(row.reached)
            self.assertEqual(row.frame, 1414)
            self.assertEqual(row.pc, 0x09d762)

    @unittest.skipUnless(has_game_executable("landmakrj", instrumented=True),
                         "landmakrj instrumented build missing")
    def test_run_instrumented_watch_until_sub_08e9c6(self):
        # With instrumented build: --inputs ten-wins-poke.txt --watch 0x08e62e --until sub_08e9c6
        # reaches sub_08e9c6 at frame 7287 and 0x08e62e executes at frame 7286.
        with tempfile.TemporaryDirectory() as tmpdir:
            script_file = Path(tmpdir) / "ten-wins-poke.txt"
            script_file.write_text(TEN_WINS_POKE_SCRIPT)

            out_dir = Path(tmpdir) / "run_watch_out"
            run = self.g.run(frames=8000, inputs=str(script_file), watch=["0x08e62e"],
                             until=["sub_08e9c6"], out=str(out_dir))

            until_rows = run.log.where(target="sub_08e9c6")
            self.assertGreater(len(until_rows), 0)
            self.assertEqual(until_rows[0].frame, 7287)
            self.assertTrue(until_rows[0].reached)

            watch_rows = [r for r in run.log if str(r.get("pc")) == "0x08e62e"]
            self.assertEqual(len(watch_rows), 1)
            self.assertEqual(watch_rows[0].first, 7286)
            self.assertEqual(watch_rows[0].last, 7286)

            entries_log = out_dir / "entries.log"
            self.assertTrue(entries_log.is_file())
            content = entries_log.read_text()
            self.assertIn("ENTRY pc=0x08e62e frame=7286 hits=1", content)
            self.assertIn("ENTRY pc=0x08e9c6 frame=7287 hits=1", content)

    def test_until_non_entry_without_watch_raises(self):
        # --until on a non-entry without --watch raises F3AError mentioning --watch
        with tempfile.TemporaryDirectory() as tmpdir:
            out_dir = Path(tmpdir) / "err_out"
            with self.assertRaises(f3a.F3AError) as ctx:
                self.g.run(frames=10, until=["0x08e62e"], out=str(out_dir))
            self.assertIn("--watch", str(ctx.exception))

    def test_malformed_input_script_fails(self):
        # A malformed input script line fails naming the line
        with tempfile.TemporaryDirectory() as tmpdir:
            script_file = Path(tmpdir) / "bad_script.txt"
            script_file.write_text("700+20 coin\ninvalid_script_line\n")
            out_dir = Path(tmpdir) / "err_out"
            with self.assertRaises(f3a.F3AError) as ctx:
                self.g.run(frames=10, inputs=str(script_file), out=str(out_dir))
            err_msg = str(ctx.exception)
            self.assertTrue("inputs.txt:2" in err_msg or "expected a number" in err_msg)

    def test_run_all_writers_discover_known_and_unknown_pcs(self):
        # run --frames 1500 --all-writers writes discovery.log with every observed writer;
        # discover groups rows with known_pcs / unknown_pcs; unknown_only=True filters fully known groups
        with tempfile.TemporaryDirectory() as tmpdir:
            out_dir = Path(tmpdir) / "run_writers"
            self.g.run(frames=1500, all_writers=True, out=str(out_dir))
            disc_log = out_dir / "discovery.log"
            self.assertTrue(disc_log.is_file())

            rows = self.g.discover(disc_log)
            target_row = next((r for r in rows if r.get("routine") == "sub_01008a" and r.get("layer") == "lines"), None)
            self.assertIsNotNone(target_row)
            self.assertEqual(target_row.known_pcs, ["0x0100ba"])
            self.assertEqual(target_row.unknown_pcs, ["0x0100d0"])

            unk_rows = self.g.discover(disc_log, unknown_only=True)
            self.assertTrue(any(r.get("routine") == "sub_01008a" and r.get("layer") == "lines" for r in unk_rows))
            # unknown_only=True excludes fully known groups (groups where known_pcs is non-empty and unknown_pcs is empty)
            self.assertFalse(any(r.get("known_pcs") and not r.get("unknown_pcs") for r in unk_rows))
            self.assertLess(len(unk_rows), len(rows))

    def test_run_dump_sprite_writers(self):
        # run dumping frame 1500 writes sprite_writers.bin (16384 bytes) and sprites decode shows writer PC/routine
        with tempfile.TemporaryDirectory() as tmpdir:
            out_dir = Path(tmpdir) / "run_sprites"
            run = self.g.run(frames=1500, dump_start=1500, dump_every=1, out=str(out_dir))
            dump_dir = out_dir / "dumps" / "frame_1500"
            sw_bin = dump_dir / "sprite_writers.bin"
            self.assertTrue(sw_bin.is_file())
            self.assertEqual(sw_bin.stat().st_size, 16384)

            rows = run.sprites()
            written_rows = [r for r in rows if r.get("writer")]
            self.assertGreater(len(written_rows), 0)
            for r in written_rows:
                pc = r.writer
                owner = self.g.core.routine_of(pc)
                self.assertIsNotNone(owner)
                self.assertEqual(r.writer_routine, self.g.core.label(owner))

            # Check at least one known sprite writer routine (e.g. sub_00431a or sub_000400 or sub_009b32)
            routines = {r.writer_routine for r in written_rows}
            known_writers = {"sub_00431a", "sub_000400", "sub_009b32"}
            self.assertTrue(bool(routines & known_writers), f"Expected one of {known_writers} in {routines}")


# ---------------------------------------------------------------- runtime: landmakrj (indirect)

@unittest.skipUnless(has_game_executable("landmakrj", instrumented=True),
                     "landmakrj instrumented build missing")
class LandmakrjIndirectRuntimeFactsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmpdir = tempfile.TemporaryDirectory()
        cls.out_dir = Path(cls.tmpdir.name) / "run_indirect"
        cls.g = f3a.game("landmakrj")
        cls.game_run = cls.g.run(frames=2000, indirect=True, out=str(cls.out_dir))

    @classmethod
    def tearDownClass(cls):
        cls.tmpdir.cleanup()

    def test_indirect_log_site_000c42_targets(self):
        # indirect.log records computed jmp/jsr (site, target) counts; site 0x000c42 targets 0x000c46 and 0x000c48
        log_file = self.out_dir / "indirect.log"
        self.assertTrue(log_file.is_file())
        content = log_file.read_text().splitlines()
        self.assertTrue(content and content[0].startswith("# f3rt indirect targets v1"))
        site_c42_lines = [line for line in content if "site=0x000c42" in line]
        self.assertTrue(any("target=0x000c46" in line for line in site_c42_lines))
        self.assertTrue(any("target=0x000c48" in line for line in site_c42_lines))

    def test_flow_dispatch_observed_not_static_and_local_markers(self):
        # flow '?dispatch' --observed RUN_DIR:
        # site 0x003f30 marks sub_0a811e as (not a static candidate), and site 0x0040a6 marks targets as (local)
        flow_res = self.g.flow("?dispatch", observed=str(self.out_dir))
        site_rows = [r for r in flow_res if r.get("direction") == "site"]

        # site 0x003f30 observed target sub_0a811e is not a static candidate
        site_3f30 = next((r for r in site_rows if r.site == 0x003f30), None)
        self.assertIsNotNone(site_3f30)
        self.assertIn("sub_0a811e", site_3f30.observed)
        self.assertNotIn("sub_0a811e", site_3f30.candidates)
        self.assertIn(f"sub_0a811e x{site_3f30.observed['sub_0a811e']} (not a static candidate)", site_3f30.line)

        # site 0x0040a6 observed targets are inside the site's own routine (local)
        site_40a6 = next((r for r in site_rows if r.site == 0x0040a6), None)
        self.assertIsNotNone(site_40a6)
        self.assertGreater(len(site_40a6.observed), 0)
        for target, count in site_40a6.observed.items():
            self.assertIn(f"{target} x{count} (local)", site_40a6.line)


if __name__ == "__main__":
    unittest.main()
