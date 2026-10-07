"""Synthetic-ROM regressions for callback and bytecode discovery (no game data)."""
import struct
import unittest

from recomp.discovery import discover


class ActorDiscoveryTests(unittest.TestCase):
    def fixture(self):
        rom = bytearray(0x1000)
        struct.pack_into(">II", rom, 0, 0x410000, 0x400)
        config = {"discovery": {
            "scan_jump_tables": False, "scan_task_traps": False, "scan_callbacks": False,
            "actor_scripts": {"pointer_field": 0x1c, "operand_bytes": [4] * 10 + [0],
                              "code_pointer_opcodes": [0], "jump_opcode": 5,
                              "call_opcode": 9, "return_opcode": 10},
        }}
        return rom, config

    def test_long_callback_and_cyclic_script_calls(self):
        rom, config = self.fixture()
        rom[0x400:0x40a] = bytes.fromhex("297c00000600001c4e75")
        struct.pack_into(">HIHIHI", rom, 0x600, 0, 0x800, 9, 0x620, 5, 0x600)
        struct.pack_into(">HIHHI", rom, 0x620, 0, 0x880, 10, 0, 0x900)
        rom[0x800:0x852] = bytes.fromhex("4e71") * 40 + bytes.fromhex("4e75")
        rom[0x880:0x882] = bytes.fromhex("4e75")
        rom[0x900:0x902] = bytes.fromhex("4e75")
        result = discover(bytes(rom), config)
        self.assertTrue({0x800, 0x850, 0x880} <= result.instructions.keys())
        self.assertTrue({0x600, 0x620, 0x900}.isdisjoint(result.instructions))

    def test_register_staged_script_counter_records(self):
        for load, store in (("203b", "2940"), ("207b", "2948")):
            with self.subTest(load=load):
                rom, config = self.fixture()
                config["discovery"]["actor_scripts"]["pointer_table_strides"] = [4, 8]
                rom[0x400:0x40e] = bytes.fromhex(load + "007e4e714e71" + store + "001c4e75")
                struct.pack_into(">IIIII", rom, 0x480, 0x600, 0, 0x620, 1, 0xffffffff)
                for script, callback in ((0x600, 0x800), (0x620, 0x820)):
                    struct.pack_into(">HIH", rom, script, 0, callback, 10)
                    rom[callback:callback + 2] = bytes.fromhex("4e75")
                result = discover(bytes(rom), config)
                self.assertTrue({0x800, 0x820} <= result.instructions.keys())
                self.assertTrue({0x480, 0x488, 0x600, 0x620}.isdisjoint(result.instructions))

    def test_explicit_script_table_count_excludes_adjacent_data(self):
        rom, config = self.fixture()
        config["discovery"]["actor_scripts"]["pointer_tables"] = [
            {"table": 0x480, "count": 2},
        ]
        rom[0x400:0x402] = bytes.fromhex("4e75")
        struct.pack_into(">III", rom, 0x480, 0x600, 0x620, 0x640)
        for script, callback in ((0x600, 0x800), (0x620, 0x820), (0x640, 0x840)):
            struct.pack_into(">HIH", rom, script, 0, callback, 10)
            rom[callback:callback + 2] = bytes.fromhex("4e75")
        result = discover(bytes(rom), config)
        self.assertTrue({0x800, 0x820} <= result.instructions.keys())
        self.assertTrue({0x480, 0x600, 0x620, 0x840}.isdisjoint(result.instructions))

    def test_staged_jump_tables_with_backward_destinations(self):
        for dispatch in ("207b003e4e714e714ed0", "41fb003e4e714e714ef00151"):
            with self.subTest(dispatch=dispatch):
                rom, config = self.fixture()
                config["discovery"]["scan_jump_tables"] = True
                struct.pack_into(">I", rom, 4, 0x500)
                code = bytes.fromhex(dispatch)
                rom[0x500:0x500 + len(code)] = code
                struct.pack_into(">III", rom, 0x540, 0x400, 0x420, 0xffffffff)
                rom[0x400:0x402] = bytes.fromhex("4e75")
                rom[0x420:0x424] = bytes.fromhex("70014e75")
                result = discover(bytes(rom), config)
                self.assertTrue({0x400, 0x420, 0x422} <= result.instructions.keys())
                self.assertNotIn(0x540, result.instructions)

    def test_signed_full_extension_table_displacement(self):
        rom, config = self.fixture()
        config["discovery"]["scan_jump_tables"] = True
        struct.pack_into(">I", rom, 4, 0x500)
        rom[0x500:0x508] = bytes.fromhex("4ebb0121ffe04e75")
        struct.pack_into(">III", rom, 0x4e2, 0x400, 0x420, 0xffffffff)
        rom[0x400:0x402] = bytes.fromhex("4e75")
        rom[0x420:0x422] = bytes.fromhex("4e75")
        result = discover(bytes(rom), config)
        self.assertTrue({0x400, 0x420, 0x506} <= result.instructions.keys())
        self.assertNotIn(0x4e2, result.instructions)



class AllAlignedDiscoveryTests(unittest.TestCase):
    def fixture(self, coverage="all_aligned"):
        rom = bytearray(0x1000)
        struct.pack_into(">II", rom, 0, 0x410000, 0x400)
        config = {"discovery": {"coverage": coverage}}
        return rom, config

    def test_computed_target_with_no_pointer_literal(self):
        rom, config = self.fixture("all_aligned")
        # MOVE.W #$80,D0; LEA $580(PC),A0; ADDA.W D0,A0; JMP (A0).
        # The $600 destination exists only after adding a data-supplied offset.
        rom[0x400:0x40c] = bytes.fromhex("303c008041fa017ad0c04ed0")
        rom[0x600:0x604] = bytes.fromhex("702a4e75")      # moveq #42, d0; rts
        self.assertNotIn(b"\x00\x00\x06\x00", rom)
        result = discover(bytes(rom), config)
        self.assertIn(0x600, result.instructions)
        self.assertIn(0x602, result.instructions)

    def test_odd_offset_24bit_pointer_target(self):
        rom, config = self.fixture("all_aligned")
        # 24-bit target pointer 0x000700 stored at ODD byte offset 0x481
        rom[0x481:0x484] = b"\x00\x07\x00"
        rom[0x700:0x704] = bytes.fromhex("4e714e75")      # nop; rts
        result = discover(bytes(rom), config)
        self.assertIn(0x700, result.instructions)
        self.assertIn(0x702, result.instructions)

    def test_greater_than_32_straight_line_instructions_without_filter(self):
        rom, config = self.fixture("all_aligned")
        # 40 NOPs followed by RTS at 0x800 (41 straight-line instructions, >32 heuristic limit)
        rom[0x800:0x850] = bytes.fromhex("4e71") * 40
        rom[0x850:0x852] = bytes.fromhex("4e75")
        result = discover(bytes(rom), config)
        expected_pcs = {0x800 + i * 2 for i in range(41)}
        self.assertTrue(expected_pcs <= result.instructions.keys())

    def test_overlapping_starts(self):
        rom, config = self.fixture("all_aligned")
        # At 0x500: move.l #$4e714e75, d0 (6 bytes: 20 3c 4e 71 4e 75)
        # Inside extension words: 0x502 is NOP (4e71), 0x504 is RTS (4e75)
        rom[0x500:0x506] = bytes.fromhex("203c4e714e75")
        result = discover(bytes(rom), config)
        self.assertTrue({0x500, 0x502, 0x504} <= result.instructions.keys())

    def test_odd_pcs_and_truncated_instruction_boundary(self):
        rom, config = self.fixture("all_aligned")
        # MOVE.L immediate needs six bytes; only its first word remains.
        rom[0xffe:0x1000] = bytes.fromhex("203c")
        result = discover(bytes(rom), config)
        # All decoded instructions must have even PCs and even sizes within ROM bounds
        for pc, insn in result.instructions.items():
            self.assertEqual(pc % 2, 0, f"Odd PC found: {pc:#x}")
            self.assertEqual(insn.size % 2, 0, f"Odd size at {pc:#x}: {insn.size}")
            self.assertLessEqual(pc + insn.size, len(rom))
        # Odd PC is not in instructions
        self.assertNotIn(0x401, result.instructions)
        # Truncated 0xffe is rejected from instructions and recorded in invalid_pcs
        self.assertNotIn(0xffe, result.instructions)
        self.assertIn(0xffe, result.invalid_pcs)
        # Odd entry points in config must raise ValueError
        with self.assertRaises(ValueError):
            discover(bytes(rom), {"discovery": {"entry_points": [0x401]}})

    def test_recursive_behavior_retained(self):
        rom, config = self.fixture("recursive")
        # Reachable code at 0x400: rts
        rom[0x400:0x402] = bytes.fromhex("4e75")
        # Unreached code at 0x900: nop; rts (valid code, but no seed/transfer reaches it)
        rom[0x900:0x904] = bytes.fromhex("4e714e75")
        result = discover(bytes(rom), config)
        self.assertIn(0x400, result.instructions)
        self.assertNotIn(0x900, result.instructions)
        self.assertNotIn(0x902, result.instructions)


class ExclusionTests(unittest.TestCase):
    def fixture(self, coverage="all_aligned"):
        rom, config = AllAlignedDiscoveryTests().fixture(coverage)
        config["exclude"] = [
            {"start": 0x800, "end": 0x900,
             "reason": "synthetic data", "evidence": "test-owned descriptor interval"},
        ]
        return rom, config

    def test_half_open_interval_and_candidate_accounting(self):
        rom, config = self.fixture()
        rom[0x7fe:0x802] = bytes.fromhex("4e714e71")
        rom[0x8fe:0x902] = bytes.fromhex("4e714e71")
        result = discover(bytes(rom), config)
        self.assertIn(0x7fe, result.instructions)
        self.assertIn(0x900, result.instructions)
        self.assertFalse(any(0x800 <= pc < 0x900 for pc in result.instructions))
        self.assertFalse(any(0x800 <= pc < 0x900 for pc in result.invalid_pcs))
        summary = result.report["summary"]
        self.assertEqual(summary["aligned_candidate_count"],
                         summary["aligned_decoded_count"] + summary["aligned_invalid_count"]
                         + summary["excluded_candidate_count"])

    def test_vector_and_explicit_table_targets_reject_exclusions(self):
        for source in ("vector", "targets", "table"):
            with self.subTest(source=source):
                rom, config = self.fixture()
                if source == "vector":
                    struct.pack_into(">I", rom, 4, 0x800)
                else:
                    table = {"address": 0x500}
                    if source == "targets":
                        table["targets"] = [0x800]
                    else:
                        struct.pack_into(">I", rom, 0x600, 0x800)
                        table.update(table=0x600, count=1)
                    config["discovery"]["jump_tables"] = [table]
                with self.assertRaisesRegex(ValueError, "excluded PC 0x800"):
                    discover(bytes(rom), config)

    def test_unproven_direct_transfer_is_reported_for_runtime_enforcement(self):
        rom, config = self.fixture()
        rom[0x500:0x506] = bytes.fromhex("4ef900000800")
        result = discover(bytes(rom), config)
        self.assertIn({"pc": "0x000500", "target": "0x000800",
                       "enforcement": "fatal_at_runtime"},
                      result.report["excluded_transfers"])
        self.assertNotIn(0x800, result.instructions)

    def test_recursive_reachable_exclusion_is_not_silently_dropped(self):
        rom, config = self.fixture("recursive")
        rom[0x400:0x406] = bytes.fromhex("4ef900000800")
        with self.assertRaisesRegex(ValueError, "excluded PC 0x800"):
            discover(bytes(rom), config)

    def test_overlap_odd_bounds_and_missing_evidence_reject(self):
        rom, config = self.fixture()
        for change in ({"start": 0x801}, {"end": 0x1002}, {"evidence": ""}):
            with self.subTest(change=change):
                invalid = dict(config["exclude"][0], **change)
                with self.assertRaises(ValueError):
                    discover(bytes(rom), {**config, "exclude": [invalid]})
        with self.assertRaisesRegex(ValueError, "Overlapping"):
            discover(bytes(rom), {**config, "exclude": config["exclude"] * 2})

if __name__ == "__main__":
    unittest.main()
