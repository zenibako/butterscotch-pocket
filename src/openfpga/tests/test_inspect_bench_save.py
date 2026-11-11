"""Exercise benchmark history triage using synthetic save contents."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOL = Path(__file__).resolve().parents[1] / 'tools/inspect_bench_save.py'
END = b'--- end of report ---\n'


class InspectBenchSaveTest(unittest.TestCase):
    def test_distinguishes_retained_copies_from_unseen_complete_reports(self):
        flowey_title = b'=== Undertale Photoshop Flowey benchmark, speed ==='
        flowey = flowey_title + b'\nFlowey vine pressure 100.0 101.0\n' + END
        later_flowey = flowey.replace(b'100.0 101.0', b'90.0 91.0')
        saber_title = b'=== Undertale Chaos Saber benchmark, accuracy, example-build ==='
        saber = saber_title + b'\nAsriel Chaos Saber 200.0 201.0\n' + END
        focus_title = b'=== Focused Deltarune benchmark tiles ==='
        focus = focus_title + b'\nbuild example\n' + END
        incomplete = b'=== Undertale incomplete benchmark, speed ===\n'
        with tempfile.TemporaryDirectory() as directory:
            earlier = Path(directory) / 'earlier.sav'
            latest = Path(directory) / 'latest.sav'
            earlier.write_bytes(flowey + b'\0' + later_flowey)
            data = flowey * 2 + later_flowey + incomplete + saber + focus + b'\0\n' + saber
            latest.write_bytes(data)
            output = subprocess.check_output([sys.executable, TOOL, latest, earlier], text=True)
            self.assertEqual(latest.read_bytes(), data)

        self.assertIn('5 complete blocks, 4 distinct, 1 incomplete', output)
        rows = [line.split('\t') for line in output.splitlines()[1:]]
        self.assertEqual([(status, copies, title) for status, copies, _, title in rows], [
            ('seen earlier', '2', flowey_title.decode()),
            ('unseen', '1', flowey_title.decode()),
            ('unseen', '1', saber_title.decode()),
            ('unseen', '1', focus_title.decode()),
        ])
        self.assertEqual(len({digest for _, _, digest, _ in rows}), 4)


if __name__ == '__main__':
    unittest.main()
