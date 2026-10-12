import tempfile
import unittest
from pathlib import Path

from tools.decode.trace import RECORD, CommandRing, Decoder, records


class SoundDecodeTests(unittest.TestCase):
    def write(self, decoder, address, value, width=2):
        return list(decoder.decode((10, 0, 0xC17814, address, value, 3, width)))

    def test_voice_page_byte_lanes_and_fixed_point(self):
        d = Decoder()
        self.write(d, 0x20001E, 7)
        self.write(d, 0x200004, 0x1FFF123F, 4)
        self.write(d, 0x200002, 0x401)
        self.write(d, 0x200011, 0xAA, 1)  # LVOL low byte is not connected.
        self.write(d, 0x30000F, 6, 1)
        event = self.write(d, 0x200000, 0x0308)[0]
        self.assertEqual(event["event"], "voice_start")
        self.assertEqual(event["start_word"], (6 << 20) + (0x1FFF1220 >> 9))
        self.assertEqual((event["frequency"], event["sample_step"]), (0x400, 1.0))
        self.assertEqual((event["voice"], event["output_pair"], event["left_volume"]), (7, 3, 128))
        self.assertTrue(event["loop"])
        self.write(d, 0x20001E, 39)
        self.write(d, 0x200004, 0)
        self.assertEqual(d.snapshot(7)["start_word"], event["start_word"])

    def test_board_reset_preserves_otis_but_invalidates_dsp(self):
        d = Decoder()
        self.write(d, 0x200002, 0x444)
        self.write(d, 0x260001, 0x56, 1)
        list(d.decode((20, 0, 0, 0, 2, 4, 0)))
        self.assertEqual(d.snapshot(0)["frequency"], 0x444)
        event = self.write(d, 0x260141, 3, 1)[0]
        self.assertEqual(event["gpr_value"], 0)
        self.write(d, 0x260101, 3, 1)  # Live DSP register select: value not observed.
        self.assertIsNone(self.write(d, 0x260141, 4, 1)[0]["gpr_value"])

    def test_command_ring_wrap_and_deferred_dispatch(self):
        ring = CommandRing()
        ring.producer = 0x3FE
        packet = bytes.fromhex("068e01072768")
        for index, byte in enumerate(packet):
            list(ring.main_write(0xC00000 + ((0x3FE + index) & 0x3FF), byte))
        list(ring.main_write(0xC00480, 0))
        submitted = list(ring.main_write(0xC00481, 8))
        self.assertEqual(submitted[0]["payload"], [1, 7, 39, 104])
        consumed = ring.consume(0x1407FC, 6)
        # New publication must not overwrite an older queued task's identity.
        for index, byte in enumerate(bytes.fromhex("04860874")):
            list(ring.main_write(0xC00004 + index, byte))
        list(ring.main_write(0xC00481, 16))
        ring.consume(0x140008, 4)
        self.assertEqual(ring.dispatch(0x140008, 4)["command_id"], 2)
        self.assertEqual(ring.dispatch(0x1407FC, 6), consumed)
        self.assertEqual(consumed["command_id"], submitted[0]["command_id"])
        with self.assertRaisesRegex(ValueError, "unconsumed"):
            ring.dispatch(0x1407FC, 6)

    def test_command_lifecycle_in_decoder(self):
        d = Decoder()
        packet = bytes.fromhex("068e01072768")
        for index, byte in enumerate(packet):
            events = list(d.decode((10, 0, 0x1234, 0xC00000 + index, byte, 1, 1)))
            self.assertEqual(len(events), 1)
            self.assertEqual(events[0]["event"], "main_write")
        list(d.decode((11, 0, 0x1234, 0xC00480, 0, 1, 1)))
        events = list(d.decode((12, 0, 0x1234, 0xC00481, 12, 1, 1)))
        self.assertEqual(len(events), 2)
        self.assertEqual(events[0]["event"], "command")
        self.assertEqual(events[0]["command_id"], 1)
        self.assertEqual(events[0]["payload"], [1, 7, 39, 104])
        self.assertEqual(events[1]["event"], "main_write")

        # Sound CPU consumes the command
        consumed = list(d.decode((20, 0, 0xC11116, 0x140000, 6, 2, 2)))
        self.assertEqual(consumed[0]["event"], "command_consumed")
        self.assertEqual(consumed[0]["command_id"], 1)

        # Sound CPU dispatches the command
        dispatched = list(d.decode((30, 0, 0xC12ECC, 0x140000, 6, 2, 2)))
        self.assertEqual(dispatched[0]["event"], "command_dispatch")
        self.assertEqual(dispatched[0]["command_id"], 1)
        self.assertEqual(dispatched[0]["payload"], [1, 7, 39, 104])

        # Removed kinds now raise ValueError
        for removed_kind in (6, 7, 8, 9, 10, 11):
            with self.assertRaises(ValueError):
                list(d.decode((40, 0, 0, 0, 0, removed_kind, 0)))

    def test_trace_requires_end_and_monotonic_time(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace"
            start = RECORD.pack(9, 0, 0, 0xC00000, 3, 1, 1)
            end = RECORD.pack(10, 0, 0, 0, 0, 5, 0)
            for payload in (start, start + end[:-1], end + start, start + RECORD.pack(8, 0, 0, 0, 0, 5, 0)):
                path.write_bytes(b"F3SND2\0\0" + payload)
                with self.assertRaises(ValueError):
                    list(records(path))
            path.write_bytes(b"F3SND2\0\0" + start + end)
            self.assertEqual([r[0] for r in records(path)], [9, 10])


if __name__ == "__main__":
    unittest.main()
