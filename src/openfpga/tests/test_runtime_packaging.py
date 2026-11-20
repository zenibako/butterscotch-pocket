#!/usr/bin/env python3
"""Exercise the actual staging/card scripts using disposable, asset-free SDKs."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
REQUIRED = '52abf78bde0446bc4af8e23631f41ff950e5a535'


class RuntimePackaging(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.base = self.root / 'base'
        self.dest = self.root / 'dest'
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.base.mkdir()
        self.write(self.source / 'src/firmware/os/kernel/syscall.c', 'fsync source')
        self.write(self.source / 'src/firmware/os/bld/pocket/os.bin', 'fsync OS')
        build = self.source / 'src/fpga/targets/pocket/bld/os20'
        self.write(build / 'ap_core.qsf', 'VERILOG_MACRO INCLUDE_SAVESTATE\n')
        self.write(build / 'output_files/ap_core.sta.summary', 'Slack : 0.5\n')
        # Only VCS ancestry is simulated: artifact validation and shell entrypoints are real.
        self.write(self.bin / 'git', '''#!/bin/sh
shift 2
case "$1" in
merge-base) test "$3" = "$SUPPORTED_REV" ;;
rev-parse) echo "$SUPPORTED_REV" ;;
diff) exit "${DIRTY_SOURCE:-0}" ;;
ls-files) cat "$TRACKED_LIST" ;;
*) exit 80 ;;
esac
''', executable=True)
        self.write(self.source / 'Makefile', '''sdk:
\tmkdir -p "$(DEST)/runtime/pocket"
\tcp src/firmware/os/bld/pocket/os.bin "$(DEST)/runtime/pocket/os.bin"
\tprintf bitstream > "$(DEST)/runtime/pocket/os20.rbf_r"
\tprintf loader > "$(DEST)/runtime/pocket/loader.bin"
''')
        # All tracked build inputs precede the built OS.
        os.utime(self.source / 'src/firmware/os/kernel/syscall.c', (1, 1))
        os.utime(self.source / 'Makefile', (1, 1))
        tracked = self.root / 'tracked-inputs'
        tracked.write_bytes(b'src/firmware/os/kernel/syscall.c\0')
        self.env = dict(os.environ, TRACKED_LIST=str(tracked), PATH=f'{self.bin}:{os.environ["PATH"]}',
                        SUPPORTED_REV=REQUIRED, SAVESTATES='1')

    @staticmethod
    def write(path, data, executable=False):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(data)
        if executable:
            path.chmod(0o755)

    def stage(self):
        return subprocess.run(['bash', str(TOOLS / 'stage-savestate-sdk.sh'),
                               str(self.source), str(self.base), str(self.dest)],
                              env=self.env, text=True, capture_output=True)

    def card(self):
        return subprocess.run(['bash', str(TOOLS / 'mkcard.sh'), '/unused/image.sh',
                               str(self.dest), '/unused/dist', '/unused/port'],
                              env=self.env, text=True, capture_output=True)

    def valid_stage(self):
        result = self.stage()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_old_save_state_revision_rejected(self):
        for revision in ('6a9594f4c6add2cb33034b08cf4d18df684f11e2',
                         'c61ac344ca8762e57e9f5ca5556193894e02d92e'):
            with self.subTest(revision=revision):
                self.env['SUPPORTED_REV'] = revision
                self.assertNotEqual(self.stage().returncode, 0)
                self.assertFalse(self.dest.exists())

    def test_legacy_marker_does_not_authorize_card(self):
        self.write(self.dest / 'runtime/pocket/SAVESTATES_RUNTIME', 'legacy')
        result = self.card()
        self.assertIn('runtime provenance', result.stdout + result.stderr)
        self.assertNotIn('no game is built', result.stdout + result.stderr)

    def test_legacy_marker_cannot_invoke_image_assembly(self):
        self.write(self.dest / 'runtime/pocket/UNDERTALE_SAVESTATES', 'legacy')
        self.write(self.dest / '.obj/undertale/app.elf', 'synthetic program')
        port = self.root / 'port'
        self.write(port / 'games/undertale/data.win', 'synthetic data')
        self.write(port / 'games/undertale/textures.bin', 'synthetic textures')
        called = self.root / 'image-called'
        image = self.root / 'image.sh'
        self.write(image, f'#!/bin/bash\ntouch "{called}"\nexit 91\n')
        result = subprocess.run(['bash', str(TOOLS / 'mkcard.sh'), str(image),
                                 str(self.dest), str(self.root / 'dist'), str(port)],
                                env=self.env, text=True, capture_output=True)
        self.assertFalse(called.exists(), 'legacy marker reached image assembly')
        self.assertIn('runtime provenance', result.stderr)

    def test_missing_or_malformed_marker_is_rejected(self):
        for contents in (None, '{broken JSON', '[]', '{"version": true}'):
            with self.subTest(contents=contents):
                path = self.dest / 'runtime/pocket/SAVESTATES_RUNTIME.json'
                if contents is not None:
                    self.write(path, contents)
                result = self.card()
                self.assertIn('runtime provenance', result.stderr)
                self.assertNotIn('no game is built', result.stdout)

    def test_failed_stage_removes_inherited_provenance(self):
        self.write(self.base / 'runtime/pocket/SAVESTATES_RUNTIME.json', 'old marker')
        path = self.source / 'Makefile'
        path.write_text(path.read_text().replace('cp src/firmware/os/bld/pocket/os.bin',
                                               'printf old >'))
        os.utime(path, (1, 1))
        self.assertNotEqual(self.stage().returncode, 0)
        self.assertFalse((self.dest / 'runtime/pocket/SAVESTATES_RUNTIME.json').exists())

    def test_valid_staging_marker_is_accepted_before_asset_checks(self):
        self.valid_stage()
        result = self.card()
        self.assertIn('no game is built', result.stdout + result.stderr)
        marker = json.loads((self.dest / 'runtime/pocket/SAVESTATES_RUNTIME.json').read_text())
        self.assertEqual(marker['capabilities'], ['save-slot-fsync'])
        self.assertEqual(marker['source_commit'], REQUIRED)
        for name in ('os.bin', 'os20.rbf_r', 'loader.bin'):
            expected = hashlib.sha256((self.dest / 'runtime/pocket' / name).read_bytes()).hexdigest()
            self.assertEqual(marker['sha256'][name], expected)

    def test_modified_runtime_files_are_rejected(self):
        self.valid_stage()
        for name in ('os.bin', 'os20.rbf_r', 'loader.bin'):
            with self.subTest(name=name):
                path = self.dest / 'runtime/pocket' / name
                original = path.read_bytes()
                path.write_bytes(original + b'changed')
                result = self.card()
                self.assertIn('runtime provenance', result.stdout + result.stderr)
                self.assertNotIn('no game is built', result.stdout + result.stderr)
                path.write_bytes(original)

    def test_forged_capability_marker_is_rejected(self):
        self.valid_stage()
        path = self.dest / 'runtime/pocket/SAVESTATES_RUNTIME.json'
        marker = json.loads(path.read_text())
        marker['capabilities'] = []
        path.write_text(json.dumps(marker))
        self.assertIn('runtime provenance', self.card().stderr)

    def test_stale_built_os_is_rejected(self):
        os.utime(self.source / 'src/firmware/os/bld/pocket/os.bin', (0, 0))
        result = self.stage()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('stale', result.stderr)

    def test_supported_revision_with_stale_export_is_rejected(self):
        path = self.source / 'Makefile'
        path.write_text(path.read_text().replace('cp src/firmware/os/bld/pocket/os.bin',
                                               'printf old >'))
        os.utime(path, (1, 1))
        result = self.stage()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('built os.bin', result.stderr)
        self.assertFalse((self.dest / 'runtime/pocket/SAVESTATES_RUNTIME.json').exists())

    def test_newer_test_docs_and_unrelated_tools_do_not_invalidate_os(self):
        paths = ('src/firmware/os/kernel/test/pc/test_save_sync.py',
                 'src/firmware/os/kernel/test/pc/test_save.c',
                 'src/firmware/os/README.md', 'tools/check-unrelated.py',
                 'src/firmware/os/targets/mister/save.c')
        for name in paths:
            self.write(self.source / name, 'newer non-Pocket-build input')
            with open(self.env['TRACKED_LIST'], 'ab') as tracked:
                tracked.write(name.encode() + b'\0')
        self.valid_stage()

    def test_missing_exports_cannot_recertify_base_binaries(self):
        path = self.source / 'Makefile'
        original = path.read_text()
        for name in ('os20.rbf_r', 'loader.bin'):
            with self.subTest(name=name):
                self.dest = self.root / f'dest-without-{name}'
                self.write(self.base / 'runtime/pocket' / name, 'old inherited runtime')
                path.write_text('\n'.join(line for line in original.split('\n')
                                          if f'/runtime/pocket/{name}' not in line))
                os.utime(path, (1, 1))
                result = self.stage()
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(name, result.stderr)
                self.assertFalse((self.dest / 'runtime/pocket/SAVESTATES_RUNTIME.json').exists())

    def test_native_card_pins_os20_for_image_script(self):
        self.valid_stage()
        self.write(self.dest / '.obj/undertale/app.elf', 'synthetic program')
        port = self.root / 'port'
        self.write(port / 'games/undertale/data.win', 'synthetic data')
        called = self.root / 'image-variant'
        image = self.root / 'image.sh'
        self.write(image, f'#!/bin/bash\nprintf "%s" "$VARIANT" > "{called}"\nexit 91\n')
        for variant in ('os25', 'os30', None, 'os20'):
            with self.subTest(variant=variant):
                called.unlink(missing_ok=True)
                env = dict(self.env)
                env.pop('VARIANT', None)
                if variant is not None:
                    env['VARIANT'] = variant
                result = subprocess.run(['bash', str(TOOLS / 'mkcard.sh'), str(image),
                                         str(self.dest), '/unused/dist', str(port)],
                                        env=env, text=True, capture_output=True)
                if variant in ('os25', 'os30'):
                    self.assertFalse(called.exists())
                    self.assertIn('requires VARIANT=os20', result.stderr)
                else:
                    self.assertEqual(result.returncode, 91, result.stdout + result.stderr)
                    self.assertEqual(called.read_text(), 'os20')
                    called.unlink()

    def test_dirty_source_is_rejected(self):
        self.env['DIRTY_SOURCE'] = '1'
        result = self.stage()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('uncommitted', result.stderr)


if __name__ == '__main__':
    unittest.main()
