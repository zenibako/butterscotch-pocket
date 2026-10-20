"""Exercise the actual draft step without creating a GitHub release."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


class ReleaseNotesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.notes = self.root / 'docs/release-notes'
        self.notes.mkdir(parents=True)
        self.assets = self.root / 'release-assets'
        self.assets.mkdir()
        payload = b'synthetic public release payload'
        (self.assets / 'core.zip').write_bytes(payload)
        (self.assets / 'SHA256SUMS').write_text(hashlib.sha256(payload).hexdigest() + '  core.zip\n')
        self.published = self.root / 'published-body.txt'
        tools = self.root / 'bin'
        tools.mkdir()
        checksum = shutil.which('sha256sum') or shutil.which('gsha256sum')
        if checksum is None:
            self.skipTest('GNU sha256sum is required by the actual workflow step')
        (tools / 'sha256sum').symlink_to(checksum)
        # Only the external GitHub CLI boundary is replaced. Preserve both CLI
        # forms so the test observes an empty body from the old workflow.
        gh = tools / 'gh'
        gh.write_text('''#!/usr/bin/env python3
import os
from pathlib import Path
import sys
args = sys.argv[1:]
if '--notes-file' in args:
    body = Path(args[args.index('--notes-file') + 1]).read_text()
else:
    body = args[args.index('--notes') + 1]
Path(os.environ['PUBLISHED_BODY']).write_text(body)
''')
        gh.chmod(0o755)
        self.env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'],
                        GITHUB_WORKSPACE=str(self.root), GITHUB_SHA='1' * 40,
                        RELEASE_VERSION='v0.7.6', PUBLISHED_BODY=str(self.published))

    def run_step(self):
        workflow = (Path(__file__).resolve().parents[1] / '.github/workflows/build.yml').read_text()
        step = workflow.split('      - name: Create complete draft for manual publication\n', 1)[1]
        body = re.search(r'        run: \|\n((?:          [^\n]*\n?)+)', step).group(1)
        script = '\n'.join(line[10:] for line in body.splitlines())
        return subprocess.run(['bash', '-e', '-o', 'pipefail', '-c', script],
                              cwd=self.root, env=self.env, capture_output=True, text=True)

    def test_draft_publishes_notes_for_the_selected_version(self):
        expected = '# v0.7.6\n\nKnown issue: battle Memories can lose sprites.\n'
        (self.notes / 'v0.7.6.md').write_text(expected)
        (self.notes / 'v0.7.5.md').write_text('These are older release notes.\n')
        result = self.run_step()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.published.read_text(), expected)

    def test_missing_or_empty_notes_prevent_draft_creation(self):
        for content in (None, ''):
            with self.subTest(content=content):
                if content is not None:
                    (self.notes / 'v0.7.6.md').write_text(content)
                result = self.run_step()
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.published.exists())


if __name__ == '__main__':
    unittest.main()
