"""Safety-boundary regressions for ROM exclusion proposals (synthetic data)."""
import struct
import unittest

from tools.rom.exclusions import candidate_ranges, scan_rom, toml_proposals


class ExclusionScannerTests(unittest.TestCase):
    def fixture(self, cpu="main"):
        base = 0 if cpu == "main" else 0xc00000
        rom = bytearray(bytes.fromhex("4afc") * (0x5000 // 2))
        rom[:1024] = b"\xff" * 1024
        struct.pack_into(">II", rom, 0, 0x410000, base + 0x400)
        rom[0x400:0x402] = bytes.fromhex("4e75")
        rom[0x1001:0x3001] = b"\xff" * 0x2000
        return bytes(rom), {"game": {"id": "synthetic"}}, base

    def fill(self, report, start):
        return next(c for c in report["candidates"] if c["start"] == start
                    and c["kind"] == "constant_fill")

    def test_fill_alignment_does_not_consume_boundary_or_checksum(self):
        rom, _, _ = self.fixture()
        regions = candidate_ranges(rom, 0)
        fill = next(c for c in regions if c["kind"] == "constant_fill")
        self.assertEqual((fill["start"], fill["end"]), (0x1002, 0x3000))
        self.assertEqual(rom[0x3000:0x3004], bytes.fromhex("fffc4afc"))

    def test_periodic_padding_preserves_trailer_and_does_not_classify_nops(self):
        rom = bytes.fromhex("00ff") * 2049 + bytes.fromhex("1234")
        fill = next(c for c in candidate_ranges(rom, 0) if c["kind"] == "periodic_fill")
        self.assertEqual((fill["start"], fill["end"]), (0, 4098))
        self.assertEqual(candidate_ranges(bytes.fromhex("4e71") * 4096, 0), [])

    def test_profile_fetch_half_open_boundaries_for_both_cpus(self):
        for cpu in ("main", "sound"):
            rom, config, base = self.fixture(cpu)
            start, end = base + 0x1002, base + 0x3000
            profile = {cpu: {"fetched": [[start - 2, start], [end, end + 2]]}}
            candidate = self.fill(scan_rom(rom, config, cpu, profile), start)
            self.assertTrue(candidate["safe_to_propose"])
            profile[cpu]["fetched"].append([end - 1, end])
            report = scan_rom(rom, config, cpu, profile)
            candidate = self.fill(report, start)
            self.assertEqual(candidate["status"], "conflicting")
            self.assertIn("observed_instruction_fetch", {b["kind"] for b in candidate["blockers"]})
            self.assertNotIn(f"start = 0x{start:x}", toml_proposals(report))

    def test_declared_entry_blocks_even_undecodable_fill(self):
        rom, config, _ = self.fixture()
        config["discovery"] = {"entry_points": [0x1002]}
        candidate = self.fill(scan_rom(rom, config), 0x1002)
        self.assertFalse(candidate["safe_to_propose"])
        self.assertIn("declared_entry", {b["kind"] for b in candidate["blockers"]})

    def test_explicit_pointer_table_targets_block_without_reachable_table(self):
        rom, config, base = self.fixture("sound")
        rom = bytearray(rom)
        struct.pack_into(">I", rom, 0x800, base + 0x1002)
        config["sound"] = {"discovery": {"pointer_tables": [{"table": base + 0x800, "count": 1}]}}
        candidate = self.fill(scan_rom(bytes(rom), config, "sound"), base + 0x1002)
        self.assertFalse(candidate["safe_to_propose"])
        self.assertIn("explicit_pointer_table_target", {b["kind"] for b in candidate["blockers"]})

    def test_direct_control_reference_blocks_undecodable_fill(self):
        rom, config, _ = self.fixture()
        rom = bytearray(rom)
        rom[0x400:0x406] = bytes.fromhex("4ef900001002")
        candidate = self.fill(scan_rom(bytes(rom), config), 0x1002)
        self.assertFalse(candidate["safe_to_propose"])
        self.assertIn("rooted_control_reference", {b["kind"] for b in candidate["blockers"]})

    def test_data_reads_and_partial_known_data_cannot_prove_entropy_window(self):
        rom, config, _ = self.fixture()
        rom = bytearray(rom)
        rom[0x1000:0x2000] = bytes(range(256)) * 16
        profile = {"main": {"data_reads": [[0x1000, 0x2000]], "known_data": [
            {"start": 0x1000, "end": 0x1ffe, "reason": "HLE table", "evidence": "Typed table consumer"},
        ]}}
        report = scan_rom(bytes(rom), config, profile=profile)
        candidate = next(c for c in report["candidates"] if c["start"] == 0x1000)
        self.assertEqual(candidate["status"], "rejected")
        self.assertEqual(candidate["rejection_reason"], "statistics_without_explicit_data_evidence")
        profile["main"]["known_data"][0]["end"] = 0x2000
        report = scan_rom(bytes(rom), config, profile=profile)
        candidate = next(c for c in report["candidates"] if c["start"] == 0x1000)
        self.assertTrue(candidate["safe_to_propose"])
        profile["main"]["fetched"] = [[0x1000, 0x1002]]
        report = scan_rom(bytes(rom), config, profile=profile)
        candidate = next(c for c in report["candidates"] if c["start"] == 0x1000)
        self.assertEqual(candidate["status"], "conflicting")

    def test_known_low_entropy_descriptors_do_not_require_entropy(self):
        rom, config, _ = self.fixture()
        rom = bytearray(rom)
        rom[0x1000:0x2000] = b"\x12" * 0x1000
        profile = {"main": {"known_data": [
            {"start": 0x1001, "end": 0x2000, "reason": "Descriptor",
             "evidence": "Typed game-data consumer"},
        ]}}
        report = scan_rom(bytes(rom), config, profile=profile)
        candidate = next(c for c in report["candidates"] if c["kind"] == "known_data")
        self.assertEqual((candidate["start"], candidate["end"]), (0x1002, 0x2000))
        self.assertTrue(candidate["safe_to_propose"])
        profile["main"]["fetched"] = [[0x1800, 0x1802]]
        self.assertFalse(next(c for c in scan_rom(bytes(rom), config, profile=profile)["candidates"]
                              if c["kind"] == "known_data")["safe_to_propose"])


if __name__ == "__main__":
    unittest.main()
