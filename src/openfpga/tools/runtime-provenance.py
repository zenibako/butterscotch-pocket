#!/usr/bin/env python3
"""Bind a staged save-slot-fsync runtime to its exported bytes.

This is a local build provenance/corruption guard, not a signed attestation.
Only the staging command should create the marker.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

REQUIRED_REVISION = '52abf78bde0446bc4af8e23631f41ff950e5a535'
FILES = ('os.bin', 'os20.rbf_r', 'loader.bin')
MARKER = 'SAVESTATES_RUNTIME.json'
# Match Pocket firmware compile/link inputs, excluding test fixtures and other
# targets. Unrelated docs/tools must not require a firmware rebuild.
SOURCE_DIRS = ('src/firmware/os/hal/', 'src/firmware/os/kernel/',
               'src/firmware/os/shared/', 'src/firmware/os/targets/pocket/',
               'src/firmware/api/', 'src/firmware/lib/iso9660/',
               'src/firmware/musl/include/')
BUILD_INPUTS = ('Makefile', 'src/firmware/os/Makefile', 'src/firmware/os/os.ld',
                'src/fpga/targets/pocket/Makefile', 'tools/firmware-container.sh',
                'tools/oci.sh', 'tools/docker/Dockerfile.firmware',
                'src/firmware/musl/build_musl.sh',
                'src/firmware/os/targets/pocket/boot/append_os_crc.py')


def is_build_input(name):
    path = Path(name)
    return name in BUILD_INPUTS or (
        name.startswith(SOURCE_DIRS) and not {'test', 'tests'} & set(path.parts)
        and path.suffix in ('.c', '.h', '.S', '.s', '.ld'))


def git(source, *args):
    return subprocess.check_output(['git', '-C', str(source), *args])


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_evidence(source):
    try:
        git(source, 'merge-base', '--is-ancestor', REQUIRED_REVISION, 'HEAD')
    except subprocess.CalledProcessError:
        raise ValueError(f'Build zenibako/openfpgaCore at {REQUIRED_REVISION} or later')
    paths = git(source, 'ls-files', '-z', '--', 'src/firmware', 'tools',
                'Makefile', 'src/fpga/targets/pocket/Makefile').split(b'\0')
    inputs = [name.decode() for name in filter(None, paths) if is_build_input(name.decode())]
    if not inputs:
        raise ValueError('no tracked Pocket firmware build inputs found')
    try:
        git(source, 'diff', '--quiet', 'HEAD', '--', *inputs)
    except subprocess.CalledProcessError:
        raise ValueError('runtime build inputs have uncommitted changes; commit and rebuild')
    built = source / 'src/firmware/os/bld/pocket/os.bin'
    built_time = built.stat().st_mtime_ns
    for name in inputs:
        path = source / name
        if path.stat().st_mtime_ns > built_time:
            raise ValueError(f'built os.bin is stale relative to {path}; rebuild firmware')
    return git(source, 'rev-parse', 'HEAD').decode().strip(), built


def write_marker(source, runtime):
    commit, built = source_evidence(source)
    if digest(built) != digest(runtime / 'os.bin'):
        raise ValueError('exported os.bin differs from built os.bin; rebuild and export Pocket firmware')
    marker = dict(version=1, capabilities=['save-slot-fsync'],
                  required_revision=REQUIRED_REVISION, source_commit=commit,
                  sha256={name: digest(runtime / name) for name in FILES})
    (runtime / MARKER).write_text(json.dumps(marker, indent=2) + '\n')
    check_marker(runtime)


def check_marker(runtime):
    marker = json.loads((runtime / MARKER).read_text())
    if (type(marker.get('version')) is not int or marker.get('version') != 1 or marker.get('capabilities') != ['save-slot-fsync']
            or marker.get('required_revision') != REQUIRED_REVISION
            or not re.fullmatch(r'[0-9a-f]{40}', marker.get('source_commit', ''))):
        raise ValueError('unsupported save-slot-fsync marker; restage the SDK')
    hashes = marker.get('sha256', {})
    if set(hashes) != set(FILES):
        raise ValueError('runtime file hashes are missing; restage the SDK')
    for name in FILES:
        if hashes[name] != digest(runtime / name):
            raise ValueError(f'{name} differs from the staged runtime; restage the SDK')


if __name__ == '__main__':
    try:
        mode, *paths = sys.argv[1:]
        if mode == 'source' and len(paths) == 1:
            source_evidence(Path(paths[0]))
        elif mode == 'write' and len(paths) == 2:
            write_marker(*map(Path, paths))
        elif mode == 'check' and len(paths) == 1:
            check_marker(Path(paths[0]))
        else:
            raise ValueError('usage: runtime-provenance.py source SOURCE | write SOURCE RUNTIME | check RUNTIME')
    except (OSError, ValueError, TypeError, AttributeError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'runtime provenance: {error}')
