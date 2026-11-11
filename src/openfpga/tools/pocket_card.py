#!/usr/bin/env python3
"""Collect a Butterscotch benchmark save or install a core ZIP on a mounted Pocket card."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
from zipfile import BadZipFile, ZipFile


LOGS = ("bs_bullet", "undertale", "deltarune", "deltarune_tiles")
CORE_PAYLOADS = {
    "zenibako.BsBullet": {"bs_bullet.elf", "bs_bullet_os.bin"},
    "zenibako.Butterscotch": {"undertale.elf", "deltarune.elf", "os.bin"},
}
CORE_FILES = {
    "audio.json", "core.json", "data.json", "icon.bin", "info.txt",
    "input.json", "interact.json", "loader.bin", "updaters.json", "variants.json", "video.json",
}
COMMON = set.union(*CORE_PAYLOADS.values()) | {"undertale_os.ini", "deltarune_os.ini"}
PLATFORM_FILES = {"Platforms/butterscotch.json", "Platforms/_images/butterscotch.bin"}
SAVES = PurePosixPath("Saves/butterscotch/common")


def on_card(card, relative):
    path = card
    for part in relative.parts:
        path = path / part
        if path.is_symlink():
            raise ValueError(f"Refusing symlink on card: {path}")
    return path


def mounted_card(path):
    if not path.is_absolute():
        raise ValueError("Use the card host's absolute mount path")
    card = path.resolve(strict=True)
    if card == Path(card.anchor) or not card.is_mount():
        raise ValueError(f"Expected a mounted card root: {card}")
    for directory in ("Assets", "Cores"):
        if not stat.S_ISDIR(on_card(card, PurePosixPath(directory)).stat().st_mode):
            raise ValueError(f"Expected an initialized Pocket card: {card}")
    return card


def file_hash(path):
    with path.open("rb") as source:
        return stream_hash(source)


def stream_hash(source):
    digest = hashlib.sha256()
    for block in iter(lambda: source.read(1024 * 1024), b""):
        digest.update(block)
    return digest.hexdigest()


def read_log(card, log):
    if log not in LOGS:
        raise ValueError(f"Unknown benchmark log: {log}")
    path = on_card(card, SAVES / f"{log}_1.sav")
    if not stat.S_ISREG(path.stat().st_mode):
        raise ValueError(f"Expected a regular save file: {path}")
    raw = path.read_bytes()
    if not raw:
        raise ValueError(f"Empty benchmark save: {path}")
    meta = {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
    return json.dumps(meta).encode() + b"\n" + raw


def save_log(received, output):
    header, _, raw = received.partition(b"\n")
    meta = json.loads(header)
    if not raw or len(raw) != meta["bytes"] or hashlib.sha256(raw).hexdigest() != meta["sha256"]:
        raise ValueError("Save length or SHA-256 differs from the source")
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "wb") as dest:
            dest.write(raw)
        if file_hash(output) != meta["sha256"]:
            raise OSError("Saved copy failed read-back verification")
    except BaseException:
        output.unlink()
        raise
    return dict(meta, path=str(output))


def allowed_member(path):
    parts = path.parts
    name = path.name
    if len(parts) == 3 and parts[0] == "Cores" and parts[1] in CORE_PAYLOADS:
        return name in CORE_FILES or re.fullmatch(r"os\d+\.rbf_r", name)
    if len(parts) == 4 and parts[:2] == ("Assets", "butterscotch"):
        if parts[2] == "common":
            return name in COMMON or re.fullmatch(r"bs_bullet_[a-z0-9_]+\.ini", name)
        return parts[2] in CORE_PAYLOADS and path.suffix == ".json"
    return str(path) in PLATFORM_FILES


def package_members(archive):
    members = {}
    seen = set()
    for info in archive.infolist():
        if info.is_dir():
            continue
        path = PurePosixPath(info.filename)
        mode = stat.S_IFMT(info.external_attr >> 16)
        if (str(path) != info.filename or path.is_absolute() or "\\" in info.filename or
                ".." in path.parts or not allowed_member(path) or mode not in (0, stat.S_IFREG)):
            raise ValueError(f"Unexpected member in core ZIP: {info.filename}")
        key = info.filename.casefold()
        if key in seen:
            raise ValueError(f"Duplicate member in core ZIP: {info.filename}")
        seen.add(key)
        members[path] = info
    versions = {}
    for core, payload in CORE_PAYLOADS.items():
        folder = PurePosixPath("Cores") / core
        if not any(path.parent == folder for path in members):
            continue
        metadata = folder / "core.json"
        required = {metadata, folder / "loader.bin"} | {
            PurePosixPath("Assets/butterscotch/common") / name for name in payload
        }
        if required - members.keys() or not any(
            path.parent == folder and path.suffix == ".rbf_r" for path in members
        ):
            raise ValueError(f"Core ZIP lacks the complete {core} runtime or program")
        version = json.loads(archive.read(members[metadata]))["core"]["metadata"]["version"]
        if not isinstance(version, str) or not version:
            raise ValueError(f"Core ZIP has no version for {core}")
        versions[core] = version
    if not versions:
        raise ValueError("Expected a Butterscotch or Bullet Hell core ZIP")
    for path in members:
        if path.parts[0] != "Assets":
            continue
        core = path.parts[2]
        if core == "common":
            core = "zenibako.BsBullet" if path.name.startswith("bs_bullet") else "zenibako.Butterscotch"
        if core not in versions:
            raise ValueError(f"Asset belongs to a core absent from the ZIP: {path}")
    return members, versions


def install_overlay(archive, members, card):
    device = card.stat().st_dev
    stage = card / ".pocket-card-install"
    try:
        stage.mkdir(mode=0o700)
    except FileExistsError as error:
        raise ValueError(f"Another install or recovery is present at {stage}") from error
    prepared, replaced = [], []
    keep_stage = False
    try:
        # Finish reading and validating every member and reserving space for
        # replacements and backups before changing any installed file.
        for i, (relative, member) in enumerate(members.items()):
            target = on_card(card, relative)
            incoming = stage / f"new-{i}"
            with archive.open(member) as source, incoming.open("wb") as dest:
                shutil.copyfileobj(source, dest)
                dest.flush()
                os.fsync(dest.fileno())
            digest = file_hash(incoming)
            backup = None
            if target.exists():
                if file_hash(target) == digest:
                    continue
                backup = stage / f"old-{i}"
                shutil.copyfile(target, backup)
            prepared.append((relative, target, incoming, backup, digest))
        if not card.is_mount() or card.stat().st_dev != device:
            raise ValueError("Card mount changed during preparation")
        (stage / "recovery.json").write_text(json.dumps([
            {"path": str(relative), "backup": backup.name if backup else None, "sha256": digest}
            for relative, _, _, backup, digest in prepared
        ], indent=2) + "\n")
        for relative, target, incoming, backup, _ in prepared:
            on_card(card, relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            replaced.append((target, backup))
            os.replace(incoming, target)
        for relative, target, _, _, digest in prepared:
            if file_hash(on_card(card, relative)) != digest:
                raise OSError(f"Installed file failed read-back verification: {target}")
    except BaseException:
        try:
            for target, backup in reversed(replaced):
                if backup is None:
                    target.unlink(missing_ok=True)
                else:
                    os.replace(backup, target)
        except OSError as error:
            keep_stage = True
            raise OSError(f"Install rollback failed; recovery files are at {stage}: {error}") from error
        raise
    finally:
        if not keep_stage:
            shutil.rmtree(stage)
    return len(replaced)


def install_zip(card, source, expected, unmount):
    digest = stream_hash(source)
    if digest != expected:
        raise ValueError("ZIP SHA-256 differs from the expected build")
    source.seek(0)
    with ZipFile(source) as archive:
        members, versions = package_members(archive)
        changed = install_overlay(archive, members, card)
    result = {"sha256": digest, "versions": versions, "verified": len(members), "changed": changed,
              "unmounted": False}
    if unmount:
        cmd = ["diskutil", "unmount"] if sys.platform == "darwin" else ["umount"]
        try:
            subprocess.run([*cmd, str(card)], check=True, capture_output=True)
        except (OSError, subprocess.CalledProcessError) as error:
            detail = error.stderr.decode(errors="replace").strip() if isinstance(error, subprocess.CalledProcessError) else str(error)
            result["unmount_error"] = detail or str(error)
        else:
            result["unmounted"] = True
    return result


def remote(args, *values, source=None):
    if args.host.startswith("-"):
        raise ValueError("Use an SSH host alias or user@host")
    command = shlex.join([args.remote_python, "-c", Path(__file__).read_text(),
                          "--worker", args.action, str(args.card), *values])
    try:
        result = subprocess.run(["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", args.host, command],
                                stdin=source if source is not None else subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    except subprocess.CalledProcessError as error:
        raise OSError(error.stderr.decode(errors="replace").strip() or str(error)) from error
    return result.stdout


def worker(action, mount, *values):
    card = mounted_card(Path(mount))
    if action == "collect":
        sys.stdout.buffer.write(read_log(card, values[0]))
    elif action == "install":
        with tempfile.TemporaryFile() as source:
            shutil.copyfileobj(sys.stdin.buffer, source)
            source.seek(0)
            print(json.dumps(install_zip(card, source, values[0], values[1] == "unmount")))
    else:
        raise ValueError(f"Unknown card operation: {action}")


def main():
    if sys.argv[1:2] == ["--worker"]:
        worker(*sys.argv[2:])
        return 0
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    collect = actions.add_parser("collect", help="copy one raw benchmark save")
    collect.add_argument("--log", choices=LOGS, required=True)
    collect.add_argument("--out", type=Path, required=True, help="new local file in a private directory")
    install = actions.add_parser("install", help="install and verify an existing core ZIP")
    install.add_argument("--zip", type=Path, required=True)
    install.add_argument("--sha256", help="expected archive checksum from the build")
    install.add_argument("--unmount", action="store_true", help="unmount after successful verification")
    for action in (collect, install):
        action.add_argument("--card", type=Path, required=True, help="absolute mount on the card host")
        action.add_argument("--host", help="SSH alias or user@host when the card is elsewhere")
        action.add_argument("--remote-python", default="python3", help="Python executable on the SSH host")
    args = parser.parse_args()
    card = None if args.host else mounted_card(args.card)
    if args.action == "collect":
        if args.out.exists() or args.out.is_symlink():
            raise ValueError(f"Choose a fresh output path: {args.out}")
        raw = remote(args, args.log) if args.host else read_log(card, args.log)
        result = save_log(raw, args.out)
    else:
        with args.zip.open("rb") as source:
            expected = stream_hash(source)
            if args.sha256 and expected != args.sha256.lower():
                raise ValueError("ZIP SHA-256 differs from the expected build")
            source.seek(0)
            if args.host:
                result = json.loads(remote(args, expected, "unmount" if args.unmount else "keep", source=source))
                if result["sha256"] != expected:
                    raise ValueError("SSH host verified a different ZIP SHA-256")
            else:
                result = install_zip(card, source, expected, args.unmount)
    print(json.dumps(result, sort_keys=True))
    if "unmount_error" in result:
        print(f"Core files verified, card still mounted: {result['unmount_error']}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, BadZipFile) as error:
        sys.exit(f"pocket-card: {error}")
