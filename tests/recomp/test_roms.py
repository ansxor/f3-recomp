"""Behavioral checks for sparse grouped lanes, sound mirrors and validated dump variants."""
import hashlib
from pathlib import Path
import tempfile
import unittest
import zlib

from recomp.roms import load_region


def lane(name, data, offset=0, stride=1, group=1):
    return {"file": name, "size": len(data), "offset": offset, "stride": stride, "group": group,
            "crc": f"{zlib.crc32(data):08x}", "sha1": hashlib.sha1(data).hexdigest()}


class RomRegionTests(unittest.TestCase):
    def test_grouped_tile_lanes_and_sparse_zero_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            root.joinpath("low").write_bytes(bytes([1, 2, 3, 4]))
            root.joinpath("mid").write_bytes(bytes([5, 6, 7, 8]))
            config = {"tiles": {"size": 8, "lanes": [lane("low", bytes([1, 2, 3, 4]), 0, 4, 2),
                                                      lane("mid", bytes([5, 6, 7, 8]), 2, 4, 2)]}}
            self.assertEqual(load_region(config, "tiles", root), bytes([1, 2, 5, 6, 3, 4, 7, 8]))
            config = {"samples": {"size": 12, "lanes": [lane("low", bytes([1, 2, 3, 4]), 4, 2)]}}
            self.assertEqual(load_region(config, "samples", root), bytes([0, 0, 0, 0, 1, 0, 2, 0, 3, 0, 4, 0]))

    def test_sound_mirror_keeps_both_banks_and_rejects_corruption(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = bytes([1, 2, 3, 4])
            root.joinpath("program").write_bytes(data)
            config = {"sound": {"size": 4, "mirror_size": 8, "lanes": [lane("program", data)]}}
            self.assertEqual(load_region(config, "sound", root), bytes([1, 2, 3, 4, 1, 2, 3, 4]))
            root.joinpath("program").write_bytes(bytes([1, 2, 3, 5]))
            with self.assertRaisesRegex(ValueError, "CRC32 mismatch"):
                load_region(config, "sound", root)

    def test_continue_then_patch_preserves_gseeker_lane_order(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            mask03 = bytes(range(1, 9))
            mask04 = bytes(range(9, 17))
            patch15 = bytes(range(0x21, 0x25))
            patch16 = bytes(range(0x31, 0x35))
            for name, data in (("d40_03.rom", mask03), ("d40_04.rom", mask04),
                               ("d40_15.rom", patch15), ("d40_16.rom", patch16)):
                root.joinpath(name).write_bytes(data)
            first_half = lane("d40_04.rom", mask04, 9, 2)
            first_half.update(source_offset=0, length=4)
            continued_half = lane("d40_04.rom", mask04, 0, 2)
            continued_half.update(source_offset=4, length=4)
            lanes = [lane("d40_03.rom", mask03, 0, 2), first_half, continued_half]
            config = {"sprites": {"size": 16, "fill": 0, "lanes": lanes}}
            self.assertEqual(load_region(config, "sprites", root),
                             bytes([13, 0, 14, 0, 15, 0, 16, 0, 5, 9, 6, 10, 7, 11, 8, 12]))
            # Scaled MAME geometry: patches replace both lower lanes after CONTINUE.
            lanes.extend([lane("d40_15.rom", patch15, 0, 2),
                          lane("d40_16.rom", patch16, 1, 2)])
            self.assertEqual(load_region(config, "sprites", root),
                             bytes([0x21, 0x31, 0x22, 0x32, 0x23, 0x33, 0x24, 0x34,
                                    5, 9, 6, 10, 7, 11, 8, 12]))

    def test_continue_validates_bytes_outside_selected_source_slice(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            chip = bytes(range(1, 9))
            for source_offset in (0, 4):
                with self.subTest(source_offset=source_offset):
                    spec = lane("d40_04.rom", chip, 1, 2)
                    spec.update(source_offset=source_offset, length=4)
                    config = {"sprites": {"size": 8, "fill": 0, "lanes": [spec]}}
                    root.joinpath("d40_04.rom").write_bytes(chip)
                    selected = chip[source_offset:source_offset + 4]
                    self.assertEqual(load_region(config, "sprites", root),
                                     bytes([0, selected[0], 0, selected[1],
                                            0, selected[2], 0, selected[3]]))
                    corrupt = bytearray(chip)
                    corrupt[4 if source_offset == 0 else 0] ^= 0xff
                    root.joinpath("d40_04.rom").write_bytes(corrupt)
                    with self.assertRaisesRegex(ValueError, "CRC32 mismatch"):
                        load_region(config, "sprites", root)

    def test_only_hash_validated_short_dumps_are_ff_padded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            short = bytes([1, 2])
            full = short + b"\xff\xff"
            spec = lane("chip", full)
            spec.update(short_size=2, short_crc=f"{zlib.crc32(short):08x}",
                        short_sha1=hashlib.sha1(short).hexdigest())
            config = {"sound": {"size": 4, "fill": 255, "lanes": [spec]}}
            root.joinpath("chip").write_bytes(short)
            self.assertEqual(load_region(config, "sound", root), full)
            root.joinpath("chip").write_bytes(bytes([1, 3]))
            with self.assertRaisesRegex(ValueError, "CRC32 mismatch"):
                load_region(config, "sound", root)

    def test_group_cannot_write_past_region(self):
        config = {"tiles": {"size": 7, "lanes": [lane("chip", bytes([1, 2, 3, 4]), 2, 4, 2)]}}
        with self.assertRaisesRegex(ValueError, "exceeds region"):
            load_region(config, "tiles", Path("unused"))


if __name__ == "__main__":
    unittest.main()
