package integration

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/zenibako/butterscotch-pocket/src/openfpga/tools/pocket-data/internal/testgame"
	"github.com/zenibako/butterscotch-pocket/src/openfpga/tools/pocket-data/prep"
)

// The C converters remain an independent oracle, not a runtime dependency.
func TestCReference(t *testing.T) {
	required := os.Getenv("REQUIRE_REFERENCE_TESTS") == "1"
	source := os.Getenv("BUTTERSCOTCH_SOURCE")
	script := filepath.Join("..", "..", "prepare_game.py")
	if source != "" {
		script = filepath.Join(source, "src", "openfpga", "tools", "prepare_game.py")
	}
	script, err := filepath.Abs(script)
	if err != nil {
		t.Fatal(err)
	}
	if info, err := os.Stat(script); err != nil || !info.Mode().IsRegular() {
		if source == "" && os.IsNotExist(err) && !required {
			t.Skip("C reference needs the full checkout containing src/openfpga/tools/prepare_game.py; set BUTTERSCOTCH_SOURCE to use another checkout")
		}
		t.Fatalf("C reference requires src/openfpga/tools/prepare_game.py in the local checkout or BUTTERSCOTCH_SOURCE override: %s", script)
	}
	python, err := exec.LookPath("python3")
	if err != nil {
		if required {
			t.Fatal("C oracle requires developer Python")
		}
		t.Skip("C oracle needs developer Python and a host C compiler")
	}
	if _, err = exec.LookPath("cc"); err != nil {
		if required {
			t.Fatal("C oracle requires a host C compiler")
		}
		t.Skip("C oracle needs a host C compiler")
	}
	root := t.TempDir()
	tools := filepath.Join(root, "tools")
	cmd := exec.Command(python, script, "--build-tools", "--tools-dir", tools)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build C oracle: %v\n%s", err, out)
	}
	for _, game := range []string{"undertale", "deltarune"} {
		t.Run(game, func(t *testing.T) {
			source := filepath.Join(root, game)
			if err := testgame.Write(source, game); err != nil {
				t.Fatal(err)
			}
			addVorbis(t, source)
			cOut := filepath.Join(root, game+"-c")
			gOut := filepath.Join(root, game+"-go")
			cmd := exec.Command(python, script, "--game", game, "--source", source, "--output", cOut, "--tools-dir", tools)
			if out, err := cmd.CombinedOutput(); err != nil {
				t.Fatalf("C reference: %v\n%s", err, out)
			}
			if _, err := prep.Prepare(prep.Options{Game: game, Source: source, Output: gOut}); err != nil {
				t.Fatal(err)
			}
			compareAssets(t, cOut, gOut)
		})
	}
}

// Run the actual oracle test in an empty checkout to exercise its failure/skip
// policy without depending on the checkout running this parent test.
func TestCReferenceMissingSource(t *testing.T) {
	binary, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		name, source, required string
		wantFailure            bool
	}{
		{"optional local", "", "", false},
		{"required local", "", "1", true},
		{"explicit override", t.TempDir(), "", true},
		{"required override", t.TempDir(), "1", true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cmd := exec.Command(binary, "-test.run=^TestCReference$", "-test.v")
			cmd.Dir = t.TempDir()
			cmd.Env = append(os.Environ(), "BUTTERSCOTCH_SOURCE="+tc.source, "REQUIRE_REFERENCE_TESTS="+tc.required)
			output, err := cmd.CombinedOutput()
			if (err != nil) != tc.wantFailure {
				t.Fatalf("missing C reference: err=%v, wantFailure=%v\n%s", err, tc.wantFailure, output)
			}
			if !tc.wantFailure && !strings.Contains(string(output), "--- SKIP: TestCReference") {
				t.Fatalf("missing optional oracle was not skipped: %s", output)
			}
		})
	}
}
