"""Pin refreshes must preserve matched dependencies and keep build instructions usable."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import subprocess
from contextlib import redirect_stdout
import io

spec = importlib.util.spec_from_file_location("update_core", Path(__file__).with_name("update_core.py"))
update_core = importlib.util.module_from_spec(spec)
spec.loader.exec_module(update_core)

OLD = "1" * 40
NEW = "2" * 40


class PinUpdateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "release").mkdir()
        (self.root / "docs").mkdir()
        self.lock = {"core": {"repository": "zenibako/Butterscotch", "commit": OLD},
                     "sdk": {"base_commit": "3" * 40},
                     "runtime": {"commit": "4" * 40},
                     "sha256": {"runtime.bin": "5" * 64}}
        (self.root / "release/core.lock.json").write_text(json.dumps(self.lock, indent=2) + "\n")
        self.guide = f"Core: `{OLD}`\nCheckout: `git checkout --detach {OLD}`\n"
        (self.root / "docs/BUILD.md").write_text(self.guide)

    def test_updates_both_documented_pins_without_changing_other_dependencies(self):
        changes = update_core.pin_changes(self.root, NEW)
        lock = json.loads(changes["release/core.lock.json"])
        self.assertEqual(lock["core"], {"repository": "zenibako/Butterscotch", "commit": NEW})
        for key in ("sdk", "runtime", "sha256"):
            self.assertEqual(lock[key], self.lock[key])
        self.assertEqual(changes["docs/BUILD.md"], f"Core: `{NEW}`\nCheckout: `git checkout --detach {NEW}`\n")
        # Planning must not change the files before source comparison succeeds.
        self.assertEqual((self.root / "docs/BUILD.md").read_text(), self.guide)

    def test_rejects_floating_or_malformed_candidate_without_writing(self):
        for candidate in ("main", "abc123", "A" * 40, "2" * 40 + "\n"):
            with self.subTest(candidate=candidate), self.assertRaises(ValueError):
                update_core.pin_changes(self.root, candidate)
        self.assertEqual(json.loads((self.root / "release/core.lock.json").read_text()), self.lock)

    def test_refuses_silent_documentation_drift(self):
        (self.root / "docs/BUILD.md").write_text("Core pin: stale documentation\n")
        with self.assertRaisesRegex(ValueError, "docs/BUILD.md"):
            update_core.pin_changes(self.root, NEW)
        self.assertEqual(json.loads((self.root / "release/core.lock.json").read_text()), self.lock)

    def test_same_pin_produces_no_changes(self):
        self.assertEqual(update_core.pin_changes(self.root, OLD), {})

    def core_checkout(self):
        """Reuse real committed objects locally without network or fixture commits."""
        source = self.root / "source"
        subprocess.run(["git", "clone", "--quiet", "--shared", str(update_core.ROOT), str(source)], check=True)
        candidate = update_core.git(source, "rev-parse", "HEAD")
        # CI checks out one commit. Do not depend on a parent being available.
        old = candidate
        self.lock["core"]["commit"] = old
        (self.root / "release/core.lock.json").write_text(json.dumps(self.lock, indent=2) + "\n")
        (self.root / "docs/BUILD.md").write_text(f"Core: {old}\nCheckout: {old}\n")
        return source, candidate

    def test_prepared_patch_applies_cleanly_and_updates_only_release_inputs(self):
        output = self.root / "candidate"
        output.mkdir()
        patch = output / "core-pin.patch"
        patch.write_text(update_core.patch_text(self.root, update_core.pin_changes(self.root, NEW)))
        subprocess.run(["git", "apply", "--check", str(patch)], cwd=self.root, check=True)
        subprocess.run(["git", "apply", str(patch)], cwd=self.root, check=True)
        lock = json.loads((self.root / "release/core.lock.json").read_text())
        self.assertEqual(lock["core"]["commit"], NEW)
        self.assertEqual(lock["sha256"], self.lock["sha256"])
        self.assertEqual((self.root / "docs/BUILD.md").read_text(), f"Core: `{NEW}`\nCheckout: `git checkout --detach {NEW}`\n")

    def test_prepare_with_current_pin_is_a_clean_noop(self):
        source, candidate = self.core_checkout()
        output = self.root / "candidate"
        original = (self.root / "release/core.lock.json").read_bytes()
        with redirect_stdout(io.StringIO()):
            update_core.prepare(self.root, source, output, apply=True)
        self.assertEqual((self.root / "release/core.lock.json").read_bytes(), original)
        self.assertEqual((output / "core-pin.patch").read_text(), "")
        self.assertIn(candidate, (output / "REVIEW.md").read_text())

    def test_dirty_core_or_missing_previous_object_cannot_apply_a_pin(self):
        source, _ = self.core_checkout()
        original = (self.root / "release/core.lock.json").read_bytes()
        (source / "untracked-private-data").write_text("not a release input")
        with self.assertRaisesRegex(ValueError, "clean"), redirect_stdout(io.StringIO()):
            update_core.prepare(self.root, source, self.root / "candidate", apply=True)
        self.assertEqual((self.root / "release/core.lock.json").read_bytes(), original)
        self.assertFalse((self.root / "candidate").exists())
        (source / "untracked-private-data").unlink()
        self.lock["core"]["commit"] = OLD
        (self.root / "release/core.lock.json").write_text(json.dumps(self.lock))
        (self.root / "docs/BUILD.md").write_text(OLD)
        with self.assertRaises(subprocess.CalledProcessError), redirect_stdout(io.StringIO()):
            update_core.prepare(self.root, source, self.root / "candidate", apply=True)
        self.assertEqual(json.loads((self.root / "release/core.lock.json").read_text())["core"]["commit"], OLD)
        self.assertFalse((self.root / "candidate").exists())


if __name__ == "__main__":
    unittest.main()
