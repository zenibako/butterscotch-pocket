#!/usr/bin/env python3
"""Release orchestration only; installed converters do not require Python."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
CORE_ID = "zenibako.Butterscotch"
CORE_JSON = f"Cores/{CORE_ID}/core.json"
DIST_FILES = [
    f"Cores/{CORE_ID}/{name}" for name in
    ("audio.json", "core.json", "data.json", "icon.bin", "info.txt", "input.json",
     "interact.json", "variants.json", "video.json")
] + [
    "Platforms/butterscotch.json", "Platforms/_images/butterscotch.bin",
    "Assets/butterscotch/common/undertale_os.ini",
    "Assets/butterscotch/common/deltarune_os.ini",
    f"Assets/butterscotch/{CORE_ID}/Undertale.json",
    f"Assets/butterscotch/{CORE_ID}/Deltarune.json",
]
TARGETS = [(system, arch) for system in ("darwin", "linux", "windows")
           for arch in ("amd64", "arm64")]


def run(*args, **kwargs):
    """Run a checked subprocess, forwarding options and propagating failures."""
    return subprocess.run(args, check=True, **kwargs)


def git(path, *args):
    """Return stripped Git output from the given checkout; propagate Git failures."""
    return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()


def sha(path):
    """Return the SHA-256 hex digest of the complete file contents."""
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load_lock():
    """Read the checked-in release dependency lock as a JSON object."""
    return json.loads((ROOT / "release/core.lock.json").read_text())


def verify_pins(lock=None):
    """Return the lock after validating immutable revisions, hashes and provenance.

    Raise ValueError for inconsistent pins or bundled runtime contents.
    """
    lock = load_lock() if lock is None else lock
    for group in ("core", "runtime"):
        if not re.fullmatch(r"[0-9a-f]{40}", lock[group]["commit"]):
            raise ValueError(f"{group} must pin a full immutable commit")
    if not re.fullmatch(r"[0-9a-f]{40}", lock["sdk"]["base_commit"]):
        raise ValueError("SDK base must pin a full immutable commit")
    required = {"release/sdk.tar.gz", "release/runtime/SAVESTATES_RUNTIME.json"}
    required.update("release/runtime/" + n for n in ("os.bin", "os25.rbf_r", "loader.bin"))
    if set(lock["sha256"]) != required:
        raise ValueError("lock must cover the complete SDK/runtime set")
    for path, expected in lock["sha256"].items():
        if sha(ROOT / path) != expected:
            raise ValueError(f"pinned file hash mismatch: {path}")
    runtime = json.loads((ROOT / "release/runtime/SAVESTATES_RUNTIME.json").read_text())
    if runtime["source_commit"] != lock["runtime"]["commit"]:
        raise ValueError("runtime source provenance mismatch")
    for name, expected in runtime["sha256"].items():
        if sha(ROOT / "release/runtime" / name) != expected:
            raise ValueError(f"runtime provenance mismatch: {name}")
    return lock


def verify_source(source):
    """Return verified pins for a clean core checkout at the locked revision.

    Raise ValueError when the checkout revision or working tree differs.
    """
    lock = verify_pins()
    if git(source, "rev-parse", "HEAD") != lock["core"]["commit"]:
        raise ValueError("core checkout does not match release/core.lock.json")
    if git(source, "status", "--porcelain", "--untracked-files=all"):
        raise ValueError("core source must be a clean, dedicated checkout")
    return lock


def check_elf(path):
    """Reject files lacking a little-endian 32-bit RISC-V ELF header."""
    data = path.read_bytes()
    if len(data) < 52 or data[:6] != b"\x7fELF\x01\x01" or data[18:20] != b"\xf3\x00":
        raise ValueError(f"expected little-endian 32-bit RISC-V ELF: {path}")


def build_core(source, output):
    """Build both public game ELFs from verified source with the pinned SDK.

    Return the programs directory; reject compiler or block-RAM placement mismatches.
    """
    lock = verify_source(source)
    compiler = subprocess.check_output(["riscv64-unknown-elf-gcc", "-dumpfullversion"], text=True).strip()
    if compiler != "13.2.0":
        raise ValueError(f"expected GCC 13.2.0, got {compiler}")
    output.mkdir(parents=True, exist_ok=True)
    programs = output / "programs"
    programs.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="core-build-", dir=output) as scratch:
        scratch = Path(scratch)
        with tarfile.open(ROOT / lock["sdk"]["archive"]) as archive:
            archive.extractall(scratch / "sdk", filter="data")
        for game in ("undertale", "deltarune"):
            # The public build has no compile-time private game fingerprint.
            run("make", "-j2", "-C", str(source / "src/openfpga"),
                "USE_SDK_CONTAINER=0", "OF_SDK_IN_CONTAINER=1", "SAVESTATES=1",
                f"SDK_ROOT={scratch / 'sdk'}", f"ROOT={scratch / 'objects'}",
                f"GAME={game}", f"G={scratch / 'no-game-data'}/", "STATE_GAME_ID=", "elf")
            elf = scratch / "objects/.obj" / game / "app.elf"
            check_elf(elf)
            symbols = subprocess.check_output(["riscv64-unknown-elf-nm", "-S", str(elf)], text=True)
            loops = re.findall(r"^([0-9a-fA-F]+)\s+\S+\s+\S\s+executeLoop(?:\..*)?$", symbols, re.M)
            if not loops or any(int(address, 16) >= 0x10000000 for address in loops):
                raise ValueError(f"{game}: interpreter loop is missing from block RAM")
            shutil.copyfile(elf, programs / f"{game}.elf")
    return programs


def package_core(source, programs, output, version):
    """Return an allowlisted core ZIP and write per-file provenance beside it.

    Validate pins, version, ELFs and runtime metadata before publishing the ZIP;
    fixed entry timestamps and ordering make identical inputs reproducible.
    """
    lock = verify_pins()
    if not re.fullmatch(r"v?\d+\.\d+\.\d+(?:-[a-zA-Z0-9.-]+)?", version):
        raise ValueError("version must be a semantic release version")
    files = {}
    for name in DIST_FILES:
        path = source / "src/openfpga/dist" / name
        if path.is_symlink():
            raise ValueError(f"dist symlink is forbidden: {path}")
        files[name] = path.read_bytes()
    for game in ("undertale", "deltarune"):
        elf = programs / f"{game}.elf"
        check_elf(elf)
        files[f"Assets/butterscotch/common/{game}.elf"] = elf.read_bytes()
    for name in ("os25.rbf_r", "loader.bin"):
        files[f"Cores/{CORE_ID}/{name}"] = (ROOT / "release/runtime" / name).read_bytes()
    files["Assets/butterscotch/common/os.bin"] = (ROOT / "release/runtime/os.bin").read_bytes()
    core = json.loads(files[CORE_JSON])
    if core["core"]["cores"][0]["filename"] != "os25.rbf_r":
        raise ValueError("core definition does not name the pinned runtime")
    core["core"]["metadata"]["version"] = version.removeprefix("v")
    epoch = int(git(ROOT, "log", "-1", "--format=%ct"))
    core["core"]["metadata"]["date_release"] = datetime.fromtimestamp(epoch, timezone.utc).strftime("%Y-%m-%d")
    core["core"]["metadata"]["url"] = "https://github.com/zenibako/butterscotch-pocket"
    core["core"]["metadata"]["description"] = "Undertale v1.08; Deltarune ch.1 (experimental, own data)"
    core["core"]["framework"]["sleep_supported"] = True
    # The pinned os25 variant has no link peripheral; this runner does not
    # support cartridges. Do not inherit the SDK's generic hardware defaults.
    core["core"]["framework"].setdefault("hardware", {}).update(
        link_port=False, cartridge_adapter=-1)
    files[CORE_JSON] = (json.dumps(core, indent=4) + "\n").encode()
    files[f"Cores/{CORE_ID}/info.txt"] = (ROOT / "release/core-info.txt").read_bytes()
    files[f"Cores/{CORE_ID}/updaters.json"] = (ROOT / "release/updaters.json").read_bytes()
    # A fixed timestamp and sorted allowlist make accidental assets impossible
    # and repeated assembly from the same inputs byte-identical.
    output.mkdir(parents=True, exist_ok=True)
    archive_path = output / "butterscotch-pocket.zip"
    with tempfile.NamedTemporaryFile(dir=output, suffix=".zip") as temporary:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for name, content in sorted(files.items()):
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                archive.writestr(info, content)
        shutil.copyfile(temporary.name, archive_path)
    provenance = {"version": version, "pins": lock,
                  "files": {name: hashlib.sha256(data).hexdigest() for name, data in sorted(files.items())},
                  "commercial_assets_included": False,
                  "memories_identity": "runtime game metadata and loaded bytecode; no private build fingerprint"}
    (output / "CORE-PROVENANCE.json").write_text(json.dumps(provenance, indent=2) + "\n")
    return archive_path


def source_archive(source, output, filename, paths=()):
    """Archive tracked HEAD content, optionally restricted to selected paths."""
    run("git", "-C", str(source), "archive", "--format=tar.gz", "-o", str(output / filename), "HEAD", *paths)


def build_tools(output):
    """Build six native bundles, WASI and plugin assets with licenses and source."""
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, CGO_ENABLED="0")
    for system, arch in TARGETS:
        with tempfile.TemporaryDirectory(prefix="tool-bundle-", dir=output) as scratch:
            bundle = Path(scratch)
            binary = "prepare-data.exe" if system == "windows" else "prepare-data"
            run("go", "build", "-trimpath", "-o", str(bundle / binary), "./cmd/prepare-data",
                cwd=ROOT, env=dict(env, GOOS=system, GOARCH=arch))
            for name in ("GAME-DATA.md", "LICENSE-GO-TOOLS"):
                shutil.copyfile(ROOT / name, bundle / name)
            shutil.copytree(ROOT / "third_party/licenses", bundle / "licenses")
            for dep in ("oggvorbis", "vorbis"):
                shutil.copyfile(ROOT / f"third_party/{dep}/LICENSE", bundle / f"licenses/{dep}.txt")
            with tarfile.open(output / f"prepare-data-{system}-{arch}.tar.gz", "w:gz") as archive:
                for path in sorted(bundle.iterdir()):
                    archive.add(path, arcname=path.name)
    run("go", "build", "-trimpath", "-o", str(output / "prepare-data.wasm"), "./cmd/prepare-data",
        cwd=ROOT, env=dict(env, GOOS="wasip1", GOARCH="wasm"))
    run(os.environ.get("TINYGO", "tinygo"), "build", "-p", "2", "-trimpath", "-target", "wasip1",
        "-buildmode=c-shared", "-o", str(output / "plugin.wasm"), "./cmd/pocket-plugin", cwd=ROOT)
    shutil.copyfile(ROOT / "plugin.json", output / "plugin.json")
    shutil.copyfile(ROOT / "plugin_icon.png", output / "plugin_icon.png")
    source_archive(ROOT, output, "pocket-tools-source.tar.gz", (
        ".github", ".agents", ".claude/skills/pocket-release", ".gitignore", ".gitattributes", "AGENTS.md", "CLAUDE.md", "README.md", "GAME-DATA.md",
        "LICENSE", "LICENSE-GO-TOOLS", "NOTICE", "LICENSES", "REUSE.toml", "cmd", "prep",
        "plugin", "plugin.json", "plugin_icon.png", "internal", "integration", "third_party", "go.mod", "go.sum",
        "release", "docs"))


def seal_release(source, runtime_source, output):
    """Archive clean pinned sources and write checksums for the complete release.

    Reject missing or extra assets and a plugin without the WASM magic header.
    """
    lock = verify_source(source)
    if git(runtime_source, "rev-parse", "HEAD") != lock["runtime"]["commit"]:
        raise ValueError("runtime source checkout does not match the lock")
    if git(runtime_source, "status", "--porcelain", "--untracked-files=all"):
        raise ValueError("runtime source checkout must be clean")
    source_archive(runtime_source, output, "openfpga-runtime-source.tar.gz")
    gitlinks = []
    for line in git(runtime_source, "ls-tree", "-r", "HEAD").splitlines():
        meta, name = line.split("\t", 1)
        mode, kind, commit = meta.split()
        if mode == "160000":
            gitlinks.append({"path": name, "commit": commit})
    (output / "RUNTIME-SUBMODULES.json").write_text(json.dumps({
        "repository": lock["runtime"]["repository"], "commit": lock["runtime"]["commit"],
        "gitlinks": gitlinks,
        "retrieval": "Clone the pinned runtime repository and run git submodule update --init --recursive; see docs/BUILD.md. Archives omit submodule working trees."
    }, indent=2) + "\n")
    source_archive(source, output, "butterscotch-core-source.tar.gz")
    shutil.copyfile(ROOT / "release/core.lock.json", output / "core.lock.json")
    shutil.copyfile(ROOT / "GAME-DATA.md", output / "GAME-DATA.md")
    with tarfile.open(output / "licenses.tar.gz", "w:gz") as archive:
        for path in (ROOT / "LICENSE", ROOT / "LICENSE-GO-TOOLS", ROOT / "NOTICE", ROOT / "LICENSES", ROOT / "release/licenses", ROOT / "third_party"):
            archive.add(path, arcname=str(path.relative_to(ROOT)))
    expected = {"butterscotch-pocket.zip", "CORE-PROVENANCE.json", "plugin.wasm", "plugin.json", "plugin_icon.png",
                "prepare-data.wasm", "butterscotch-core-source.tar.gz", "pocket-tools-source.tar.gz",
                "core.lock.json", "GAME-DATA.md", "licenses.tar.gz", "openfpga-runtime-source.tar.gz", "RUNTIME-SUBMODULES.json"}
    expected.update(f"prepare-data-{system}-{arch}.tar.gz" for system, arch in TARGETS)
    present = {p.name for p in output.iterdir() if p.is_file()}
    if present - {"SHA256SUMS"} != expected:
        raise ValueError(f"release asset set differs: missing={expected-present}, extra={present-expected-{'SHA256SUMS'}}")
    if (output / "plugin.wasm").read_bytes()[:4] != b"\x00asm":
        raise ValueError("plugin is not WASM")
    (output / "SHA256SUMS").write_text("".join(f"{sha(output/name)}  {name}\n" for name in sorted(expected)))


def main():
    """Parse release CLI arguments and dispatch the selected validation or build step."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("validate", "build-core", "package-core", "build-tools", "assemble-release"))
    parser.add_argument("--source", type=Path, default=ROOT / "out/core")
    parser.add_argument("--runtime-source", type=Path, default=ROOT / "out/runtime-source")
    parser.add_argument("--out", type=Path, default=ROOT / "out/artifacts")
    parser.add_argument("--programs", type=Path, default=ROOT / "out/programs")
    parser.add_argument("--version", default="0.0.0-dev")
    args = parser.parse_args()
    source, output = args.source.resolve(), args.out.resolve()
    if args.command == "validate":
        verify_pins()
    elif args.command == "build-core":
        build_core(source, output)
    elif args.command == "package-core":
        # Build outputs may be inside a dedicated source checkout, but source
        # identity was checked before compilation. CI keeps them outside it.
        verify_source(source)
        package_core(source, args.programs.resolve(), output, args.version)
    elif args.command == "build-tools":
        build_tools(output)
    elif args.command == "assemble-release":
        seal_release(source, args.runtime_source.resolve(), output)


if __name__ == "__main__":
    main()
