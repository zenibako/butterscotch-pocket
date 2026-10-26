"""Check the privilege boundary between reusable validation and draft publication."""
from pathlib import Path
import re
import unittest

WORKFLOWS = Path(__file__).resolve().parents[3] / ".github/workflows"


def jobs(text):
    """Read job sections at the fixed indentation required by these workflows."""
    body = text.split("\njobs:\n", 1)[1]
    matches = list(re.finditer(r"^  ([a-z][a-z0-9-]*):\n", body, re.M))
    return {match.group(1): body[match.end():matches[i + 1].start() if i + 1 < len(matches) else len(body)]
            for i, match in enumerate(matches)}


class WorkflowPermissionTests(unittest.TestCase):
    def test_explicit_version_takes_precedence_over_tag_context(self):
        workflow = (WORKFLOWS / "release-pocket.yml").read_text()
        version = re.search(r"^  RELEASE_VERSION: \$\{\{ (.*) \}\}$", workflow, re.M).group(1)
        self.assertTrue(version.startswith("inputs.version ||"), version)

    def test_reusable_validation_cannot_elevate_read_only_caller(self):
        workflow = (WORKFLOWS / "release-pocket.yml").read_text()
        self.assertIn("  workflow_call:\n", workflow)
        self.assertIn("\npermissions:\n  contents: read\n", workflow)
        self.assertNotRegex(workflow, r"contents:\s*write")
        self.assertNotIn("gh release create", workflow)
        self.assertNotIn("create_draft:", workflow)

    def test_publisher_gates_only_write_job_on_complete_same_version_validation(self):
        workflow = (WORKFLOWS / "publish-pocket.yml").read_text()
        self.assertIn("\npermissions:\n  contents: read\n", workflow)
        self.assertIn("  workflow_dispatch:\n", workflow)
        self.assertNotIn("  workflow_call:\n", workflow)
        sections = jobs(workflow)
        self.assertEqual(set(sections), {"validation", "draft-release"})
        self.assertIn("uses: ./.github/workflows/release-pocket.yml", sections["validation"])
        self.assertIn("version: ${{ inputs.version }}", sections["validation"])
        self.assertNotIn("contents: write", sections["validation"])
        publisher = sections["draft-release"]
        self.assertIn("needs: validation", publisher)
        self.assertIn("if: github.event_name == 'workflow_dispatch' && inputs.create_draft", publisher)
        self.assertIn("contents: write", publisher)
        self.assertIn("name: butterscotch-pocket-distribution", publisher)
        self.assertIn("sha256sum -c SHA256SUMS", publisher)
        self.assertIn('gh release create "$RELEASE_VERSION" --draft --target "$GITHUB_SHA"', publisher)
