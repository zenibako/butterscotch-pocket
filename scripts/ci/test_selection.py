"""Exercise the selector and final CI verdict through their command-line APIs."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("selection.py")
DOMAINS = {"unix", "windows", "macos", "haiku", "ps2", "ps3", "vita",
           "switch", "web", "android", "compilers", "pocket", "pocket_tools", "tests"}
GROUPS = {"Unix": {"unix"}, "Windows": {"windows"}, "macOS": {"macos"},
          "Haiku": {"haiku"}, "Consoles": {"ps2", "ps3", "vita", "switch"},
          "Other": {"web", "android"}, "Compilers": {"compilers"},
          "Pocket": {"pocket"}, "PocketTools": {"pocket_tools"}, "Tests": {"tests"}}


class SelectionTests(unittest.TestCase):
    def invoke(self, command, value, *args, env=None):
        if env is None:
            env = dict(os.environ)
            env.pop("GITHUB_OUTPUT", None)
        else:
            env = dict(env)
        env.pop("GITHUB_STEP_SUMMARY", None)
        return subprocess.run([sys.executable, str(SCRIPT), command, *args],
                              input=json.dumps(value), text=True, capture_output=True, env=env)

    def selected(self, paths):
        result = self.invoke("select", paths, "--paths-json", "-")
        self.assertEqual(result.returncode, 0, result.stderr)
        value = json.loads(result.stdout)
        self.assertEqual(set(value), DOMAINS | {"full_matrix"})
        self.assertTrue(all(type(x) is bool for x in value.values()))
        return {name for name in DOMAINS if value[name]}

    def test_pocket_platform_change_skips_unrelated_builds(self):
        self.assertEqual(self.selected(["src/openfpga/platform/of_menu.c"]), {"pocket", "pocket_tools"})

    def test_release_inputs_select_program_and_distribution_checks(self):
        for path in ("src/openfpga/release/pipeline.py", "src/openfpga/release/core.lock.json",
                     "src/openfpga/release/runtime/os.bin", "src/openfpga/Makefile"):
            with self.subTest(path=path):
                self.assertEqual(self.selected([path]), {"pocket", "pocket_tools"})
        for path in (".github/workflows/release-pocket.yml", ".github/workflows/publish-pocket.yml"):
            self.assertEqual(self.selected([path]), {"pocket_tools"})

    def test_go_module_and_bundled_guide_select_tools(self):
        for path in ("src/openfpga/tools/pocket-data/go.mod", "src/openfpga/GAME-DATA.md"):
            with self.subTest(path=path):
                self.assertEqual(self.selected([path]), {"pocket_tools"})

    def test_c_reference_changes_select_both_conversion_and_port_checks(self):
        for path in ("src/openfpga/tools/prepare_game.py", "src/openfpga/tools/mktexpack.c",
                     "src/openfpga/tools/mkmusic.c", "src/openfpga/platform/of_log.c",
                     "src/openfpga/platform/of_music_pack.h", "src/openfpga/platform/of_hooks.h",
                     "src/openfpga/platform/of_perf.h", "src/openfpga/platform/of_save_format.h"):
            with self.subTest(path=path):
                self.assertEqual(self.selected([path]), {"pocket", "pocket_tools"})

    def test_shared_code_and_build_inputs_select_every_domain(self):
        for path in ("src/sw/sw_drawing.c", "src/runner.c", "src/rvalue.h",
                     "src/image/image_decoder.c", "src/embedded/stb_impl.c", "vendor/stb/image.h",
                     "Makefile", "CMakeLists.txt", "cmake/new-platform.cmake"):
            with self.subTest(path=path):
                self.assertEqual(self.selected([path]), DOMAINS)

    def test_snapshot_codec_remains_pocket_only(self):
        for path in ("src/runner_snapshot.c", "src/runner_snapshot.h",
                     "tests/runner_snapshot_test.c", "tests/sw_button_cache_test.c"):
            with self.subTest(path=path):
                self.assertEqual(self.selected([path]), {"pocket"})

    def test_isolated_ports_select_only_their_domain(self):
        for path, domain in (("src/ps2/main.c", "ps2"), ("src/audio/ps2/audio.c", "ps2"),
                             ("src/ps3/main.c", "ps3"), ("cmake/ppu.cmake", "ps3"),
                             ("src/vita/main.c", "vita"), ("src/switch/main.c", "switch"),
                             ("src/web/index.html", "web"), ("src/backends/web.c", "web"),
                             ("src/android/frontend/build.gradle", "android"),
                             ("src/backends/appkit.m", "macos"),
                             ("artifacts/windows/icon.ico", "windows")):
            with self.subTest(path=path):
                self.assertEqual(self.selected([path]), {domain})

    def test_mixed_changes_union_their_dependencies(self):
        self.assertEqual(self.selected(["src/ps2/main.c", "src/openfpga/platform/of_menu.c",
                                        "docs/notes.md"]), {"ps2", "pocket", "pocket_tools"})

    def test_move_between_ports_checks_both_old_and_new_paths(self):
        self.assertEqual(self.selected(["src/ps2/input.c", "src/vita/input.c"]), {"ps2", "vita"})

    def test_general_docs_and_skills_skip_heavy_jobs(self):
        self.assertEqual(self.selected(["README.md", "docs/design.md", ".claude/skills/x/SKILL.md",
                                        "src/openfpga/docs/results.md"]), set())

    def test_non_document_under_docs_does_not_hide_code(self):
        self.assertEqual(self.selected(["docs/tool.py"]), DOMAINS)

    def test_shared_game_fixture_selects_core_and_pocket(self):
        self.assertEqual(self.selected(["tests/try-catch-finally-test/data.win"]), {"tests", "pocket"})

    def test_workflow_changes_run_the_jobs_they_define(self):
        self.assertEqual(self.selected([".github/workflows/build-console.yml"]),
                         {"ps2", "ps3", "vita", "switch"})
        self.assertEqual(self.selected([".github/workflows/build-other.yml"]), {"web", "android"})

    def test_dispatcher_policy_and_unknown_paths_select_every_domain(self):
        for path in (".github/workflows/build.yml", "scripts/ci/selection.py", "new-shared-file",
                     ".github/workflows/new.yml", "../src/ps2/main.c"):
            with self.subTest(path=path):
                self.assertEqual(self.selected([path]), DOMAINS)

    def test_manual_dispatch_forces_all_platforms_and_expanded_matrix(self):
        result = self.invoke("select", {}, "--event-name", "workflow_dispatch", "--event-file", "-")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), dict.fromkeys(DOMAINS | {"full_matrix"}, True))

    def test_initial_push_or_missing_diff_defaults_to_full(self):
        for event in ({"before": "0" * 40, "after": "a" * 40},
                      {"before": "a" * 40, "after": "b" * 40}):
            with tempfile.TemporaryDirectory() as directory:
                result = self.invoke("select", event, "--event-name", "push", "--event-file", "-",
                                     "--repo", directory)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout), dict.fromkeys(DOMAINS | {"full_matrix"}, True))

    def test_invalid_event_never_silently_skips_builds(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "select", "--event-name", "push",
                                 "--event-file", "-"], input="{broken", text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(all(json.loads(result.stdout).values()))

    def test_empty_diff_skips_builds_and_keeps_the_gate(self):
        self.assertEqual(self.selected([]), set())

    def test_real_git_empty_push_and_pr_diff(self):
        repo = SCRIPT.parents[2]
        head = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
        for event_name, event in (("push", {"before": head, "after": head}),
                                  ("pull_request", {"pull_request": {"base": {"sha": head},
                                                                    "head": {"sha": head}}})):
            result = self.invoke("select", event, "--event-name", event_name, "--event-file", "-",
                                 "--repo", str(repo))
            self.assertEqual(result.returncode, 0, result.stderr)
            value = json.loads(result.stdout)
            self.assertFalse(any(value[name] for name in DOMAINS))
            self.assertEqual(value["full_matrix"], event_name == "push")

    def test_outputs_do_not_interpret_changed_filenames_as_commands_or_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "outputs"
            env = dict(os.environ, GITHUB_OUTPUT=str(output))
            result = self.invoke("select", ["src/openfpga/a\nadmin=true.c"], "--paths-json", "-", env=env)
            self.assertEqual(result.returncode, 0, result.stderr)
            lines = dict(line.split("=", 1) for line in output.read_text().splitlines())
            self.assertNotIn("admin", lines)
            self.assertEqual(lines["pocket"], "true")
            self.assertEqual(json.loads(lines["selection"])["pocket"], True)

    def verdict(self, selected, changes="success"):
        selection = {name: name in selected for name in DOMAINS}
        selection["full_matrix"] = False
        needs = {"Changes": {"result": changes, "outputs": {"selection": json.dumps(selection)}}}
        needs.update({job: {"result": "success" if domains & selected else "skipped"}
                      for job, domains in GROUPS.items()})
        return needs

    def test_gate_accepts_passed_selected_jobs_and_expected_skips(self):
        for selected in (set(), {"pocket"}, {"ps2", "android"}, DOMAINS):
            result = self.invoke("check", self.verdict(selected), "--needs-json", "-")
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_gate_rejects_failed_cancelled_or_skipped_selected_job(self):
        for state in ("failure", "cancelled", "skipped", "unknown"):
            needs = self.verdict({"pocket"})
            needs["Pocket"]["result"] = state
            result = self.invoke("check", needs, "--needs-json", "-")
            self.assertNotEqual(result.returncode, 0, state)

    def test_gate_rejects_failed_classifier_or_missing_results(self):
        needs = self.verdict(set(), changes="failure")
        self.assertNotEqual(self.invoke("check", needs, "--needs-json", "-").returncode, 0)
        needs = self.verdict({"pocket"})
        del needs["Pocket"]
        self.assertNotEqual(self.invoke("check", needs, "--needs-json", "-").returncode, 0)

    def test_gate_rejects_missing_or_malformed_selection(self):
        for raw in ("", "{}", '{"pocket":"false"}'):
            needs = self.verdict(set())
            needs["Changes"]["outputs"]["selection"] = raw
            self.assertNotEqual(self.invoke("check", needs, "--needs-json", "-").returncode, 0)

    def test_gate_does_not_hide_failure_of_an_unexpected_extra_build(self):
        needs = self.verdict({"pocket"})
        needs["Unix"]["result"] = "failure"
        self.assertNotEqual(self.invoke("check", needs, "--needs-json", "-").returncode, 0)


if __name__ == "__main__":
    unittest.main()
