import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("pipeline", Path(__file__).with_name("pipeline.py"))
pipeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pipeline)

class PackageTests(unittest.TestCase):
    def setUp(self):
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

    def test_only_public_files_and_matching_runtime(self):
        secret = self.dist / "Assets/butterscotch/common/data.win"
        secret.write_text("PRIVATE GAME")
        (self.dist / "Saves").mkdir()
        (self.dist / "Saves/secret.sav").write_text("PRIVATE SAVE")
        archive = pipeline.package_core(self.source, self.programs, self.output, "v0.2.0")
        with zipfile.ZipFile(archive) as z:
            self.assertNotIn("Assets/butterscotch/common/data.win", z.namelist())
            self.assertFalse(any(n.startswith("Saves/") for n in z.namelist()))
            self.assertFalse(any("Sleep Test" in n for n in z.namelist()))
            core = json.loads(z.read(pipeline.CORE_JSON))["core"]
            self.assertTrue(core["framework"]["sleep_supported"])
            self.assertEqual(core["metadata"]["version"], "0.2.0")
            self.assertIn("Assets/butterscotch/common/deltarune.elf", z.namelist())
            self.assertEqual(z.read("Cores/zenibako.Butterscotch/os25.rbf_r"), (pipeline.ROOT/"release/runtime/os25.rbf_r").read_bytes())

    def test_missing_program_fails_without_publishing(self):
        (self.programs/"deltarune.elf").unlink()
        with self.assertRaises((FileNotFoundError, ValueError)):
            pipeline.package_core(self.source, self.programs, self.output, "v0.2.0")
        self.assertFalse((self.output/"butterscotch-pocket.zip").exists())

    def test_packaged_hardware_matches_supported_game_core(self):
        path = self.dist / pipeline.CORE_JSON
        definition = json.loads(path.read_text())
        definition["core"]["framework"] = {
            "target_product": "Analogue Pocket", "version_required": "2.2",
            "sleep_supported": False,
            "dock": {"supported": True, "analog_output": False},
            "hardware": {"link_port": True, "cartridge_adapter": 0},
        }
        path.write_text(json.dumps(definition))
        archive = pipeline.package_core(self.source, self.programs, self.output, "v0.2.0")
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
        archive = pipeline.package_core(self.source, self.programs, self.output, "v0.2.0")
        with zipfile.ZipFile(archive) as z:
            text = z.read(f"Cores/{pipeline.CORE_ID}/info.txt").decode("ascii")
        self.assertLessEqual(len(text.splitlines()), 32)
        self.assertIn("Save-state status", text)
        self.assertIn("experimental", text)
        self.assertIn("No cartridge or link-cable support", text)
        self.assertIn("v1.08", text)

    def test_wrong_elf_architecture_is_rejected(self):
        (self.programs/"undertale.elf").write_bytes(b"not an ELF")
        with self.assertRaises(ValueError):
            pipeline.package_core(self.source, self.programs, self.output, "v0.2.0")

    def test_corrupt_runtime_is_rejected(self):
        lock = json.loads((pipeline.ROOT/"release/core.lock.json").read_text())
        lock["sha256"]["release/runtime/os.bin"] = "0"*64
        with self.assertRaises(ValueError):
            pipeline.verify_pins(lock)

    def test_mutable_commit_is_rejected(self):
        lock = json.loads((pipeline.ROOT/"release/core.lock.json").read_text())
        lock["core"]["commit"] = "main"
        with self.assertRaises(ValueError):
            pipeline.verify_pins(lock)

if __name__ == "__main__":
    unittest.main()
