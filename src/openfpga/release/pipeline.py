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

RELEASE_ROOT = Path(__file__).resolve().parent
POCKET_ROOT = RELEASE_ROOT.parent
REPO_ROOT = POCKET_ROOT.parent.parent
TOOLS_ROOT = POCKET_ROOT / "tools/pocket-data"
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
    return json.loads((RELEASE_ROOT / "core.lock.json").read_text())


def verify_pins(lock=None):
    """Return the lock after validating immutable revisions, hashes and provenance.

    Raise ValueError for inconsistent pins or bundled runtime contents.
    """
    lock = load_lock() if lock is None else lock
    if lock.get("schema") != 2 or lock["core"] != {
            "repository": "zenibako/butterscotch-pocket", "source": "checkout"}:
        raise ValueError("core must use this repository checkout (lock schema 2)")
    for group in ("runtime",):
        if not re.fullmatch(r"[0-9a-f]{40}", lock[group]["commit"]):
            raise ValueError(f"{group} must pin a full immutable commit")
    if not re.fullmatch(r"[0-9a-f]{40}", lock["sdk"]["base_commit"]):
        raise ValueError("SDK base must pin a full immutable commit")
    required = {"release/sdk.tar.gz", "release/runtime/SAVESTATES_RUNTIME.json"}
    required.update("release/runtime/" + n for n in ("os.bin", "os25.rbf_r", "loader.bin"))
    if set(lock["sha256"]) != required:
        raise ValueError("lock must cover the complete SDK/runtime set")
    for path, expected in lock["sha256"].items():
        if sha(POCKET_ROOT / path) != expected:
            raise ValueError(f"pinned file hash mismatch: {path}")
    runtime = json.loads((RELEASE_ROOT / "runtime/SAVESTATES_RUNTIME.json").read_text())
    if runtime["source_commit"] != lock["runtime"]["commit"]:
        raise ValueError("runtime source provenance mismatch")
    for name, expected in runtime["sha256"].items():
        if sha(RELEASE_ROOT / "runtime" / name) != expected:
            raise ValueError(f"runtime provenance mismatch: {name}")
    return lock


def verify_source(source):
    """Return dependency pins plus the actual clean, shared checkout identity."""
    source = Path(source).resolve()
    if source != REPO_ROOT.resolve():
        raise ValueError("core and tools must use the same repository checkout")
    if Path(git(source, "rev-parse", "--show-toplevel")).resolve() != source:
        raise ValueError("source must be the repository root")
    if git(source, "status", "--porcelain", "--untracked-files=all"):
        raise ValueError("release source must be a clean repository checkout")
    lock = verify_pins()
    return dict(lock, source={"repository": lock["core"]["repository"],
                             "commit": git(source, "rev-parse", "HEAD")})


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
        with tarfile.open(POCKET_ROOT / lock["sdk"]["archive"]) as archive:
            archive.extractall(scratch / "sdk", filter="data")
        for game in ("undertale", "deltarune"):
            # Public and local builds use the same runtime game identity.
            run("make", "-j2", "-C", str(source / "src/openfpga"),
                "USE_SDK_CONTAINER=0", "OF_SDK_IN_CONTAINER=1", "SAVESTATES=1",
                f"SDK_ROOT={scratch / 'sdk'}", f"ROOT={scratch / 'objects'}",
                f"GAME={game}", f"G={scratch / 'no-game-data'}/", "elf")
            elf = scratch / "objects/.obj" / game / "app.elf"
            check_elf(elf)
            symbols = subprocess.check_output(["riscv64-unknown-elf-nm", "-S", str(elf)], text=True)
            loops = re.findall(r"^([0-9a-fA-F]+)\s+\S+\s+\S\s+executeBytecodeInstructions(?:\..*)?$", symbols, re.M)
            if not loops or any(int(address, 16) >= 0x10000000 for address in loops):
                raise ValueError(f"{game}: interpreter loop is missing from block RAM")
            shutil.copyfile(elf, programs / f"{game}.elf")
    if verify_source(source)["source"] != lock["source"]:
        raise ValueError("source changed during core compilation")
    proof = dict(lock["source"], files={f"{game}.elf": sha(programs / f"{game}.elf")
                                         for game in ("undertale", "deltarune")})
    (programs / "PROGRAM-PROVENANCE.json").write_text(json.dumps(proof, indent=2) + "\n")
    return programs


def release_core_json(contents, version):
    """Apply release metadata while preserving the checkout's core identifiers."""
    core = json.loads(contents)
    if core["core"]["cores"][0]["filename"] != "os25.rbf_r":
        raise ValueError("core definition does not name the pinned runtime")
    core["core"]["metadata"]["version"] = version.removeprefix("v")
    epoch = int(git(REPO_ROOT, "log", "-1", "--format=%ct"))
    core["core"]["metadata"]["date_release"] = datetime.fromtimestamp(epoch, timezone.utc).strftime("%Y-%m-%d")
    core["core"]["metadata"]["url"] = "https://github.com/zenibako/butterscotch-pocket"
    core["core"]["metadata"]["description"] = "Undertale v1.08; Deltarune ch.1 (experimental, own data)"
    core["core"]["framework"]["sleep_supported"] = True
    # The pinned os25 variant has no link peripheral; this runner does not
    # support cartridges. Do not inherit the SDK's generic hardware defaults.
    core["core"]["framework"].setdefault("hardware", {}).update(
        link_port=False, cartridge_adapter=-1)
    return (json.dumps(core, indent=4) + "\n").encode()


def package_core(source, programs, output, version):
    """Return an allowlisted core ZIP and write per-file provenance beside it.

    Validate pins, version, ELFs and runtime metadata before publishing the ZIP;
    fixed entry timestamps and ordering make identical inputs reproducible.
    """
    verified = verify_source(source)
    identity = verified.pop("source")
    lock = verified
    program_provenance = json.loads((programs / "PROGRAM-PROVENANCE.json").read_text())
    program_files = program_provenance.pop("files", {})
    if program_provenance != identity:
        raise ValueError("program source does not match this repository checkout")
    if set(program_files) != {"undertale.elf", "deltarune.elf"}:
        raise ValueError("program artifact provenance must cover both games")
    for name, digest in program_files.items():
        if sha(programs / name) != digest:
            raise ValueError(f"program artifact hash mismatch: {name}")
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
        files[f"Cores/{CORE_ID}/{name}"] = (RELEASE_ROOT / "runtime" / name).read_bytes()
    files["Assets/butterscotch/common/os.bin"] = (RELEASE_ROOT / "runtime/os.bin").read_bytes()
    files[CORE_JSON] = release_core_json(files[CORE_JSON], version)
    files[f"Cores/{CORE_ID}/info.txt"] = (RELEASE_ROOT / "core-info.txt").read_bytes()
    files[f"Cores/{CORE_ID}/updaters.json"] = (RELEASE_ROOT / "updaters.json").read_bytes()
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
    provenance = {"version": version, "pins": lock, "source": identity,
                  "archive_sha256": sha(archive_path),
                  "files": {name: hashlib.sha256(data).hexdigest() for name, data in sorted(files.items())},
                  "commercial_assets_included": False,
                  "memories_identity": "runtime game metadata and loaded bytecode; no private build fingerprint"}
    (output / "CORE-PROVENANCE.json").write_text(json.dumps(provenance, indent=2) + "\n")
    return archive_path


def source_archive(source, output, filename, paths=()):
    """Archive tracked HEAD content, optionally restricted to selected paths."""
    run("git", "-C", str(source), "archive", "--format=tar.gz", "-o", str(output / filename), "HEAD", *paths)


def tracked_paths(paths):
    """Select requested archive paths present in HEAD, ignoring local-only files."""
    relative = (path.relative_to(REPO_ROOT).as_posix() for path in paths)
    return tuple(path for path in relative if git(REPO_ROOT, "ls-tree", "HEAD", "--", path))


def tracked_contents(paths):
    """Return exact HEAD blobs below the requested paths, never local log files."""
    relative = [path.relative_to(REPO_ROOT).as_posix() for path in paths]
    tree = subprocess.check_output(["git", "-C", str(REPO_ROOT), "ls-tree", "-rz",
                                    "--full-tree", "HEAD", "--", *relative])
    contents = {}
    for entry in tree.split(b"\0"):
        if not entry:
            continue
        meta, name = entry.split(b"\t", 1)
        mode, kind, object_id = meta.split()
        if mode not in (b"100644", b"100755") or kind != b"blob":
            raise ValueError("release notices must be regular tracked files")
        contents[os.fsdecode(name)] = subprocess.check_output(
            ["git", "-C", str(REPO_ROOT), "cat-file", "blob", object_id.decode("ascii")])
    return contents


def tools_source_archive(output):
    """Archive the complete tracked module, Pocket build inputs and notices."""
    paths = (TOOLS_ROOT, RELEASE_ROOT, POCKET_ROOT / "docs", POCKET_ROOT / "GAME-DATA.md",
             REPO_ROOT / "LICENSE", REPO_ROOT / "LICENSES", REPO_ROOT / "NOTICE",
             REPO_ROOT / "REUSE.toml", REPO_ROOT / ".agents/skills/pocket-release",
             REPO_ROOT / ".claude/skills/pocket-release", REPO_ROOT / ".github/workflows/release-pocket.yml",
             REPO_ROOT / ".github/workflows/publish-pocket.yml")
    source_archive(REPO_ROOT, output, "pocket-tools-source.tar.gz", tracked_paths(paths))


def build_tools(output):
    """Build six native bundles, WASI and plugin assets with licenses and source."""
    identity = verify_source(REPO_ROOT)["source"]
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, CGO_ENABLED="0")
    license_paths = (POCKET_ROOT / "GAME-DATA.md", TOOLS_ROOT / "LICENSE-GO-TOOLS",
                     TOOLS_ROOT / "third_party/licenses", TOOLS_ROOT / "third_party/oggvorbis/LICENSE",
                     TOOLS_ROOT / "third_party/vorbis/LICENSE")
    head_licenses = tracked_contents(license_paths)
    for system, arch in TARGETS:
        with tempfile.TemporaryDirectory(prefix="tool-bundle-", dir=output) as scratch:
            bundle = Path(scratch)
            binary = "prepare-data.exe" if system == "windows" else "prepare-data"
            run("go", "build", "-trimpath", "-o", str(bundle / binary), "./cmd/prepare-data",
                cwd=TOOLS_ROOT, env=dict(env, GOOS=system, GOARCH=arch))
            licenses_prefix = (TOOLS_ROOT / "third_party/licenses").relative_to(REPO_ROOT)
            for path, content in head_licenses.items():
                relative = Path(path)
                if relative.is_relative_to(licenses_prefix):
                    destination = bundle / "licenses" / relative.relative_to(licenses_prefix)
                elif relative.name == "LICENSE":
                    destination = bundle / "licenses" / (relative.parent.name + ".txt")
                else:
                    destination = bundle / relative.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(content)
            with tarfile.open(output / f"prepare-data-{system}-{arch}.tar.gz", "w:gz") as archive:
                for path in sorted(bundle.iterdir()):
                    archive.add(path, arcname=path.name)
    run("go", "build", "-trimpath", "-o", str(output / "prepare-data.wasm"), "./cmd/prepare-data",
        cwd=TOOLS_ROOT, env=dict(env, GOOS="wasip1", GOARCH="wasm"))
    run(os.environ.get("TINYGO", "tinygo"), "build", "-p", "2", "-trimpath", "-target", "wasip1",
        "-buildmode=c-shared", "-o", str(output / "plugin.wasm"), "./cmd/pocket-plugin", cwd=TOOLS_ROOT)
    shutil.copyfile(TOOLS_ROOT / "plugin.json", output / "plugin.json")
    shutil.copyfile(TOOLS_ROOT / "plugin_icon.png", output / "plugin_icon.png")
    tools_source_archive(output)
    if verify_source(REPO_ROOT)["source"] != identity:
        raise ValueError("source changed during tools compilation")
    names = {f"prepare-data-{system}-{arch}.tar.gz" for system, arch in TARGETS}
    names.update(("prepare-data.wasm", "plugin.wasm", "plugin.json", "plugin_icon.png", "pocket-tools-source.tar.gz"))
    (output / "TOOLS-PROVENANCE.json").write_text(json.dumps({
        "source": identity, "files": {name: sha(output / name) for name in sorted(names)}}, indent=2) + "\n")


def verify_core_archive(source, output, lock, provenance):
    """Bind the ZIP's complete manifest and bytes to current source and dependencies."""
    pins = {key: value for key, value in lock.items() if key != "source"}
    if provenance["pins"] != pins:
        raise ValueError("core dependency pins do not match the current lock")
    path = output / "butterscotch-pocket.zip"
    if sha(path) != provenance["archive_sha256"]:
        raise ValueError("core artifact hash mismatch")
    version = provenance["version"]
    if not re.fullmatch(r"v?\d+\.\d+\.\d+(?:-[a-zA-Z0-9.-]+)?", version):
        raise ValueError("core contents need a semantic release version")
    known = {name: (source / "src/openfpga/dist" / name).read_bytes() for name in DIST_FILES}
    known[CORE_JSON] = release_core_json(known[CORE_JSON], version)
    known[f"Cores/{CORE_ID}/info.txt"] = (RELEASE_ROOT / "core-info.txt").read_bytes()
    known[f"Cores/{CORE_ID}/updaters.json"] = (RELEASE_ROOT / "updaters.json").read_bytes()
    for filename in ("loader.bin", "os25.rbf_r"):
        known[f"Cores/{CORE_ID}/{filename}"] = (RELEASE_ROOT / "runtime" / filename).read_bytes()
    known["Assets/butterscotch/common/os.bin"] = (RELEASE_ROOT / "runtime/os.bin").read_bytes()
    expected = set(known) | {f"Assets/butterscotch/common/{game}.elf" for game in ("undertale", "deltarune")}
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)) or set(names) != expected or set(provenance["files"]) != expected:
            raise ValueError("core ZIP manifest does not match the complete public allowlist")
        for name in names:
            contents = archive.read(name)
            if hashlib.sha256(contents).hexdigest() != provenance["files"][name]:
                raise ValueError(f"core file hash mismatch: {name}")
            if name in known and contents != known[name]:
                raise ValueError(f"core contents do not match current source/runtime/version: {name}")


def seal_release(source, runtime_source, output):
    """Archive clean pinned sources and write checksums for the complete release.

    Reject missing or extra assets and a plugin without the WASM magic header.
    """
    lock = verify_source(source)
    for filename in ("CORE-PROVENANCE.json", "TOOLS-PROVENANCE.json"):
        provenance = json.loads((output / filename).read_text())
        if provenance["source"] != lock["source"]:
            raise ValueError(f"release source mismatch: {filename}")
    core = json.loads((output / "CORE-PROVENANCE.json").read_text())
    verify_core_archive(source, output, lock, core)
    tools = json.loads((output / "TOOLS-PROVENANCE.json").read_text())
    expected_tools = {f"prepare-data-{system}-{arch}.tar.gz" for system, arch in TARGETS}
    expected_tools.update(("prepare-data.wasm", "plugin.wasm", "plugin.json", "plugin_icon.png", "pocket-tools-source.tar.gz"))
    if set(tools["files"]) != expected_tools:
        raise ValueError("tool provenance must cover the complete tool artifact set")
    for name, digest in tools["files"].items():
        if Path(name).name != name or sha(output / name) != digest:
            raise ValueError(f"tool artifact hash mismatch: {name}")
    if Path(git(runtime_source, "rev-parse", "--show-toplevel")).resolve() != Path(runtime_source).resolve():
        raise ValueError("runtime source must be the repository root")
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
    shutil.copyfile(RELEASE_ROOT / "core.lock.json", output / "core.lock.json")
    shutil.copyfile(POCKET_ROOT / "GAME-DATA.md", output / "GAME-DATA.md")
    license_paths = (REPO_ROOT / "LICENSE", REPO_ROOT / "LICENSES", REPO_ROOT / "NOTICE",
                     RELEASE_ROOT / "NOTICE", RELEASE_ROOT / "licenses",
                     TOOLS_ROOT / "LICENSE-GO-TOOLS", TOOLS_ROOT / "third_party")
    source_archive(REPO_ROOT, output, "licenses.tar.gz", tracked_paths(license_paths))
    expected = {"butterscotch-pocket.zip", "CORE-PROVENANCE.json", "TOOLS-PROVENANCE.json", "plugin.wasm", "plugin.json", "plugin_icon.png",
                "prepare-data.wasm", "butterscotch-core-source.tar.gz", "pocket-tools-source.tar.gz",
                "core.lock.json", "GAME-DATA.md", "licenses.tar.gz", "openfpga-runtime-source.tar.gz", "RUNTIME-SUBMODULES.json"}
    expected.update(f"prepare-data-{system}-{arch}.tar.gz" for system, arch in TARGETS)
    entries = list(output.iterdir())
    present = {p.name for p in entries}
    if any(p.is_symlink() or not p.is_file() for p in entries):
        raise ValueError("release asset set must contain only regular public files")
    if present - {"SHA256SUMS"} != expected:
        raise ValueError(f"release asset set differs: missing={expected-present}, extra={present-expected-{'SHA256SUMS'}}")
    if (output / "plugin.wasm").read_bytes()[:4] != b"\x00asm":
        raise ValueError("plugin is not WASM")
    (output / "SHA256SUMS").write_text("".join(f"{sha(output/name)}  {name}\n" for name in sorted(expected)))


def main():
    """Parse release CLI arguments and dispatch the selected validation or build step."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("validate", "build-core", "package-core", "build-tools", "assemble-release"))
    parser.add_argument("--source", type=Path, default=REPO_ROOT)
    parser.add_argument("--runtime-source", type=Path, default=POCKET_ROOT / "out/runtime-source")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--programs", type=Path, default=POCKET_ROOT / "out/build/programs")
    parser.add_argument("--version", default="0.0.0-dev")
    args = parser.parse_args()
    default_output = POCKET_ROOT / ("out/build" if args.command == "build-core" else "out/artifacts")
    source, output = args.source.resolve(), (args.out or default_output).resolve()
    if args.command == "validate":
        verify_pins()
    elif args.command == "build-core":
        build_core(source, output)
    elif args.command == "package-core":
        package_core(source, args.programs.resolve(), output, args.version)
    elif args.command == "build-tools":
        build_tools(output)
    elif args.command == "assemble-release":
        seal_release(source, args.runtime_source.resolve(), output)


if __name__ == "__main__":
    main()
