"""Exercise the card workflow against disposable mounts and an SSH transport shim."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from zipfile import ZipFile


TOOL = Path(__file__).resolve().parents[1] / "tools/pocket_card.py"
CORE = "Cores/zenibako.BsBullet"
ELF = "Assets/butterscotch/common/bs_bullet.elf"
SAVES = "Saves/butterscotch/common"


class CardTest(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.root = Path(work.name)
        self.card = self.root / "Pocket's SD"
        (self.card / CORE).mkdir(parents=True)
        (self.card / ELF).parent.mkdir(parents=True)
        (self.card / SAVES).mkdir(parents=True)
        self.raw = b"benchmark\r\n--- end of report ---\r\n\0\0\0"
        self.save = self.card / SAVES / "bs_bullet_1.sav"
        self.save.write_bytes(self.raw)
        self.progress = self.card / SAVES / "undertale_0.sav"
        self.progress.write_bytes(b"player progress")
        self.game = self.card / ELF.replace("bs_bullet.elf", "data.win")
        self.game.write_bytes(b"game data")
        (self.card / ELF).write_bytes(b"old ELF")
        self.zip = self.root / "test build.zip"
        self.make_zip()
        # Treat only the fixture directory as mounted. The real tool has no
        # alternate path around the mount check.
        (self.root / "sitecustomize.py").write_text(
            "import os\nfrom pathlib import Path\n"
            "Path.is_mount = lambda self: str(self) == os.environ.get('CARD_TEST_MOUNT')\n"
            "replace = os.replace\n"
            "def fail_replace(src, dest):\n"
            "    if Path(src).name == os.environ.get('CARD_TEST_FAIL_REPLACE'):\n"
            "        raise OSError('simulated card write failure')\n"
            "    replace(src, dest)\n"
            "os.replace = fail_replace\n"
        )
        shim = self.root / "ssh"
        shim.write_text(
            "#!/bin/sh\n"
            'while [ "$#" -gt 2 ]; do shift; done\n'
            '[ "$1" = fixture-host ] || exit 99\n'
            'exec /bin/sh -c "$2"\n'
        )
        shim.chmod(0o700)
        self.env = dict(os.environ, PYTHONPATH=str(self.root),
                        PATH=str(self.root) + os.pathsep + os.environ["PATH"],
                        CARD_TEST_MOUNT=str(self.card.resolve()))

    def make_zip(self, extra=None):
        files = {
            f"{CORE}/core.json": json.dumps({"core": {"metadata": {"version": "test-rc"}}}),
            f"{CORE}/loader.bin": b"loader",
            f"{CORE}/os25.rbf_r": b"fpga",
            ELF: b"new ELF",
            "Assets/butterscotch/common/bs_bullet_os.bin": b"runtime",
            "Platforms/butterscotch.json": b"{}",
        }
        with ZipFile(self.zip, "w") as archive:
            for path, data in (files | (extra or {})).items():
                archive.writestr(path, data)

    def card_tool(self, action, *args, remote=False, env=None):
        host = ("--host", "fixture-host", "--remote-python", sys.executable) if remote else ()
        return subprocess.run(
            [sys.executable, str(TOOL), action, "--card", str(self.card), *host, *args],
            env=env or self.env, capture_output=True, text=True, timeout=30,
        )

    def test_collect_exact_save_local_and_remote_without_overwriting(self):
        for remote in (False, True):
            out = self.root / f"snapshot-{remote}.sav"
            args = ("--log", "bs_bullet", "--out", str(out))
            run = self.card_tool("collect", *args, remote=remote)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(out.read_bytes(), self.raw)
            self.assertEqual(out.stat().st_mode & 0o777, 0o600)
            self.assertIn(hashlib.sha256(self.raw).hexdigest(), run.stdout)
            self.assertNotEqual(self.card_tool("collect", *args, remote=remote).returncode, 0)
        self.assertEqual(self.save.read_bytes(), self.raw)

    def test_install_remote_verified_zip_and_repeat(self):
        args = ("--zip", str(self.zip), "--sha256", hashlib.sha256(self.zip.read_bytes()).hexdigest())
        run = self.card_tool("install", *args, remote=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(json.loads(run.stdout)["versions"], {"zenibako.BsBullet": "test-rc"})
        self.assertEqual((self.card / ELF).read_bytes(), b"new ELF")
        self.assertEqual(self.progress.read_bytes(), b"player progress")
        self.assertEqual(self.game.read_bytes(), b"game data")
        self.assertEqual(self.save.read_bytes(), self.raw)
        again = self.card_tool("install", *args, remote=True)
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertEqual(json.loads(again.stdout)["changed"], 0)
        self.assertFalse(list(self.card.glob(".pocket-card-*")))

    def test_unmount_follows_install_and_preserves_verified_files_on_failure(self):
        unmount = self.root / ("diskutil" if sys.platform == "darwin" else "umount")
        unmount.write_text(
            "#!/bin/sh\n"
            '[ "$1" = unmount ] && shift\n'
            f'[ "$(cat "$1/{ELF}")" = "new ELF" ] || exit 9\n'
            '[ "$CARD_TEST_UNMOUNT_FAIL" = yes ] && { echo busy >&2; exit 1; }\n'
            'touch "$1/unmount-called"\n'
        )
        unmount.chmod(0o700)
        args = ("--zip", str(self.zip), "--unmount")
        failure = self.card_tool("install", *args, remote=True,
                                 env=dict(self.env, CARD_TEST_UNMOUNT_FAIL="yes"))
        self.assertEqual(failure.returncode, 1, failure.stdout)
        self.assertIn("Core files verified, card still mounted: busy", failure.stderr)
        self.assertEqual((self.card / ELF).read_bytes(), b"new ELF")
        success = self.card_tool("install", *args, remote=True)
        self.assertEqual(success.returncode, 0, success.stderr)
        self.assertTrue(json.loads(success.stdout)["unmounted"])
        self.assertTrue((self.card / "unmount-called").exists())

    def test_install_rejects_save_game_and_traversal_members(self):
        for name in (f"{SAVES}/undertale_0.sav", "Assets/butterscotch/common/data.win",
                     "Assets/butterscotch/common/os.bin", f"{CORE}/../../outside"):
            with self.subTest(name=name):
                self.make_zip({name: b"bad"})
                run = self.card_tool("install", "--zip", str(self.zip))
                self.assertNotEqual(run.returncode, 0, run.stdout)
                self.assertEqual((self.card / ELF).read_bytes(), b"old ELF")
                self.assertEqual(self.progress.read_bytes(), b"player progress")
                self.assertEqual(self.game.read_bytes(), b"game data")

    def test_install_requires_mount_and_matching_checksum(self):
        args = ("--zip", str(self.zip))
        self.assertNotEqual(self.card_tool("install", *args, "--sha256", "0" * 64).returncode, 0)
        self.assertNotEqual(self.card_tool("install", *args, env=dict(self.env, CARD_TEST_MOUNT="")).returncode, 0)
        self.assertEqual((self.card / ELF).read_bytes(), b"old ELF")

    def test_partial_install_rolls_back(self):
        run = self.card_tool("install", "--zip", str(self.zip), remote=True,
                             env=dict(self.env, CARD_TEST_FAIL_REPLACE="new-4"))
        self.assertNotEqual(run.returncode, 0, run.stdout)
        self.assertIn("simulated card write failure", run.stderr)
        self.assertEqual((self.card / ELF).read_bytes(), b"old ELF")
        self.assertFalse((self.card / CORE / "core.json").exists())
        self.assertFalse(list(self.card.glob(".pocket-card-*")))

    def test_install_refuses_symlink_or_existing_install(self):
        outside = self.root / "unrelated.elf"
        outside.write_bytes(b"unrelated")
        target = self.card / ELF
        target.unlink()
        target.symlink_to(outside)
        args = ("--zip", str(self.zip))
        self.assertNotEqual(self.card_tool("install", *args).returncode, 0)
        self.assertEqual(outside.read_bytes(), b"unrelated")
        target.unlink()
        target.write_bytes(b"old ELF")
        (self.card / ".pocket-card-install").mkdir()
        self.assertNotEqual(self.card_tool("install", *args).returncode, 0)
        self.assertEqual(target.read_bytes(), b"old ELF")


if __name__ == "__main__":
    unittest.main()
