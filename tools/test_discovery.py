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
        rom, config = self.fixture()
        config["discovery"]["actor_scripts"]["pointer_table_strides"] = [4, 8]
        rom[0x400:0x40e] = bytes.fromhex("203b007e4e714e712940001c4e75")
        struct.pack_into(">IIIII", rom, 0x480, 0x600, 0, 0x620, 1, 0xffffffff)
        for script, callback in ((0x600, 0x800), (0x620, 0x820)):
            struct.pack_into(">HIH", rom, script, 0, callback, 10)
            rom[callback:callback + 2] = bytes.fromhex("4e75")
        result = discover(bytes(rom), config)
        self.assertTrue({0x800, 0x820} <= result.instructions.keys())
        self.assertTrue({0x480, 0x488, 0x600, 0x620}.isdisjoint(result.instructions))

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


if __name__ == "__main__":
    unittest.main()
