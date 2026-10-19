#!/usr/bin/env python3
"""Prepare a reviewable core-pin patch; builds always consume a committed lock."""
import argparse
import difflib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def pin_changes(root, candidate):
    """Plan lock and guide changes without altering SDK/runtime pins or files."""
    if not re.fullmatch(r"[0-9a-f]{40}", candidate):
        raise ValueError("candidate must be a full immutable commit")
    lock_path = root / "release/core.lock.json"
    lock = json.loads(lock_path.read_text())
    old = lock["core"]["commit"]
    if not re.fullmatch(r"[0-9a-f]{40}", old):
        raise ValueError("existing core pin must be a full immutable commit")
    if old == candidate:
        return {}
    guide = (root / "docs/BUILD.md").read_text()
    if old not in guide:
        raise ValueError("docs/BUILD.md does not contain the existing core pin; reconcile it first")
    lock["core"]["commit"] = candidate
    return {"release/core.lock.json": json.dumps(lock, indent=2) + "\n",
            "docs/BUILD.md": guide.replace(old, candidate)}


def git(source, *args):
    """Read Git state, propagating failed object lookups rather than guessing."""
    return subprocess.check_output(["git", "-C", str(source), *args], text=True).strip()


def patch_text(root, changes):
    """Render a patch applicable with git apply, without writing tracked files."""
    return "".join("".join(difflib.unified_diff(
        (root / name).read_text().splitlines(keepends=True), text.splitlines(keepends=True),
        fromfile="a/" + name, tofile="b/" + name)) for name, text in changes.items())


def prepare(root, source, output, apply=False):
    """Compare exact trees, write a patch/report, and optionally apply that patch.

    The source must be clean at the candidate, with the previous pin fetched.
    No branch, commit, push, PR or release is created by this command.
    """
    candidate = git(source, "rev-parse", "--verify", "HEAD^{commit}")
    if git(source, "status", "--porcelain", "--untracked-files=all"):
        raise ValueError("candidate core source must be a clean, dedicated checkout")
    old = json.loads((root / "release/core.lock.json").read_text())["core"]["commit"]
    changes = pin_changes(root, candidate)
    # Comparing endpoints avoids the misleading merge-base diff after a rewrite.
    names = git(source, "diff", "--name-status", old, candidate, "--")
    stat = git(source, "diff", "--stat", old, candidate, "--")
    code_changed = bool(git(source, "diff", "--name-only", old, candidate, "--", "src", "vendor"))
    patch = patch_text(root, changes)
    report = (
        f"# Core pin candidate\n\nPrevious: `{old}`\n\nCandidate: `{candidate}`\n\n"
        f"Update needed: {'yes' if changes else 'no'}\n\n"
        f"Changes under src/ or vendor/: {'yes' if code_changed else 'none'}\n\n"
        "This compares the two exact trees, not their merge base. Review all paths\n"
        "below, including build scripts outside src/. SDK/runtime pins and hashes\n"
        "are preserved. This report does not establish hardware compatibility.\n\n"
        f"```text\n{names or '(no file changes)'}\n\n{stat}\n```\n\n"
        "Apply core-pin.patch in a clean Pocket release checkout at the originating\n"
        "commit (git apply --check first), review, sign a commit, and push a branch\n"
        "to run Build and verify distribution. Do not publish until the complete\n"
        "distribution passes and required hardware checks are recorded.\n")
    output.mkdir(parents=True, exist_ok=True)
    (output / "core-pin.patch").write_text(patch)
    (output / "REVIEW.md").write_text(report)
    if apply:
        for name, text in changes.items():
            (root / name).write_text(text)
    print(report)
    return candidate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True,
                        help="clean core checkout at the candidate; fetch the previous pin too")
    parser.add_argument("--out", type=Path, default=ROOT / "out/core-update")
    parser.add_argument("--apply", action="store_true", help="also update this checkout's lock and guide")
    args = parser.parse_args()
    prepare(ROOT, args.source.resolve(), args.out.resolve(), args.apply)


if __name__ == "__main__":
    main()
