import tempfile
from pathlib import Path
import unittest

from decode_sound import Decoder, RECORD, records


class SoundDecodeTests(unittest.TestCase):
    def write(self, decoder, address, value, width=2):
        return list(decoder.decode((10, 0, 0xc17814, address, value, 3, width)))

    def test_voice_page_byte_lanes_and_fixed_point(self):
        d = Decoder()
        self.write(d, 0x20001e, 7)
        self.write(d, 0x200004, 0x1fff123f, 4)
        self.write(d, 0x200002, 0x401)
        self.write(d, 0x200011, 0xaa, 1)  # LVOL low byte is not connected.
        self.write(d, 0x30000f, 6, 1)
        event = self.write(d, 0x200000, 0x0308)[0]
        self.assertEqual(event['event'], 'voice_start')
        self.assertEqual(event['start_word'], (6 << 20) + (0x1fff1220 >> 9))
        self.assertEqual((event['frequency'], event['sample_step']), (0x400, 1.0))
        self.assertEqual((event['voice'], event['output_pair'], event['left_volume']), (7, 3, 128))
        self.assertTrue(event['loop'])
        self.write(d, 0x20001e, 39)
        self.write(d, 0x200004, 0)
        self.assertEqual(d.snapshot(7)['start_word'], event['start_word'])

    def test_board_reset_preserves_otis_but_invalidates_dsp(self):
        d = Decoder()
        self.write(d, 0x200002, 0x444)
        self.write(d, 0x260001, 0x56, 1)
        list(d.decode((20, 0, 0, 0, 2, 4, 0)))
        self.assertEqual(d.snapshot(0)['frequency'], 0x444)
        event = self.write(d, 0x260141, 3, 1)[0]
        self.assertEqual(event['gpr_value'], 0)
        self.write(d, 0x260101, 3, 1)  # Live DSP register select: value not observed.
        self.assertIsNone(self.write(d, 0x260141, 4, 1)[0]['gpr_value'])

    def test_trace_requires_end_and_monotonic_time(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'trace'
            start = RECORD.pack(9, 0, 0, 0xc00000, 3, 1, 1)
            end = RECORD.pack(10, 0, 0, 0, 0, 5, 0)
            for payload in (start, start + end[:-1], end + start,
                            start + RECORD.pack(8, 0, 0, 0, 0, 5, 0)):
                path.write_bytes(b'F3SND1\0\0' + payload)
                with self.assertRaises(ValueError):
                    list(records(path))
            path.write_bytes(b'F3SND1\0\0' + start + end)
            self.assertEqual([r[0] for r in records(path)], [9, 10])


if __name__ == '__main__':
    unittest.main()
