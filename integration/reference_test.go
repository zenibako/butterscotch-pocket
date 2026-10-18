package integration

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/zenibako/butterscotch-pocket/internal/testgame"
	"github.com/zenibako/butterscotch-pocket/prep"
)

// The C converters remain an independent oracle, not a runtime dependency.
func TestCReference(t *testing.T) {
	required := os.Getenv("REQUIRE_REFERENCE_TESTS") == "1"
	source := os.Getenv("BUTTERSCOTCH_SOURCE")
	if source == "" {
		if required {
			t.Fatal("BUTTERSCOTCH_SOURCE is required for C reference tests")
		}
		t.Skip("set BUTTERSCOTCH_SOURCE to the pinned Butterscotch fork checkout to run C reference tests")
	}
	script, err := filepath.Abs(filepath.Join(source, "src", "openfpga", "tools", "prepare_game.py"))
	if err != nil {
		t.Fatal(err)
	}
	if info, err := os.Stat(script); err != nil || !info.Mode().IsRegular() {
		t.Fatalf("BUTTERSCOTCH_SOURCE must point to a Butterscotch fork checkout containing src/openfpga/tools/prepare_game.py: %s", script)
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
