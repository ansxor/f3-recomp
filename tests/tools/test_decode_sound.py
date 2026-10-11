import tempfile
from pathlib import Path
import unittest

from tools.decode.trace import CommandRing, Decoder, RECORD, records


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

    def test_command_ring_wrap_and_deferred_dispatch(self):
        ring = CommandRing()
        ring.producer = 0x3fe
        packet = bytes.fromhex('068e01072768')
        for index, byte in enumerate(packet):
            list(ring.main_write(0xc00000 + ((0x3fe + index) & 0x3ff), byte))
        list(ring.main_write(0xc00480, 0))
        submitted = list(ring.main_write(0xc00481, 8))
        self.assertEqual(submitted[0]['payload'], [1, 7, 39, 104])
        consumed = ring.consume(0x1407fc, 6)
        # New publication must not overwrite an older queued task's identity.
        for index, byte in enumerate(bytes.fromhex('04860874')):
            list(ring.main_write(0xc00004 + index, byte))
        list(ring.main_write(0xc00481, 16))
        ring.consume(0x140008, 4)
        self.assertEqual(ring.dispatch(0x140008, 4)['command_id'], 2)
        self.assertEqual(ring.dispatch(0x1407fc, 6), consumed)
        self.assertEqual(consumed['command_id'], submitted[0]['command_id'])
        with self.assertRaisesRegex(ValueError, 'unconsumed'):
            ring.dispatch(0x1407fc, 6)

    def test_voice_origin_survives_note_node_reuse(self):
        d = Decoder()
        # Two note allocations reuse one kernel node but belong to different songs.
        def allocate(sequence):
            d.sequence_commands[sequence] = sequence + 100
            for address, value, size in ((0x101e, 0x5e5c + sequence * 0x28, 2),
                                         (0x2003, 7, 1), (0xd45e, 0x2768, 2)):
                list(d.decode((1, 0, 0, address, value, 7, size)))
            list(d.decode((1, 0, 0, 0xd44a, 0, 10, 0)))
            return list(d.decode((1, 0, 0, 0xee92, 0x20001010, 8, 0)))[0]
        first = allocate(8)
        for address, value in ((0xedee, 0xee92), (0xedf0, 0x1010)):
            list(d.decode((1, 0, 0, address, value, 7, 2)))
        d.voice_context(31, 0xede0)
        second = allocate(3)
        self.assertEqual(first['command_id'], 108)
        self.assertEqual(second['command_id'], 103)
        self.assertNotEqual(first['note_id'], second['note_id'])
        self.assertEqual(d.snapshot(31)['driver']['origin']['command_id'], 108)

    def test_trace_requires_end_and_monotonic_time(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'trace'
            start = RECORD.pack(9, 0, 0, 0xc00000, 3, 1, 1)
            end = RECORD.pack(10, 0, 0, 0, 0, 5, 0)
            for payload in (start, start + end[:-1], end + start,
                            start + RECORD.pack(8, 0, 0, 0, 0, 5, 0)):
                path.write_bytes(b'F3SND2\0\0' + payload)
                with self.assertRaises(ValueError):
                    list(records(path))
            path.write_bytes(b'F3SND2\0\0' + start + end)
            self.assertEqual([r[0] for r in records(path)], [9, 10])


if __name__ == '__main__':
    unittest.main()
