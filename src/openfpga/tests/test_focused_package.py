"""Validate the additive diagnostic ZIP using a tiny, asset-free ELF fixture."""
import importlib.util
import json
from pathlib import Path
import tempfile
import sys
import unittest
import zipfile

PORT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('mkfocus', PORT / 'tools/mkfocus.py')
mkfocus = importlib.util.module_from_spec(spec)
sys.dont_write_bytecode = True
spec.loader.exec_module(mkfocus)


class FocusedPackageTest(unittest.TestCase):
    def test_additive_entries_and_isolated_slots(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            elf = root / 'app.elf'
            elf.write_bytes(b'\x7fELFdiagnostic fixture')
            output = root / 'focus.zip'
            template = PORT / 'dist/Assets/butterscotch/zenibako.Butterscotch/Deltarune.json'
            original = template.read_bytes()
            mkfocus.package(elf, template, output)
            with zipfile.ZipFile(output) as archive:
                self.assertIsNone(archive.testzip())
                self.assertEqual(len(archive.namelist()), 11)
                self.assertEqual(len(set(archive.namelist())), 11)
                self.assertEqual(archive.read('Assets/butterscotch/common/deltarune_tiles.elf'), elf.read_bytes())
                self.assertFalse(any(n.endswith(('.sav', '.win', '.bin', '.ofsf')) for n in archive.namelist()))
                base = {s['id']: s['filename'] for s in json.loads(original)['instance']['data_slots']}
                for number, (mode, label) in enumerate(mkfocus.MODES, 1):
                    name = f'Assets/butterscotch/zenibako.Butterscotch/Deltarune Tiles {number} {label}.json'
                    slots = {s['id']: s['filename'] for s in json.loads(archive.read(name))['instance']['data_slots']}
                    self.assertEqual(slots[3], 'deltarune_tiles.elf')
                    for slot in [1, 4, 5, 6, 7]:
                        self.assertEqual(slots[slot], base[slot])
                    for slot in range(10, 20):
                        self.assertEqual(slots[slot], f'deltarune_tiles_{slot-10}.sav')
                    ini = archive.read('Assets/butterscotch/common/' + slots[2]).decode()
                    self.assertEqual(ini, f'[os]\nELF=deltarune_tiles.elf\nARGS=--focus-{mode}\nVARIANT=os20\n')
            self.assertEqual(template.read_bytes(), original)

    def test_reject_non_elf_before_writing_zip(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bad = root / 'bad'; bad.write_bytes(b'not an elf')
            with self.assertRaises(ValueError):
                mkfocus.package(bad, PORT / 'dist/Assets/butterscotch/zenibako.Butterscotch/Deltarune.json', root / 'out.zip')
            self.assertFalse((root / 'out.zip').exists())


if __name__ == '__main__':
    unittest.main()
