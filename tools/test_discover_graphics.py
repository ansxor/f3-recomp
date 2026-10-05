"""Physical-ROM consumer regressions: plausible data is not a control-flow root."""
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

from tools.discover_graphics import make_catalog


class RootedGraphicsTests(unittest.TestCase):
    ROOT = 0x400
    TARGET = 0x500
    PREFIX = bytes.fromhex("45f900600000")  # LEA sprite RAM,A2: catalog the root.
    READ_DATA = bytes.fromhex("23f90000050000600000")  # MOVE.L data,sprite RAM.
    DATA = bytes.fromhex("4e71") * 4 + bytes.fromhex("33fc0001006000004e75")
    EXCLUSION = {"start": 0x500, "end": 0x520,
                 "reason": "descriptor payload", "evidence": "fixture-owned ROM data interval"}

    def catalog(self, code, *, excluded=False, rounds=6, discovery="", extra=()):
        rom = bytearray(0x800)
        struct.pack_into(">II", rom, 0, 0x410000, self.ROOT)
        rom[self.ROOT:self.ROOT + len(code)] = code
        rom[self.TARGET:self.TARGET + len(self.DATA)] = self.DATA
        for pc, data in extra:
            rom[pc:pc + len(data)] = data
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            root.joinpath("program.bin").write_bytes(rom)
            manifest = ('[game]\nid = "rooted_fixture"\n'
                        '[rom]\nsize = 2048\n'
                        '[[rom.lanes]]\nfile = "program.bin"\nsize = 2048\n'
                        'offset = 0\nstride = 1\n'
                        f'crc = "{zlib.crc32(rom):08x}"\n'
                        f'sha1 = "{hashlib.sha1(rom).hexdigest()}"\n')
            if excluded:
                manifest += ('[[exclude]]\nstart = 1280\nend = 1312\n'
                             'reason = "descriptor payload"\n'
                             'evidence = "fixture-owned ROM data interval"\n')
            manifest += '[discovery]\n' + discovery
            config = root / "config.toml"
            config.write_text(manifest, encoding="utf-8")
            return make_catalog(config, root, root_rounds=rounds)

    def routine(self, catalog, entry):
        return next(item for item in catalog["routines"] if item["entry"] == entry)

    def decoded_pcs(self, catalog):
        return {insn["pc"] for routine in catalog["routines"]
                for insn in routine["instructions"]}

    def assert_reached_target_writer(self, catalog, target=TARGET):
        # JMP targets can be blocks in an existing routine, not new entries.
        expected = {target + offset: self.DATA[offset:offset + size].hex()
                    for offset, size in ((0, 2), (2, 2), (4, 2), (6, 2), (8, 8), (16, 2))}
        owners = [routine for routine in catalog["routines"]
                  if any(insn["pc"] == target for insn in routine["instructions"])]
        self.assertTrue(owners, "Reached target bytecode must have a cataloged CFG owner")
        self.assertEqual({pc for pc in self.decoded_pcs(catalog)
                          if target <= pc < target + len(self.DATA)}, set(expected))
        for owner in owners:
            instructions = {insn["pc"]: insn for insn in owner["instructions"]}
            for pc, raw in expected.items():
                self.assertIn(pc, instructions)
                self.assertEqual(instructions[pc]["bytes"], raw)
                self.assertEqual(instructions[pc]["size"], len(raw) // 2)
            successors = {node["pc"]: node["successors"] for node in owner["cfg"]}
            reached, pending = set(), [owner["entry"]]
            while pending:
                pc = pending.pop()
                if pc not in reached:
                    reached.add(pc)
                    pending.extend(successors.get(pc, []))
            self.assertTrue(set(expected) <= reached)
            writer_pc = target + 8
            writers = [item for item in owner["writers"] if item["pc"] == writer_pc]
            self.assertTrue(writers, "Reached target's MMIO write must belong to its CFG owner")
            for writer in writers:
                self.assertEqual(writer["instruction"], instructions[writer_pc])
                self.assertEqual(writer["region"], "sprite")
                self.assertEqual(writer["mode"], "write")
                self.assertEqual(writer["width"], 2)
                self.assertEqual(writer["resolved_address"]["kind"], "constant")
                self.assertEqual(writer["resolved_address"]["value"], 0x600000)
                self.assertEqual(writer["source"]["kind"], "constant")
                self.assertEqual(writer["source"]["value"], 1)
                self.assertIn(writer_pc, writer["source"]["evidence_pcs"])

    def assert_unfollowed(self, catalog, candidate, *, excluded, target=TARGET):
        self.assertEqual(candidate["entry"], target)
        self.assertFalse(candidate["followed"])
        self.assertFalse(candidate["target_is_independently_rooted"])
        self.assertTrue(candidate["coherent_code_sequence"])
        self.assertEqual(candidate["target_bytes"], self.DATA[:16].hex())
        self.assertEqual(candidate["target_exclusion"], self.EXCLUSION if excluded else None)
        self.assertNotIn(target, {routine["entry"] for routine in catalog["routines"]})
        self.assertTrue(set(range(target, target + len(self.DATA), 2)).isdisjoint(self.decoded_pcs(catalog)))

    def assert_data_read(self, catalog, pc):
        root = self.routine(catalog, self.ROOT)
        instruction = next(item for item in root["instructions"] if item["pc"] == pc)
        self.assertEqual(instruction["bytes"], self.READ_DATA.hex())
        access = next(item for item in root["writers"] if item["pc"] == pc)
        self.assertEqual(access["region"], "sprite")
        self.assertEqual(access["source"]["name"], "load")
        self.assertEqual(access["source"]["arguments"][0]["value"], self.TARGET)

    def test_ram_descriptor_pointer_is_evidence_not_callback(self):
        store = bytes.fromhex("23fc0000050000400010")
        for rounds in (0, 6):
            for excluded in (False, True):
                with self.subTest(rounds=rounds, excluded=excluded):
                    catalog = self.catalog(self.PREFIX + store + self.READ_DATA + bytes.fromhex("4e75"),
                                           rounds=rounds, excluded=excluded)
                    source_pc = self.ROOT + len(self.PREFIX)
                    candidate = next(item for item in catalog["root_expansion_evidence"]
                                     if item.get("destination") == 0x400010)
                    self.assert_unfollowed(catalog, candidate, excluded=excluded)
                    self.assertEqual(candidate["source_pc"], source_pc)
                    self.assertEqual(candidate["instruction"]["pc"], source_pc)
                    self.assertEqual(candidate["instruction"]["bytes"], store.hex())
                    self.assertEqual(candidate["source"]["value"], self.TARGET)
                    self.assertIn(source_pc, candidate["pointer_evidence_pcs"])
                    self.assert_data_read(catalog, source_pc + len(store))

    def test_unknown_trap_stack_argument_remains_data(self):
        pea = bytes.fromhex("487900000500")
        trap = bytes.fromhex("4e41")
        for rounds in (0, 6):
            for excluded in (False, True):
                with self.subTest(rounds=rounds, excluded=excluded):
                    catalog = self.catalog(self.PREFIX + pea + trap + self.READ_DATA + bytes.fromhex("4e75"),
                                           rounds=rounds, excluded=excluded)
                    producer_pc = self.ROOT + len(self.PREFIX)
                    source_pc = producer_pc + len(pea)
                    candidate = next(item for item in catalog["root_expansion_evidence"]
                                     if item.get("source_pc") == source_pc and item.get("entry") == self.TARGET
                                     and item.get("input_location", "").startswith("stack:"))
                    self.assert_unfollowed(catalog, candidate, excluded=excluded)
                    self.assertEqual(candidate["instruction"]["pc"], source_pc)
                    self.assertEqual(candidate["instruction"]["bytes"], trap.hex())
                    self.assertIn({"pc": producer_pc, "bytes": pea.hex()},
                                  [{"pc": item["pc"], "bytes": item["bytes"]}
                                   for item in candidate["pointer_instructions"]])
                    self.assert_data_read(catalog, source_pc + len(trap))

    def test_genuine_excluded_control_roots_are_fatal(self):
        direct = bytes.fromhex("4eb9000005004e75")
        indirect = bytes.fromhex("41f9000005004e904e75")
        # The callee receives A0 from an ordinary documented caller, not a TRAP ABI.
        caller = bytes.fromhex("41f9000005004eb9000004404e75")
        helper = bytes.fromhex("4e904e75")
        cases = (("direct", direct, "", ()),
                 ("entry", bytes.fromhex("4e75"), "entry_points = [1280]\n", ()),
                 ("table", bytes.fromhex("4ed04e75"),
                  "[[discovery.jump_tables]]\naddress = 1030\ntargets = [1280]\n", ()),
                 ("indirect", indirect, "", ()),
                 ("caller_indirect", caller, "", ((0x440, helper),)))
        for name, code, config, extra in cases:
            for rounds in (0, 6):
                with self.subTest(control=name, rounds=rounds):
                    with self.assertRaisesRegex(ValueError, r"excluded PC 0x500"):
                        self.catalog(self.PREFIX + code, excluded=True, rounds=rounds,
                                     discovery=config, extra=extra)

    def test_included_control_targets_are_followed(self):
        cases = ((bytes.fromhex("4eb9000005004e75"), "", ()),
                 (bytes.fromhex("41f9000005004e904e75"), "", ()),
                 (bytes.fromhex("4e75"), "entry_points = [1280]\n", ()),
                 (bytes.fromhex("4ed04e75"),
                  "[[discovery.jump_tables]]\naddress = 1030\ntargets = [1280]\n", ()),
                 (bytes.fromhex("41f9000005004eb9000004404e75"), "",
                  ((0x440, bytes.fromhex("4e904e75")),)))
        for code, config, extra in cases:
            with self.subTest(code=code.hex(), config=config):
                catalog = self.catalog(self.PREFIX + code, discovery=config, extra=extra)
                self.assert_reached_target_writer(catalog)

    def test_unbounded_table_probe_does_not_supply_a_control_domain(self):
        # MOVEA.L (table,PC,D0.W),A0; NOP; NOP; JMP (A0).
        # D0 has no proved bound. The adjacent coherent descriptor is only a probe hit.
        dispatch = bytes.fromhex("207b003e4e714e714ed0")
        producer_pc = self.ROOT + len(self.PREFIX)
        transfer_pc = producer_pc + 8
        table_pc = producer_pc + 2 + 0x3e
        table = struct.pack(">III", self.TARGET, 0x540, 0xffffffff)
        extra = ((table_pc, table), (0x540, self.DATA))
        for rounds in (0, 6):
            for excluded in (False, True):
                with self.subTest(rounds=rounds, excluded=excluded):
                    catalog = self.catalog(self.PREFIX + dispatch, rounds=rounds,
                                           excluded=excluded, extra=extra)
                    candidates = [item for item in catalog["root_expansion_evidence"]
                                  if item.get("source_pc") == transfer_pc and "table_probe" in item]
                    self.assertEqual({item["entry"] for item in candidates}, {self.TARGET, 0x540})
                    for candidate in candidates:
                        target = candidate["entry"]
                        self.assert_unfollowed(catalog, candidate,
                                               excluded=excluded and target == self.TARGET, target=target)
                        self.assertEqual(candidate["instruction"]["bytes"], "4ed0")
                        self.assertEqual(candidate["instruction"]["pc"], transfer_pc)
                        probe = candidate["table_probe"]
                        self.assertEqual(probe["producer_pc"], producer_pc)
                        self.assertEqual(probe["producer_bytes"], dispatch[:4].hex())
                        self.assertTrue(probe["producer_is_reached_instruction"])
                        self.assertEqual(probe["table"], table_pc)
                        index = 0 if target == self.TARGET else 1
                        self.assertIn({"pc": table_pc + 4 * index,
                                       "bytes": struct.pack(">I", target).hex(),
                                       "index": index, "encoded_value": target},
                                      probe["matching_cells_in_bounded_probe"])
        # Explicit count is authoritative control metadata, unlike inferred probe extent.
        config = (f'[[discovery.jump_tables]]\naddress = {transfer_pc}\n'
                  f'table = {table_pc}\ncount = 1\n')
        catalog = self.catalog(self.PREFIX + dispatch, discovery=config, extra=extra)
        self.assert_reached_target_writer(catalog)
        transfer = next(node for routine in catalog["routines"] for node in routine["cfg"]
                        if node["pc"] == transfer_pc)
        self.assertEqual(transfer["transfer_targets"], [self.TARGET])
        self.assertEqual(transfer["successors"], [self.TARGET])
        instruction = next(insn for routine in catalog["routines"] for insn in routine["instructions"]
                           if insn["pc"] == transfer_pc)
        self.assertEqual(instruction["bytes"], dispatch[8:].hex())
        self.assertTrue(set(range(0x540, 0x540 + len(self.DATA), 2)).isdisjoint(self.decoded_pcs(catalog)))


if __name__ == "__main__":
    unittest.main()
