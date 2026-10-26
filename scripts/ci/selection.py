#!/usr/bin/env python3
"""Conservative CI routing and the final verdict. Uses only Python's stdlib."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys


JOB_DOMAINS = {
    "Unix": {"unix"}, "Windows": {"windows"}, "macOS": {"macos"},
    "Haiku": {"haiku"}, "Consoles": {"ps2", "ps3", "vita", "switch"},
    "Other": {"web", "android"}, "Compilers": {"compilers"},
    "Pocket": {"pocket"}, "PocketTools": {"pocket_tools"}, "Tests": {"tests"},
}
DOMAINS = frozenset().union(*JOB_DOMAINS.values())
WORKFLOWS = {
    "build-desktop.yml": {"unix"}, "build-windows.yml": {"windows"},
    "build-macos.yml": {"macos"}, "build-haiku.yml": {"haiku"},
    "build-console.yml": JOB_DOMAINS["Consoles"], "build-other.yml": JOB_DOMAINS["Other"],
    "build-old-compilers.yml": {"compilers"}, "build-pocket.yml": {"pocket"},
    "build-pocket-data-tools.yml": {"pocket_tools"}, "release-pocket.yml": {"pocket_tools"}, "publish-pocket.yml": {"pocket_tools"}, "tests.yml": {"tests"},
}
PORT_PREFIXES = {
    "src/ps2/": {"ps2"}, "src/audio/ps2/": {"ps2"}, "src/ps3/": {"ps3"},
    "src/vita/": {"vita"}, "src/switch/": {"switch"}, "src/web/": {"web"},
    "src/android/": {"android"}, "artifacts/windows/": {"windows"},
    "artifacts/macos/": {"macos"},
}
PORT_FILES = {
    "src/backends/appkit.m": {"macos"}, "src/backends/web.c": {"web"},
    "cmake/ppu.cmake": {"ps3"}, "cmake/llvm-mingw.cmake": {"windows"},
    "artifacts/zig-ar": {"unix"}, "artifacts/zig-ranlib": {"unix"},
    "src/runner_snapshot.c": {"pocket"}, "src/runner_snapshot.h": {"pocket"},
}
POCKET_TESTS = {
    "runner_snapshot_test.c", "data_win_room_cache_test.c", "of_snapshot_envelope_test.c",
    "of_save_snapshot_test.c", "of_audio_snapshot_test.c", "of_audio_silence_test.c",
    "of_pc_audio_snapshot_test.c", "sw_button_cache_test.c",
}
REFERENCE_INPUTS = {
    "src/openfpga/tools/prepare_game.py", "src/openfpga/tools/mktexpack.c",
    "src/openfpga/tools/mkmusic.c", "src/openfpga/platform/of_log.c",
    "src/openfpga/platform/of_music_pack.h",
}
DOC_SUFFIXES = {".md", ".rst", ".txt", ".png", ".svg", ".jpg", ".jpeg", ".gif", ".webp", ".pdf"}


def domains_for_path(path):
    # Never silently normalize an unexpected path into a narrower domain.
    if not isinstance(path, str) or not path or path.startswith("/") or "\\" in path:
        return DOMAINS
    if any(part in ("", ".", "..") for part in path.split("/")):
        return DOMAINS
    # of_log.c includes platform headers transitively; new headers must not
    # silently evade the Go integration test's C reference build.
    if path in REFERENCE_INPUTS or (path.startswith("src/openfpga/platform/") and path.endswith(".h")):
        return {"pocket", "pocket_tools"}
    if path.startswith("src/openfpga/tools/pocket-data/") or path == "src/openfpga/GAME-DATA.md":
        return {"pocket_tools"}
    if path.startswith(".github/workflows/"):
        return WORKFLOWS.get(path.removeprefix(".github/workflows/"), DOMAINS)
    if path.startswith("scripts/ci/"):
        return DOMAINS
    if path in PORT_FILES:
        return PORT_FILES[path]
    if path.startswith("tests/"):
        if path == "tests/try-catch-finally-test/data.win":
            return {"tests", "pocket"}
        if path.removeprefix("tests/") in POCKET_TESTS:
            return {"pocket"}
        return {"tests"}
    if path.startswith(("docs/", "src/openfpga/docs/", ".claude/skills/", ".agents/skills/")):
        return set() if Path(path).suffix.lower() in DOC_SUFFIXES else DOMAINS
    if path in {"README.md", "AGENTS.md", "CLAUDE.md"}:
        return set()
    if path == "LICENSE":  # Included in the downloadable preparation-tool bundles.
        return {"pocket_tools"}
    if path.startswith("src/openfpga/"):
        return {"pocket", "pocket_tools"}
    for prefix, domains in PORT_PREFIXES.items():
        if path.startswith(prefix):
            return domains
    # Shared renderer, VM, headers, vendor code, build system and new/unmapped
    # paths all take broad coverage, including the Go converter's C oracle.
    return DOMAINS


def select_paths(paths, full_matrix=False):
    if not isinstance(paths, list):
        raise ValueError("Expected a changed-path array")
    selected = set()
    for path in paths:
        selected.update(domains_for_path(path))
    return {**{name: name in selected for name in sorted(DOMAINS)}, "full_matrix": full_matrix}


def full_selection():
    return dict.fromkeys(sorted(DOMAINS) + ["full_matrix"], True)


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args], stderr=subprocess.PIPE)


def event_selection(event_name, event, repo):
    if event_name == "workflow_dispatch":
        return full_selection()
    if event_name == "pull_request":
        before = event["pull_request"]["base"]["sha"]
        after = event["pull_request"]["head"]["sha"]
    elif event_name == "push":
        before, after = event["before"], event["after"]
    else:
        raise ValueError("No supported diff for this event")
    for sha in (before, after):
        if not isinstance(sha, str) or not re.fullmatch(r"[0-9a-fA-F]{40,64}", sha) or set(sha) == {"0"}:
            raise ValueError("Missing or invalid diff revision")
    if event_name == "pull_request":
        before = git(repo, "merge-base", before, after).decode("ascii").strip()
    # --no-renames returns both the deleted and added paths when a file moves
    # between domains. -z handles spaces, tabs, and newlines without truncation.
    raw = git(repo, "diff", "--no-renames", "--name-only", "-z", before, after, "--")
    paths = [os.fsdecode(name) for name in raw.split(b"\0") if name]
    return select_paths(paths, full_matrix=event_name == "push")


def read_json(path):
    return json.loads(sys.stdin.read() if path == "-" else Path(path).read_text())


def check_results(needs):
    if set(needs) != {"Changes", *JOB_DOMAINS}:
        raise ValueError("Missing or unexpected CI job results")
    if needs["Changes"]["result"] != "success":
        raise ValueError("CI selection or its regression tests failed")
    selection = json.loads(needs["Changes"]["outputs"]["selection"])
    if set(selection) != DOMAINS | {"full_matrix"} or any(type(v) is not bool for v in selection.values()):
        raise ValueError("Missing or invalid domain selection")
    for job, domains in JOB_DOMAINS.items():
        result = needs[job]["result"]
        selected = any(selection[domain] for domain in domains)
        if result not in ("success", "skipped") or (selected and result != "success"):
            raise ValueError(f"{job}: {result}; selected={selected}")


def emit(selection):
    serialized = json.dumps(selection, sort_keys=True, separators=(",", ":"))
    print(serialized)
    if os.environ.get("GITHUB_OUTPUT"):
        with Path(os.environ["GITHUB_OUTPUT"]).open("a") as output:
            for name, value in selection.items():
                output.write(f"{name}={str(value).lower()}\n")
            output.write(f"selection={serialized}\n")
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        selected = ", ".join(name for name in sorted(DOMAINS) if selection[name]) or "No heavy jobs (documentation only)"
        with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a") as summary:
            summary.write(f"### CI selection\n\n{selected}\n\nExpanded Unix matrix: {selection['full_matrix']}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    select = commands.add_parser("select")
    select.add_argument("--paths-json", help="Preview a JSON changed-path array; '-' reads stdin")
    select.add_argument("--event-file", default=os.environ.get("GITHUB_EVENT_PATH"))
    select.add_argument("--event-name", default=os.environ.get("GITHUB_EVENT_NAME"))
    select.add_argument("--repo", default=".")
    check = commands.add_parser("check")
    check.add_argument("--needs-json", default="-")
    args = parser.parse_args()
    if args.command == "select":
        try:
            selection = (select_paths(read_json(args.paths_json)) if args.paths_json else
                         event_selection(args.event_name, read_json(args.event_file), args.repo))
        except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as error:
            print(f"Cannot determine a reliable diff; selecting full CI: {error}", file=sys.stderr)
            selection = full_selection()
        emit(selection)
        return 0
    try:
        check_results(read_json(args.needs_json))
    except (ValueError, KeyError, TypeError, OSError) as error:
        print(f"CI failed: {error}", file=sys.stderr)
        return 1
    print("All selected CI jobs passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
