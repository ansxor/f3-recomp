"""Profile identity, merge and strict-input invariants; synthetic addresses only."""
from pathlib import Path
import tempfile
import unittest
import zlib

from recomp.block_profile import BlockProfile, MAX_COUNT, load_hot, read_profile
from recomp.discovery import ExcludeRegion


class ProfileTests(unittest.TestCase):
    def test_union_keeps_rom_identities_and_saturates_counts(self):
        left = BlockProfile({("main", 1): (0, 8)}, {("main", 1, 2): MAX_COUNT}, {})
        right = BlockProfile({("main", 1): (0, 8), ("main", 2): (0, 8)},
                             {("main", 1, 2): 3, ("main", 2, 2): 7}, {("main", 1, 4): 1})
        left.merge(right)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "merged.profile"
            left.write(path)
            loaded = read_profile(path)
        self.assertEqual(loaded.hits, {("main", 1, 2): MAX_COUNT, ("main", 2, 2): 7})
        self.assertEqual(loaded.misses, {("main", 1, 4): 1})

    def test_miss_is_not_hot_and_rom_identity_must_match(self):
        rom = bytes.fromhex("4e714e714e71")
        crc = zlib.crc32(rom)
        profile = BlockProfile({("sound", crc): (0xc00000, len(rom))},
                               {("sound", crc, 0xc00000): 2}, {("sound", crc, 0xc00002): 1})
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sound.profile"
            profile.write(path)
            self.assertEqual(load_hot(path, "sound", rom, 0xc00000), {0xc00000})
            with self.assertRaises(ValueError):
                load_hot(path, "sound", bytes(len(rom)), 0xc00000)
            with self.assertRaises(ValueError):
                load_hot(path, "sound", rom, 0)

    def test_exclusions_reject_matching_hits_and_misses_but_not_other_roms(self):
        rom = bytes.fromhex("4e714e714e714e71")
        crc = zlib.crc32(rom)
        for cpu, base in (("main", 0), ("sound", 0xc00000)):
            excluded = ExcludeRegion(base + 2, base + 4, "synthetic data", "fixture")
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "entries.profile"
                other_crc = crc ^ 1
                other_cpu = "sound" if cpu == "main" else "main"
                identities = {(cpu, crc): (base, len(rom)),
                              (cpu, other_crc): (base, len(rom)),
                              (other_cpu, crc): (base, len(rom))}
                hits = {(cpu, crc, base): 1, (cpu, crc, base + 4): 1,
                        (cpu, other_crc, base + 2): 1,
                        (other_cpu, crc, base + 2): 1}
                BlockProfile(identities, hits).write(path)
                self.assertEqual(load_hot(path, cpu, rom, base, exclusions=(excluded,)),
                                 {base, base + 4})
                for kind in ("hit", "miss"):
                    with self.subTest(cpu=cpu, kind=kind):
                        contradictory = BlockProfile(identities, dict(hits))
                        rows = contradictory.hits if kind == "hit" else contradictory.misses
                        rows[cpu, crc, base + 2] = 1
                        contradictory.write(path)
                        with self.assertRaises(ValueError) as raised:
                            load_hot(path, cpu, rom, base, exclusions=(excluded,))
                        message = str(raised.exception)
                        for detail in (cpu, kind, f"{base + 2:#x}",
                                       f"[{excluded.start:#x}, {excluded.end:#x})",
                                       excluded.reason):
                            self.assertIn(detail, message)

    def test_rejects_invalid_addresses_counts_versions_and_missing_identity(self):
        bad_rows = ["hit main 00000001 00000001 1", "hit main 00000001 00000008 1",
                    "hit main 00000002 00000002 1", "hit main 00000001 00000002 0",
                    "hit main 00000001 00000002 -1", f"hit main 00000001 00000002 {MAX_COUNT+1}"]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.profile"
            for row in bad_rows:
                with self.subTest(row=row):
                    path.write_text("F3-BLOCK-PROFILE 1\nrom main 00000001 00000000 00000008\n" + row + "\n")
                    with self.assertRaises(ValueError): read_profile(path)
            path.write_text("F3-BLOCK-PROFILE 2\n")
            with self.assertRaises(ValueError): read_profile(path)

    def test_rejects_merge_of_conflicting_bounds(self):
        left = BlockProfile({("main", 1): (0, 8)})
        with self.assertRaises(ValueError):
            left.merge(BlockProfile({("main", 1): (0, 10)}))


if __name__ == "__main__":
    unittest.main()
