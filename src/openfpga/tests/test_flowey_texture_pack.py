"""Check the private held-pose extension preserves the original pack bytes."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / 'tools/flowey_texture_pack.py'
SPEC = importlib.util.spec_from_file_location('flowey_texture_pack', TOOL)
pack = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pack)


class StalkPackTest(unittest.TestCase):
    def test_poses_follow_the_existing_payload_without_shifting_it(self):
        stock = b'UTX2' + struct.pack('<III', 1, 64, 1)
        stock += struct.pack('<HHIII', 64, 64, 0, 0, 0)
        with tempfile.TemporaryDirectory() as directory:
            captures = Path(directory)
            for pose in range(11):
                (captures / f'{pose}.rgb555').write_bytes(
                    struct.pack('<HHH', 180, 75, 0x8000 + pose) +
                    struct.pack('<H', 0x8000 + pose) * (180 * 75 - 1))
            extended = pack.append_stalks(stock, captures)
            self.assertTrue(extended.startswith(stock))
            self.assertEqual(extended[-4:], b'UST1')
            index_at = len(extended) - 4 - 11 * 12
            for pose in range(11):
                w, h, offset, length = struct.unpack_from('<HHII', extended, index_at + pose * 12)
                self.assertEqual((w, h, offset, length), (180, 75, len(stock) + 4 * pose, 4))
                self.assertEqual(extended[offset:offset + length],
                                 struct.pack('<HH', 0x8000 + w * h - 1, 0x8000 + pose))
            with self.assertRaises(ValueError):
                pack.append_stalks(extended, captures)
            (captures / '0.rgb555').write_bytes(b'truncated')
            with self.assertRaises(ValueError):
                pack.append_stalks(stock, captures)
