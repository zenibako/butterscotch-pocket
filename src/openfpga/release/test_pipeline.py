import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
from unittest.mock import patch
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("pipeline", Path(__file__).with_name("pipeline.py"))
pipeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pipeline)

class ReleaseFixture(unittest.TestCase):
    """Exercise release packaging with synthetic public assets and ELF-header fixtures."""
    def setUp(self):
        """Create an allowlisted dist tree and synthetic ELF-header fixtures.

        The fixtures satisfy check_elf's limited checks, not full ELF validation.
        """
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / "core"
        self.dist = self.source / "src/openfpga/dist"
        for name in pipeline.DIST_FILES:
            path = self.dist / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("{}")
        core = self.dist / pipeline.CORE_JSON
        core.write_text(json.dumps({"core":{"metadata":{}, "framework":{},"cores":[{"filename":"os25.rbf_r","chip32_vm":"loader.bin"}]}}))
        self.programs = self.root / "programs"
        self.programs.mkdir()
        for game in ("undertale", "deltarune"):
            # Minimal RISC-V ELF identity; the real build also checks symbols.
            (self.programs / (game + ".elf")).write_bytes(b"\x7fELF\x01\x01" + bytes(12) + b"\xf3\x00" + bytes(100))
        self.output = self.root / "artifacts"
        self.pocket = self.source / "src/openfpga"
        shutil.copytree(Path(__file__).parent, self.pocket / "release",
                        ignore=shutil.ignore_patterns("__pycache__"))
        for name, value in (("REPO_ROOT", self.source), ("POCKET_ROOT", self.pocket),
                            ("RELEASE_ROOT", self.pocket / "release"),
                            ("TOOLS_ROOT", self.pocket / "tools/pocket-data")):
            override = patch.object(pipeline, name, value, create=True)
            override.start()
            self.addCleanup(override.stop)
        self.git("init", "-q")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "user.name", "Release Test")
        self.commit()

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.source), *args], text=True).strip()

    def commit(self):
        self.git("add", ".")
        self.git("-c", "commit.gpgsign=false", "commit", "-qm", "fixture", "--allow-empty")

    def package(self):
        self.commit()
        (self.programs / "PROGRAM-PROVENANCE.json").write_text(json.dumps({
            "repository": "zenibako/butterscotch-pocket", "commit": self.git("rev-parse", "HEAD"),
            "files": {name + ".elf": pipeline.sha(self.programs / (name + ".elf"))
                      for name in ("undertale", "deltarune")}}))
        return pipeline.package_core(self.source, self.programs, self.output, "v0.2.0")

class PackageTests(ReleaseFixture):
    def test_only_public_files_and_matching_runtime(self):
        """Ensure packaging excludes game data and saves while using the pinned runtime."""
        secret = self.dist / "Assets/butterscotch/common/data.win"
        secret.write_text("PRIVATE GAME")
        (self.dist / "Saves").mkdir()
        (self.dist / "Saves/secret.sav").write_text("PRIVATE SAVE")
        archive = self.package()
        with zipfile.ZipFile(archive) as z:
            self.assertNotIn("Assets/butterscotch/common/data.win", z.namelist())
            self.assertFalse(any(n.startswith("Saves/") for n in z.namelist()))
            self.assertFalse(any("Sleep Test" in n for n in z.namelist()))
            core = json.loads(z.read(pipeline.CORE_JSON))["core"]
            self.assertTrue(core["framework"]["sleep_supported"])
            self.assertEqual(core["metadata"]["version"], "0.2.0")
            self.assertIn("Assets/butterscotch/common/deltarune.elf", z.namelist())
            self.assertEqual(z.read("Cores/zenibako.Butterscotch/os25.rbf_r"), (self.pocket/"release/runtime/os25.rbf_r").read_bytes())

    def test_packaged_ai_declaration_matches_release_metadata(self):
        """Keep explicit AI disclosure in the installed core rather than only the README."""
        archive = self.package()
        with zipfile.ZipFile(archive) as z:
            declaration = json.loads(z.read(f"Cores/{pipeline.CORE_ID}/updaters.json"))
        self.assertEqual(declaration["declare_ai"], ["OpenAI Codex", "Claude Code"])

    def test_missing_program_fails_without_publishing(self):
        """Ensure a missing game ELF leaves no published core archive."""
        (self.programs/"deltarune.elf").unlink()
        with self.assertRaises((FileNotFoundError, ValueError)):
            self.package()
        self.assertFalse((self.output/"butterscotch-pocket.zip").exists())

    def test_packaged_hardware_matches_supported_game_core(self):
        """Check supported hardware overrides while preserving unrelated framework fields."""
        path = self.dist / pipeline.CORE_JSON
        definition = json.loads(path.read_text())
        definition["core"]["framework"] = {
            "target_product": "Analogue Pocket", "version_required": "2.2",
            "sleep_supported": False,
            "dock": {"supported": True, "analog_output": False},
            "hardware": {"link_port": True, "cartridge_adapter": 0},
        }
        path.write_text(json.dumps(definition))
        archive = self.package()
        with zipfile.ZipFile(archive) as z:
            core = json.loads(z.read(pipeline.CORE_JSON))["core"]
            framework = core["framework"]
        self.assertIn("v1.08", core["metadata"]["description"])
        self.assertIn("experimental", core["metadata"]["description"])
        self.assertLessEqual(len(core["metadata"]["description"]), 63)
        self.assertEqual(framework["hardware"]["cartridge_adapter"], -1)
        self.assertFalse(framework["hardware"]["link_port"])
        self.assertTrue(framework["sleep_supported"])
        self.assertEqual(framework["dock"], {"supported": True, "analog_output": False})
        self.assertEqual(framework["version_required"], "2.2")

    def test_info_fits_pocket_display_and_explains_sleep_limits(self):
        """Keep packaged help within Pocket display limits and state experimental support."""
        archive = self.package()
        with zipfile.ZipFile(archive) as z:
            text = z.read(f"Cores/{pipeline.CORE_ID}/info.txt").decode("ascii")
        self.assertLessEqual(len(text.splitlines()), 32)
        self.assertIn("Save-state status", text)
        self.assertIn("experimental", text)
        self.assertIn("No cartridge or link-cable support", text)
        self.assertIn("v1.08", text)

    def test_wrong_elf_architecture_is_rejected(self):
        """Reject a short non-ELF file before packaging."""
        (self.programs/"undertale.elf").write_bytes(b"not an ELF")
        with self.assertRaises(ValueError):
            self.package()

    def test_corrupt_runtime_is_rejected(self):
        """Reject a runtime file whose digest differs from the supplied lock."""
        lock = json.loads((self.pocket/"release/core.lock.json").read_text())
        lock["sha256"]["release/runtime/os.bin"] = "0"*64
        with self.assertRaises(ValueError):
            pipeline.verify_pins(lock)

    def test_mutable_runtime_commit_is_rejected(self):
        """External runtime inputs still require immutable commit pins."""
        lock = json.loads((self.pocket/"release/core.lock.json").read_text())
        lock["runtime"]["commit"] = "main"
        with self.assertRaises(ValueError):
            pipeline.verify_pins(lock)

class CheckoutTests(ReleaseFixture):
    def test_checkout_records_actual_head_without_self_pin(self):
        lock = pipeline.verify_source(self.source)
        self.assertEqual(lock["schema"], 2)
        self.assertEqual(lock["core"], {"repository": "zenibako/butterscotch-pocket", "source": "checkout"})
        self.assertEqual(lock["source"]["commit"], self.git("rev-parse", "HEAD"))

    def test_another_clean_checkout_cannot_supply_core_source(self):
        clone = self.root / "other"
        subprocess.run(["git", "clone", "-q", str(self.source), str(clone)], check=True)
        with self.assertRaisesRegex(ValueError, "same repository checkout"):
            pipeline.verify_source(clone)

    def test_dirty_checkout_is_rejected(self):
        (self.source / "untracked-private-data").write_text("private")
        with self.assertRaisesRegex(ValueError, "clean"):
            pipeline.verify_source(self.source)

    def test_ignored_build_output_preserves_clean_source(self):
        (self.source / ".gitignore").write_text("src/openfpga/out/\n")
        self.commit()
        (self.pocket / "out").mkdir()
        (self.pocket / "out/build.log").write_text("build output")
        self.assertEqual(pipeline.verify_source(self.source)["source"]["commit"], self.git("rev-parse", "HEAD"))

    def test_provenance_records_same_source_as_program_build(self):
        self.package()
        provenance = json.loads((self.output / "CORE-PROVENANCE.json").read_text())
        self.assertEqual(provenance["source"], {"repository": "zenibako/butterscotch-pocket", "commit": self.git("rev-parse", "HEAD")})
        self.assertNotIn("commit", provenance["pins"]["core"])

    def test_changed_program_cannot_reuse_build_provenance(self):
        self.package()
        elf = self.programs / "undertale.elf"
        elf.write_bytes(elf.read_bytes() + b"changed after compilation")
        with self.assertRaisesRegex(ValueError, "program artifact"):
            pipeline.package_core(self.source, self.programs, self.output, "v0.2.0")

    def test_programs_from_another_revision_cannot_be_packaged(self):
        (self.programs / "PROGRAM-PROVENANCE.json").write_text(json.dumps({
            "repository": "zenibako/butterscotch-pocket", "commit": "0" * 40}))
        with self.assertRaisesRegex(ValueError, "program source"):
            pipeline.package_core(self.source, self.programs, self.output, "v0.2.0")
        self.assertFalse((self.output / "butterscotch-pocket.zip").exists())

    def test_default_core_commands_share_build_directory(self):
        (self.source / ".gitignore").write_text("src/openfpga/out/\n")
        self.commit()
        compilers = self.root / "fake-toolchain"
        compilers.mkdir()
        driver = """#!/usr/bin/env python3
from pathlib import Path
import sys
name = Path(sys.argv[0]).name
if name == "riscv64-unknown-elf-gcc":
    print("13.2.0")
elif name == "riscv64-unknown-elf-nm":
    print("00000100 00000020 T executeBytecodeInstructions")
else:
    args = dict(arg.split("=", 1) for arg in sys.argv[1:] if "=" in arg)
    assert args["STATE_GAME_ID"] == ""
    assert "no-game-data" in args["G"]
    elf = Path(args["ROOT"]) / ".obj" / args["GAME"] / "app.elf"
    elf.parent.mkdir(parents=True, exist_ok=True)
    elf.write_bytes(bytes.fromhex("7f454c460101") + bytes(12) + bytes.fromhex("f300") + bytes(100))
"""
        for name in ("make", "riscv64-unknown-elf-gcc", "riscv64-unknown-elf-nm"):
            executable = compilers / name
            executable.write_text(driver)
            executable.chmod(0o755)
        env = dict(os.environ, PATH=str(compilers) + os.pathsep + os.environ["PATH"])
        command = [os.sys.executable, str(self.pocket / "release/pipeline.py")]
        subprocess.run(command + ["build-core"], env=env, check=True, capture_output=True, text=True)
        self.assertTrue((self.pocket / "out/build/programs/PROGRAM-PROVENANCE.json").is_file())
        subprocess.run(command + ["package-core", "--version", "v0.2.0"],
                       env=env, check=True, capture_output=True, text=True)
        self.assertTrue((self.pocket / "out/artifacts/butterscotch-pocket.zip").is_file())

    def test_archive_preserves_complete_nested_module_and_release_notices(self):
        module = self.pocket / "tools/pocket-data"
        module.mkdir(parents=True)
        (module / "go.mod").write_text("module example.invalid/tools\n")
        (module / "future-package").mkdir()
        (module / "future-package/source.go").write_text("package future\n")
        (self.source / "LICENSE").write_text("core license\n")
        self.commit()
        self.output.mkdir()
        pipeline.tools_source_archive(self.output)
        with tarfile.open(self.output / "pocket-tools-source.tar.gz") as archive:
            names = archive.getnames()
        self.assertIn("src/openfpga/tools/pocket-data/future-package/source.go", names)
        self.assertIn("src/openfpga/release/NOTICE", names)
        self.assertIn("src/openfpga/release/licenses/sdk-legacy/MIT.txt", names)
        self.assertIn("LICENSE", names)

class ToolBundleTests(ReleaseFixture):
    def test_native_bundles_use_only_tracked_head_license_files(self):
        module = self.pocket / "tools/pocket-data"
        (module / "third_party/licenses").mkdir(parents=True)
        (module / "third_party/licenses/decoder.txt").write_text("tracked decoder notice")
        for dep in ("vorbis", "oggvorbis"):
            (module / f"third_party/{dep}").mkdir()
            (module / f"third_party/{dep}/LICENSE").write_text(dep + " license")
        (module / "LICENSE-GO-TOOLS").write_text("tools license")
        (module / "plugin.json").write_text("{}")
        (module / "plugin_icon.png").write_bytes(b"public icon")
        (self.pocket / "GAME-DATA.md").write_text("public guide")
        (self.source / ".gitignore").write_text("*.log\n")
        self.commit()
        (module / "third_party/licenses/device.log").write_text("PRIVATE DEVICE LOG")
        compilers = self.root / "fake-tools"
        compilers.mkdir()
        driver = """#!/usr/bin/env python3
from pathlib import Path
import sys
Path(sys.argv[sys.argv.index("-o") + 1]).write_bytes(bytes.fromhex("0061736d") + b"synthetic tool")
"""
        for name in ("go", "tinygo"):
            executable = compilers / name
            executable.write_text(driver)
            executable.chmod(0o755)
        with patch.dict(os.environ, PATH=str(compilers) + os.pathsep + os.environ["PATH"], TINYGO=str(compilers / "tinygo")):
            pipeline.build_tools(self.output)
        for system in ("darwin", "linux", "windows"):
            for arch in ("amd64", "arm64"):
                with tarfile.open(self.output / f"prepare-data-{system}-{arch}.tar.gz") as archive:
                    names = archive.getnames()
                    self.assertIn("licenses/decoder.txt", names)
                    self.assertFalse(any("device.log" in name for name in names))
        with tarfile.open(self.output / "pocket-tools-source.tar.gz") as archive:
            self.assertFalse(any("device.log" in name for name in archive.getnames()))

class AssemblyTests(ReleaseFixture):
    def setUp(self):
        super().setUp()
        self.runtime = self.root / "runtime"
        self.runtime.mkdir()
        subprocess.run(["git", "init", "-q", str(self.runtime)], check=True)
        def runtime_git(*args):
            return subprocess.check_output(["git", "-C", str(self.runtime), *args], text=True).strip()
        runtime_git("config", "user.email", "test@example.invalid")
        runtime_git("config", "user.name", "Release Test")
        (self.runtime / "NOTICE").write_text("runtime notice")
        (self.runtime / "subtree").mkdir()
        (self.runtime / "subtree/source.c").write_text("public runtime source")
        runtime_git("add", ".")
        runtime_git("-c", "commit.gpgsign=false", "commit", "-qm", "fixture")
        commit = runtime_git("rev-parse", "HEAD")
        manifest = self.pocket / "release/runtime/SAVESTATES_RUNTIME.json"
        data = json.loads(manifest.read_text())
        data["source_commit"] = commit
        manifest.write_text(json.dumps(data))
        lock_path = self.pocket / "release/core.lock.json"
        lock = json.loads(lock_path.read_text())
        lock["runtime"]["commit"] = commit
        lock["sha256"]["release/runtime/SAVESTATES_RUNTIME.json"] = pipeline.sha(manifest)
        lock_path.write_text(json.dumps(lock))
        (self.pocket / "GAME-DATA.md").write_text("public game-data guide")
        (self.source / "LICENSE").write_text("runner license")
        module = self.pocket / "tools/pocket-data"
        (module / "third_party/licenses").mkdir(parents=True)
        (module / "LICENSE-GO-TOOLS").write_text("tools license")
        (module / "third_party/licenses/decoder.txt").write_text("decoder notice")
        self.package()
        names = {f"prepare-data-{system}-{arch}.tar.gz" for system, arch in pipeline.TARGETS}
        names.update(("prepare-data.wasm", "plugin.wasm", "plugin.json", "plugin_icon.png", "pocket-tools-source.tar.gz"))
        for name in names:
            (self.output / name).write_bytes(b"\x00asmpublic fixture")
        self.tool_names = names
        self.write_tools_provenance()

    def write_tools_provenance(self):
        (self.output / "TOOLS-PROVENANCE.json").write_text(json.dumps({
            "source": {"repository": "zenibako/butterscotch-pocket", "commit": self.git("rev-parse", "HEAD")},
            "files": {name: pipeline.sha(self.output / name) for name in self.tool_names}}))

    def test_complete_assembly_preserves_notices_and_checksums(self):
        pipeline.seal_release(self.source, self.runtime, self.output)
        with tarfile.open(self.output / "licenses.tar.gz") as archive:
            names = archive.getnames()
        self.assertIn("src/openfpga/release/NOTICE", names)
        self.assertIn("src/openfpga/release/licenses/sdk-legacy/REUSE.toml", names)
        self.assertIn("src/openfpga/tools/pocket-data/third_party/licenses/decoder.txt", names)
        self.assertIn("LICENSE", names)
        for line in (self.output / "SHA256SUMS").read_text().splitlines():
            digest, name = line.split("  ")
            self.assertEqual(digest, hashlib.sha256((self.output / name).read_bytes()).hexdigest())
        with tarfile.open(self.output / "butterscotch-core-source.tar.gz") as archive:
            self.assertIn("src/openfpga/dist/Platforms/butterscotch.json", archive.getnames())

    def update_core_provenance(self, mutate):
        path = self.output / "CORE-PROVENANCE.json"
        proof = json.loads(path.read_text())
        mutate(proof)
        path.write_text(json.dumps(proof))

    def rewrite_zip(self, mutate):
        path = self.output / "butterscotch-pocket.zip"
        with zipfile.ZipFile(path) as archive:
            contents = {name: archive.read(name) for name in archive.namelist()}
        mutate(contents)
        with zipfile.ZipFile(path, "w") as archive:
            for name, value in contents.items():
                archive.writestr(name, value)
        self.update_core_provenance(lambda proof: proof.update(
            archive_sha256=pipeline.sha(path),
            files={name: hashlib.sha256(value).hexdigest() for name, value in contents.items()}))

    def test_runtime_checkout_subdirectory_cannot_omit_root_notices(self):
        with self.assertRaisesRegex(ValueError, "runtime.*repository root"):
            pipeline.seal_release(self.source, self.runtime / "subtree", self.output)
        self.assertFalse((self.output / "SHA256SUMS").exists())

    def test_ignored_license_logs_never_enter_license_archive(self):
        (self.source / ".gitignore").write_text("*.log\n")
        self.package()
        self.write_tools_provenance()
        private = self.pocket / "tools/pocket-data/third_party/licenses/device.log"
        private.write_text("PRIVATE DEVICE LOG")
        self.assertEqual(self.git("status", "--porcelain", "--untracked-files=all"), "")
        pipeline.seal_release(self.source, self.runtime, self.output)
        with tarfile.open(self.output / "licenses.tar.gz") as archive:
            names = archive.getnames()
            self.assertIn("src/openfpga/tools/pocket-data/third_party/licenses/decoder.txt", names)
            self.assertFalse(any("device.log" in name for name in names))

    def test_forged_core_dependency_pins_are_rejected(self):
        self.update_core_provenance(lambda proof: proof["pins"]["runtime"].update(commit="0" * 40))
        with self.assertRaisesRegex(ValueError, "core.*pins"):
            pipeline.seal_release(self.source, self.runtime, self.output)

    def test_empty_core_file_proof_cannot_attest_complete_zip(self):
        self.update_core_provenance(lambda proof: proof.update(files={}))
        with self.assertRaisesRegex(ValueError, "core.*manifest"):
            pipeline.seal_release(self.source, self.runtime, self.output)

    def test_core_file_hashes_must_match_zip_bytes(self):
        self.update_core_provenance(lambda proof: proof["files"].update({pipeline.CORE_JSON: "0" * 64}))
        with self.assertRaisesRegex(ValueError, "core.*file hash"):
            pipeline.seal_release(self.source, self.runtime, self.output)

    def test_core_metadata_version_must_match_provenance(self):
        self.update_core_provenance(lambda proof: proof.update(version="v99.0.0"))
        with self.assertRaisesRegex(ValueError, "core.*contents"):
            pipeline.seal_release(self.source, self.runtime, self.output)

    def test_rehashed_runtime_substitution_is_rejected(self):
        self.rewrite_zip(lambda contents: contents.update({"Assets/butterscotch/common/os.bin": b"wrong runtime"}))
        with self.assertRaisesRegex(ValueError, "core.*contents"):
            pipeline.seal_release(self.source, self.runtime, self.output)

    def test_rehashed_extra_private_zip_entry_is_rejected(self):
        self.rewrite_zip(lambda contents: contents.update({"Assets/butterscotch/common/private-data.win": b"private"}))
        with self.assertRaisesRegex(ValueError, "core.*manifest"):
            pipeline.seal_release(self.source, self.runtime, self.output)

    def test_changed_core_archive_is_rejected(self):
        with (self.output / "butterscotch-pocket.zip").open("ab") as archive:
            archive.write(b"tampered")
        with self.assertRaisesRegex(ValueError, "core artifact"):
            pipeline.seal_release(self.source, self.runtime, self.output)
        self.assertFalse((self.output / "SHA256SUMS").exists())

    def test_tool_provenance_must_cover_complete_tool_set(self):
        self.tool_names.remove("plugin_icon.png")
        self.write_tools_provenance()
        with self.assertRaisesRegex(ValueError, "tool provenance"):
            pipeline.seal_release(self.source, self.runtime, self.output)
        self.assertFalse((self.output / "SHA256SUMS").exists())

    def test_changed_tool_artifact_is_rejected(self):
        (self.output / "plugin.json").write_text("changed")
        with self.assertRaisesRegex(ValueError, "tool artifact"):
            pipeline.seal_release(self.source, self.runtime, self.output)

    def test_mixed_source_commits_are_rejected(self):
        p = self.output / "TOOLS-PROVENANCE.json"
        data = json.loads(p.read_text())
        data["source"]["commit"] = "0" * 40
        p.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "release source"):
            pipeline.seal_release(self.source, self.runtime, self.output)

    def test_unexpected_asset_prevents_sealing(self):
        (self.output / "private-data.win").write_text("private")
        with self.assertRaisesRegex(ValueError, "release asset set"):
            pipeline.seal_release(self.source, self.runtime, self.output)
        self.assertFalse((self.output / "SHA256SUMS").exists())

    def test_unexpected_directory_prevents_uploading_nested_private_files(self):
        (self.output / "staging").mkdir()
        (self.output / "staging/private-data.win").write_text("private")
        with self.assertRaisesRegex(ValueError, "release asset set"):
            pipeline.seal_release(self.source, self.runtime, self.output)
        self.assertFalse((self.output / "SHA256SUMS").exists())

    def test_symlink_asset_prevents_sealing(self):
        # seal_release copies this asset, so exercise a tool asset instead.
        asset = self.output / "plugin_icon.png"
        payload = self.root / "outside-icon.png"
        payload.write_bytes(asset.read_bytes())
        asset.unlink()
        asset.symlink_to(payload)
        with self.assertRaisesRegex(ValueError, "release asset set"):
            pipeline.seal_release(self.source, self.runtime, self.output)
        self.assertFalse((self.output / "SHA256SUMS").exists())

    def test_missing_asset_prevents_sealing(self):
        (self.output / "prepare-data.wasm").unlink()
        with self.assertRaises((ValueError, FileNotFoundError)):
            pipeline.seal_release(self.source, self.runtime, self.output)
        self.assertFalse((self.output / "SHA256SUMS").exists())

    def test_non_wasm_plugin_prevents_sealing(self):
        (self.output / "plugin.wasm").write_bytes(b"not wasm")
        self.write_tools_provenance()
        with self.assertRaisesRegex(ValueError, "plugin is not WASM"):
            pipeline.seal_release(self.source, self.runtime, self.output)
        self.assertFalse((self.output / "SHA256SUMS").exists())

if __name__ == "__main__":
    unittest.main()
