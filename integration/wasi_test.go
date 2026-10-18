package integration

import (
	"bytes"
	"context"
	"crypto/rand"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"github.com/tetratelabs/wazero"
	"github.com/tetratelabs/wazero/imports/wasi_snapshot_preview1"
	"github.com/zenibako/butterscotch-pocket/internal/testgame"
)

func build(t *testing.T, target, path, mode string) {
	t.Helper()
	args := []string{"build", "-trimpath"}
	if mode != "" {
		args = append(args, "-buildmode="+mode)
	}
	args = append(args, "-o", path, target)
	cmd := exec.Command("go", args...)
	cmd.Dir = ".."
	cmd.Env = append(os.Environ(), "CGO_ENABLED=0")
	if filepath.Ext(path) == ".wasm" {
		cmd.Env = append(cmd.Env, "GOOS=wasip1", "GOARCH=wasm")
	}
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
}

func TestWASIParity(t *testing.T) {
	if runtime.GOOS == "wasip1" {
		t.Skip("host-side integration test")
	}
	root := t.TempDir()
	native := filepath.Join(root, "prepare-data")
	if runtime.GOOS == "windows" {
		native += ".exe"
	}
	wasm := filepath.Join(root, "prepare-data.wasm")
	build(t, "./cmd/prepare-data", native, "")
	build(t, "./cmd/prepare-data", wasm, "")
	code, err := os.ReadFile(wasm)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	rt := wazero.NewRuntimeWithConfig(ctx, wazero.NewRuntimeConfig().WithCloseOnContextDone(true))
	defer rt.Close(ctx)
	if _, err = wasi_snapshot_preview1.Instantiate(ctx, rt); err != nil {
		t.Fatal(err)
	}
	compiled, err := rt.CompileModule(ctx, code)
	if err != nil {
		t.Fatal(err)
	}
	for _, game := range []string{"undertale", "deltarune"} {
		t.Run(game, func(t *testing.T) {
			source := filepath.Join(root, game)
			if err := testgame.Write(source, game); err != nil {
				t.Fatal(err)
			}
			addVorbis(t, source)
			nout := filepath.Join(root, game+"-native")
			wout := filepath.Join(root, game+"-wasm")
			cmd := exec.Command(native, "--game", game, "--source", source, "--output", nout)
			if out, err := cmd.CombinedOutput(); err != nil {
				t.Fatalf("native: %v %s", err, out)
			}
			var log bytes.Buffer
			conf := wazero.NewModuleConfig().WithName(game).WithArgs("prepare-data", "--game", game, "--source", "/work/"+game, "--output", "/work/"+game+"-wasm").WithFSConfig(wazero.NewFSConfig().WithDirMount(root, "/work")).WithStdout(&log).WithStderr(&log).WithRandSource(rand.Reader).WithSysWalltime().WithSysNanotime()
			mod, err := rt.InstantiateModule(ctx, compiled, conf)
			if err != nil {
				t.Fatalf("WASI execution: %v\n%s", err, &log)
			}
			_ = mod.Close(ctx)
			compareAssets(t, nout, wout)
			// Re-running refuses existing output and preserves every previously produced byte.
			conf = conf.WithName(game + "-exists")
			if _, err = rt.InstantiateModule(ctx, compiled, conf); err == nil {
				t.Fatal("WASI accepted existing output")
			}
			compareAssets(t, nout, wout)
			t.Logf("native/WASI game files identical; output collision refused. %s", log.String())
		})
	}
}
func compareAssets(t *testing.T, a, b string) {
	t.Helper()
	count := 0
	err := filepath.WalkDir(filepath.Join(a, "Assets"), func(p string, d os.DirEntry, e error) error {
		if e != nil {
			return e
		}
		if d.IsDir() {
			return nil
		}
		rel, _ := filepath.Rel(a, p)
		x, e := os.ReadFile(p)
		if e != nil {
			return e
		}
		y, e := os.ReadFile(filepath.Join(b, rel))
		if e != nil {
			return e
		}
		if !bytes.Equal(x, y) {
			t.Errorf("different %s", rel)
		}
		count++
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if count != 3 {
		t.Fatalf("expected three game files, got %d", count)
	}
}

// Execute the unit suite as wasm32 as well: native tests cannot detect int-width regressions.
func TestWASIUnitSuite(t *testing.T) {
	for _, pkg := range []string{"prep", "third_party/vorbis"} {
		t.Run(pkg, func(t *testing.T) { testWASIUnitSuite(t, pkg) })
	}
}

func testWASIUnitSuite(t *testing.T, pkg string) {
	root := t.TempDir()
	binary := filepath.Join(root, "unit.test.wasm")
	cmd := exec.Command("go", "test", "-C", pkg, "-c", "-o", binary)
	cmd.Dir = ".."
	cmd.Env = append(os.Environ(), "CGO_ENABLED=0", "GOOS=wasip1", "GOARCH=wasm")
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build WASI tests: %v\n%s", err, out)
	}
	code, err := os.ReadFile(binary)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	rt := wazero.NewRuntimeWithConfig(ctx, wazero.NewRuntimeConfig().WithCloseOnContextDone(true))
	defer rt.Close(ctx)
	if _, err = wasi_snapshot_preview1.Instantiate(ctx, rt); err != nil {
		t.Fatal(err)
	}
	packageDir, err := filepath.Abs(filepath.Join("..", pkg))
	if err != nil {
		t.Fatal(err)
	}
	var log bytes.Buffer
	conf := wazero.NewModuleConfig().WithArgs("unit.test", "-test.v").WithFSConfig(wazero.NewFSConfig().WithReadOnlyDirMount(packageDir, "/").WithDirMount(root, "/tmp")).WithEnv("TMPDIR", "/tmp").WithStdout(&log).WithStderr(&log).WithRandSource(rand.Reader).WithSysWalltime().WithSysNanotime()
	if _, err = rt.InstantiateWithConfig(ctx, code, conf); err != nil {
		t.Fatalf("WASI unit suite: %v\n%s", err, &log)
	}
	t.Log(log.String())
}
