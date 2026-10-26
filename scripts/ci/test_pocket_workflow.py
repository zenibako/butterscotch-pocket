"""Execute Pocket workflow shell steps with recording tools, without an SDK."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import textwrap
import unittest


WORKFLOW = Path(__file__).parents[2] / ".github/workflows/build-pocket.yml"
BUILD_STEPS = (
    "Build the RISC-V programs", "Check the interpreter loop is in block RAM",
    "Preserve the checked Pocket programs for upload", "Build the desktop test binaries",
    "Lint the port's own files", "Check save-state regressions with sanitizers",
    "Check script profiler accounting and toggles", "Check builtin compatibility with sanitizers",
    "Check deferred and physics callback context",
    "Check clipped sprite pixels and texture bounds", "Check font sampling and menu layout",
    "Check shared battle speech and UI sampling",
)

# Only extract the literal named shell blocks used here; actionlint validates
# the complete YAML. No third-party parser is needed by the selector job.
def step(name):
    text = WORKFLOW.read_text()
    match = re.search(r"^      - name: " + re.escape(name) + r"\n(.*?)(?=^      - |\Z)",
                      text, re.M | re.S)
    if not match:
        raise AssertionError(f"Missing workflow step: {name}")
    return match[1]


def command(name):
    block = step(name)
    match = re.search(r"^        run: (.*)$", block, re.M)
    if not match:
        raise AssertionError(f"Missing shell command: {name}")
    if match[1] == "|":
        lines = []
        for line in block[match.end() + 1:].splitlines():
            if line.strip() and not line.startswith("          "):
                break
            lines.append(line)
        return textwrap.dedent("\n".join(lines))
    return match[1]


STUB = r'''import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
record = dict(tool=name, args=sys.argv[1:])
record.update({k: os.environ.get(k) for k in ("MAKEFLAGS", "PC_CC", "CC")})
with open(os.environ["RECORD"], "a") as out:
    out.write(json.dumps(record) + "\n")
if name == "riscv64-unknown-elf-nm":
    print("00004000 00000100 T executeBytecodeInstructions")
if name == "make":
    values = dict(arg.split("=", 1) for arg in sys.argv[1:] if "=" in arg)
    game = values.get("GAME", "undertale")
    if "elf" in sys.argv:
        path = pathlib.Path("out/.obj") / game / "app.elf"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(game)
    for arg in sys.argv[1:]:
        if arg.endswith("-fixture") and "=" not in arg:
            path = pathlib.Path(values["PC_BIN"] + "_" + arg.replace("-", "_"))
            path.write_text(pathlib.Path(__file__).read_text())
            path.chmod(0o755)
if name == os.environ.get("FAIL_FIXTURE"):
    sys.exit(23)
'''


class PocketWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.tools = self.root / "tools"
        self.tools.mkdir()
        for name in ("make", "cc", "ccache", "riscv64-unknown-elf-gcc", "riscv64-unknown-elf-nm"):
            path = self.tools / name
            path.write_text(f"#!{sys.executable}\n" + STUB)
            path.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.tools) + os.pathsep + os.environ["PATH"],
                        RECORD=str(self.root / "commands.jsonl"), GITHUB_OUTPUT=str(self.root / "outputs"))
        self.env.pop("CC", None)  # Ignore the developer's compiler override, not the workflow's.
        # The workflow's global env contains scalar values only.
        block = WORKFLOW.read_text().split("\nenv:\n", 1)[1].split("\npermissions:", 1)[0]
        for key, value in re.findall(r"^  ([A-Z_]+): (.+)$", block, re.M):
            self.env[key] = value.strip('"\'').replace("${{ github.workspace }}", str(self.root))

    def run_step(self, name, check=True):
        return subprocess.run(["bash", "-eo", "pipefail", "-c", command(name)], cwd=self.root,
                              env=self.env, text=True, capture_output=True, check=check)

    def build_records(self):
        for name in BUILD_STEPS:
            self.run_step(name)
        return [json.loads(line) for line in (self.root / "commands.jsonl").read_text().splitlines()]

    def test_every_existing_build_guard_and_fixture_still_runs(self):
        records = self.build_records()
        executions = [(row["tool"], row["args"]) for row in records if row["tool"].startswith("bs_pc")]
        self.assertEqual(executions, [
            ("bs_pc_profiler_fixture", []), ("bs_pc_generic_profiler_fixture", []),
            ("bs_pc_compat_builtin_compat_fixture", []),
            ("bs_pc_callbacks_call_later_fixture", []), ("bs_pc_callbacks_physics_contact_fixture", []),
            ("bs_pc_clipping_sprite_clipping_fixture", []),
            ("bs_pc_fonts_font_sampling_fixture", []),
            *[("bs_pc_fonts_menu_fixture", [str(height)]) for height in (0, 6, 16, 24)],
            ("bs_pc_battle_undertale_battle_contrast_fixture", []),
            ("bs_pc_battle_deltarune_battle_contrast_fixture", []),
        ])
        makes = [row["args"] for row in records if row["tool"] == "make"]
        for target in ("undertale_pc", "deltarune_pc", "snapshot-tests", "lint"):
            self.assertTrue(any(target in args for args in makes), target)
        self.assertEqual(sum("elf" in args for args in makes), 2)
        self.assertEqual(sum(row["tool"] == "riscv64-unknown-elf-nm" for row in records), 2)
        for game in ("undertale", "deltarune"):
            self.assertEqual((self.root / f"out/artifacts/{game}.elf").read_text(), game)

    def test_compiler_wrappers_and_two_job_limit_do_not_export_cross_cc(self):
        for row in self.build_records():
            if row["tool"] != "make":
                continue
            self.assertEqual(row["MAKEFLAGS"], "-j2")
            self.assertEqual(row["PC_CC"], "ccache cc")
            self.assertIsNone(row["CC"])
            if "elf" in row["args"]:
                self.assertIn("CC=ccache riscv64-unknown-elf-gcc", row["args"])

    def test_only_identical_sanitizer_configurations_share_core_objects(self):
        configurations = []
        for row in self.build_records():
            if row["tool"] != "make":
                continue
            flags = dict(arg.split("=", 1) for arg in row["args"] if "=" in arg)
            if "PC_OBJ_DIR" in flags:
                configurations.append(flags)
        sanitized = [f for f in configurations if "-fsanitize=address,undefined" in f.get("PC_EXTRA_CFLAGS", "")]
        undertale = [f for f in sanitized if f.get("GAME", "undertale") == "undertale"]
        self.assertEqual(len(undertale), 7)  # font/menu link separately
        self.assertEqual(len({f["PC_OBJ_DIR"] for f in undertale}), 1)
        self.assertEqual(len({(f["WADS"], f["PC_EXTRA_CFLAGS"]) for f in undertale}), 1)
        deltarune = [f for f in sanitized if f.get("GAME") == "deltarune"]
        self.assertEqual(len(deltarune), 1)
        profilers = [f for f in configurations if f not in sanitized]
        self.assertEqual(len(profilers), 2)
        self.assertEqual(len({f["PC_OBJ_DIR"] for f in [undertale[0], *deltarune, *profilers]}), 4)
        self.assertEqual(profilers[1]["PC_EXTRA_CFLAGS"], "-UOF_FAST_LOOP")

    def test_fixture_failure_stops_its_step(self):
        self.env["FAIL_FIXTURE"] = "bs_pc_fonts_font_sampling_fixture"
        self.assertEqual(self.run_step("Check font sampling and menu layout", check=False).returncode, 23)

    def test_callback_fixture_failure_stops_its_step(self):
        for fixture in ("bs_pc_callbacks_call_later_fixture", "bs_pc_callbacks_physics_contact_fixture"):
            with self.subTest(fixture=fixture):
                self.env["FAIL_FIXTURE"] = fixture
                self.assertEqual(self.run_step("Check deferred and physics callback context", check=False).returncode, 23)

    def test_sanitizer_runtime_checks_are_retained(self):
        for name in BUILD_STEPS[7:]:
            with self.subTest(step=name):
                block = step(name)
                self.assertIn("ASAN_OPTIONS: detect_leaks=0", block)
                self.assertIn("UBSAN_OPTIONS: halt_on_error=1", block)
        self.assertIn("UBSAN_OPTIONS: halt_on_error=1", step(BUILD_STEPS[5]))

    def test_cache_fingerprint_tracks_compiler_contents(self):
        self.run_step("Fingerprint the compilers")
        first = (self.root / "outputs").read_text()
        self.assertRegex(first, r"^hash=[0-9a-f]{64}\n$")
        with (self.tools / "cc").open("a") as compiler:
            compiler.write("\n# new compiler\n")
        (self.root / "outputs").unlink()
        self.run_step("Fingerprint the compilers")
        self.assertNotEqual(first, (self.root / "outputs").read_text())

    def test_cache_uses_only_compiler_storage_and_compatible_restore_keys(self):
        cache = step("Restore compiler cache")
        self.assertIn("uses: actions/cache@v6", cache)
        self.assertIn("path: ${{ env.CCACHE_DIR }}", cache)
        self.assertNotIn("out/.obj", cache)
        prefix = "pocket-ccache-v1-${{ runner.os }}-${{ runner.arch }}-${{ env.SDK_REF }}-${{ steps.toolchains.outputs.hash }}-"
        self.assertIn("key: " + prefix + "${{ github.sha }}", cache)
        self.assertIn("restore-keys: " + prefix, cache)
        self.assertEqual(self.env["CCACHE_COMPILERCHECK"], "content")
        self.assertNotIn("cache-hit", WORKFLOW.read_text())  # cache hits never skip tests


if __name__ == "__main__":
    unittest.main()
