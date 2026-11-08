"""Validate the isolated bullet-hell core ZIP with an asset-free ELF fixture."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile

PORT = Path(__file__).resolve().parents[1]
DIST = PORT / 'dist'
RUNTIME = PORT / 'release/runtime'
CORE_ID = 'zenibako.BsBullet'
CORE = DIST / 'Cores/zenibako.Butterscotch'
INSTANCE = DIST / 'Assets/butterscotch/zenibako.Butterscotch/Undertale.json'
CORE_FILES = ('audio.json', 'core.json', 'data.json', 'icon.bin', 'info.txt',
              'input.json', 'interact.json', 'variants.json', 'video.json')
# ELF32 little-endian executable header with e_machine = RISC-V (243).
ELF_HEADER = struct.pack('<16sHHI28x', b'\x7fELF\x01\x01\x01', 2, 243, 1)
MODES = (
    ('Bullet Hell - Accuracy', 'accuracy', '--bench-bullet-hell'),
    ('Bullet Hell - Speed', 'speed', '--bench-bullet-hell-smooth'),
    ('Bullet Hell - No Render', 'no_render', '--bench-bullet-hell --draw-every 100000'),
    ('Bullet Hell - No Audio', 'no_audio', '--bench-bullet-hell --mute'),
    ('Bullet Hell - Core Only', 'core_only', '--bench-bullet-hell --draw-every 100000 --mute'),
    ('Bullet Hell - Profile', 'profile', '--bench-bullet-hell --scripts'),
    ('Flowey - Accuracy', 'fx_acc', '--bench-flowey'),
    ('Flowey - Speed', 'fx_speed', '--bench-flowey-smooth'),
    ('Flowey - No Render', 'fx_norend', '--bench-flowey --draw-every 100000'),
    ('Flowey - No Audio', 'fx_noaud', '--bench-flowey --mute'),
    ('Flowey - Core Only', 'fx_core', '--bench-flowey --draw-every 100000 --mute'),
    ('Flowey - Profile', 'fx_prof', '--bench-flowey --scripts'),
)
spec = importlib.util.spec_from_file_location('mkbullet', PORT / 'tools/mkbullet.py')
mkbullet = importlib.util.module_from_spec(spec)
sys.dont_write_bytecode = True
spec.loader.exec_module(mkbullet)


class BulletPackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.elf = self.root / 'app.elf'
        self.payload = ELF_HEADER + b'bs_bullet_0.sav\0bs_bullet_1.sav\0--bench-flowey\0--bench-flowey-smooth\0'
        self.elf.write_bytes(self.payload)
        self.output = self.root / 'bullet.zip'

    def tearDown(self):
        self.temporary.cleanup()

    def package(self, output=None):
        mkbullet.package(self.elf, self.output if output is None else output, '1.2.3-rc.4')

    def test_core_runtime_and_platform_match_tracked_inputs(self):
        # Catches altered templates, mismatched runtime files, or changed source inputs.
        sources = sorted(path for root in (DIST, RUNTIME) for path in root.rglob('*')
                         if path.is_file())
        before = {path: hashlib.sha256(path.read_bytes()).digest() for path in sources}
        self.package()
        self.assertEqual({path: hashlib.sha256(path.read_bytes()).digest() for path in sources}, before)
        with zipfile.ZipFile(self.output) as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual(len(archive.namelist()), 39)
            self.assertEqual(len(set(archive.namelist())), 39)
            for name in CORE_FILES:
                actual = archive.read(f'Cores/{CORE_ID}/{name}')
                if name == 'core.json':
                    core = json.loads(actual)
                    self.assertEqual(core['core']['metadata']['shortname'], 'BsBullet')
                    self.assertEqual(core['core']['metadata']['version'], '1.2.3-rc.4')
                    self.assertFalse(core['core']['framework']['sleep_supported'])
                    self.assertEqual(core['core']['cores'][0]['filename'], 'os25.rbf_r')
                    self.assertEqual(core['core']['cores'][0]['chip32_vm'], 'loader.bin')
                else:
                    self.assertEqual(actual, (CORE / name).read_bytes(), name)
            for name in ('os25.rbf_r', 'loader.bin'):
                self.assertEqual(archive.read(f'Cores/{CORE_ID}/{name}'), (RUNTIME / name).read_bytes())
            for name in ('butterscotch.json', '_images/butterscotch.bin'):
                self.assertEqual(archive.read(f'Platforms/{name}'), (DIST / 'Platforms' / name).read_bytes())
            self.assertEqual(archive.read('Assets/butterscotch/common/bs_bullet_os.bin'),
                             (RUNTIME / 'os.bin').read_bytes())
            self.assertEqual(archive.read('Assets/butterscotch/common/bs_bullet.elf'), self.payload)
            self.assertFalse(any(name.endswith(('.sav', '.win', 'music.bin', 'textures.bin'))
                                 for name in archive.namelist()))

    def test_all_entries_have_exact_arguments_and_isolated_slots(self):
        # Catches shared saves/ELF/OS/configs, changed game slots, and argument drift.
        original = json.loads(INSTANCE.read_text())
        game_slots = {slot['id']: slot['filename'] for slot in original['instance']['data_slots']}
        self.package()
        with zipfile.ZipFile(self.output) as archive:
            instance_names = []
            for label, mode, arguments in MODES:
                name = f'Assets/butterscotch/{CORE_ID}/{label}.json'
                instance_names.append(name)
                slots = {slot['id']: slot['filename']
                         for slot in json.loads(archive.read(name))['instance']['data_slots']}
                self.assertEqual(slots[1], 'bs_bullet_os.bin')
                self.assertEqual(slots[2], f'bs_bullet_{mode}.ini')
                self.assertEqual(slots[3], 'bs_bullet.elf')
                for slot in (4, 5, 6, 7):
                    self.assertEqual(slots[slot], game_slots[slot])
                for slot in range(10, 20):
                    self.assertEqual(slots[slot], f'bs_bullet_{slot - 10}.sav')
                self.assertTrue(all(len(filename.encode('utf-8')) <= 23 for filename in slots.values()))
                self.assertEqual(archive.read(f'Assets/butterscotch/common/{slots[2]}').decode(),
                                 f'[os]\nELF=bs_bullet.elf\nARGS={arguments}\nVARIANT=os25\n')
            self.assertEqual(len(set(instance_names)), 12)

    def test_repeated_packaging_is_byte_identical(self):
        # Catches nondeterministic entry order, timestamps, or generated metadata.
        second = self.root / 'second.zip'
        self.package()
        self.package(second)
        self.assertEqual(self.output.read_bytes(), second.read_bytes())
        with zipfile.ZipFile(self.output) as first, zipfile.ZipFile(second) as other:
            self.assertEqual(first.namelist(), other.namelist())
            for name in first.namelist():
                self.assertEqual(first.read(name), other.read(name), name)

    def test_refuses_to_replace_an_existing_output(self):
        # Catches destructive overwrite of a prior package.
        self.output.write_bytes(b'previous package')
        with self.assertRaises(FileExistsError):
            self.package()
        self.assertEqual(self.output.read_bytes(), b'previous package')

    def test_rejects_non_elf_before_creating_output(self):
        # Catches packaging an invalid program or leaving a new ZIP after rejection.
        self.elf.write_bytes(b'not an ELF')
        with self.assertRaises(ValueError):
            self.package()
        self.assertFalse(self.output.exists())

    def test_rejects_ordinary_undertale_elf(self):
        self.elf.write_bytes(ELF_HEADER + b'undertale_0.sav\0undertale_1.sav\0')
        with self.assertRaisesRegex(ValueError, 'APP=bs_bullet'):
            self.package()
        self.assertFalse(self.output.exists())

    def test_rejects_bullet_elf_without_flowey_tour(self):
        self.elf.write_bytes(ELF_HEADER + b'bs_bullet_0.sav\0bs_bullet_1.sav\0')
        with self.assertRaisesRegex(ValueError, 'Flowey tour'):
            self.package()
        self.assertFalse(self.output.exists())

    def test_rejects_elf_for_the_wrong_architecture(self):
        self.elf.write_bytes(self.payload[:18] + b'\x28\0' + self.payload[20:])
        with self.assertRaisesRegex(ValueError, 'RISC-V'):
            self.package()
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
