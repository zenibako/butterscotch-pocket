package integration

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	extism "github.com/extism/go-sdk"
	"github.com/tetratelabs/wazero"
	"github.com/zenibako/butterscotch-pocket/internal/testgame"
	"github.com/zenibako/butterscotch-pocket/prep"
)

func TestExtismPlugin(t *testing.T) {
	buildRoot := t.TempDir()
	wasm := filepath.Join(buildRoot, "plugin.wasm")
	if external := os.Getenv("POCKET_PLUGIN_WASM"); external != "" {
		wasm = external
	} else {
		compiler := os.Getenv("TINYGO")
		if compiler == "" {
			compiler, _ = exec.LookPath("tinygo")
		}
		if compiler == "" {
			if os.Getenv("REQUIRE_PLUGIN_TESTS") == "1" {
				t.Fatal("TinyGo is required for plugin integration tests")
			}
			t.Skip("plugin host test requires TinyGo; WASI CLI tests still run")
		}
		cmd := exec.Command(compiler, "build", "-p", "2", "-trimpath", "-target", "wasip1", "-buildmode=c-shared", "-o", wasm, "./cmd/pocket-plugin")
		cmd.Dir = ".."
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("build TinyGo plugin: %v\n%s", err, out)
		}
	}
	for _, first := range []string{"pocket", "host"} {
		t.Run(first, func(t *testing.T) { testExtismMountOrder(t, wasm, first) })
	}
}
func testExtismMountOrder(t *testing.T, wasm, first string) {
	root := t.TempDir()
	pocket := filepath.Join(root, "sd")
	host := filepath.Join(root, "host")
	for _, dir := range []string{pocket, host} {
		if err := os.MkdirAll(dir, 0755); err != nil {
			t.Fatal(err)
		}
	}
	sentinel := filepath.Join(pocket, "Saves/butterscotch/common/progress.sav")
	if err := os.MkdirAll(filepath.Dir(sentinel), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(sentinel, []byte("keep progress"), 0644); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	var log strings.Builder
	print := extism.NewHostFunctionWithStack("print_msg", func(_ context.Context, p *extism.CurrentPlugin, stack []uint64) {
		s, err := p.ReadString(stack[0])
		if err != nil {
			t.Error(err)
		}
		log.WriteString(s)
		stack[0] = 0
	}, []extism.ValueType{extism.ValueTypeI64}, []extism.ValueType{extism.ValueTypeI64})
	manifest := extism.Manifest{Wasm: []extism.Wasm{extism.WasmFile{Path: wasm}}}
	fs := wazero.NewFSConfig()
	if first == "pocket" {
		fs = fs.WithDirMount(pocket, "pocket").WithDirMount(host, "host")
	} else {
		fs = fs.WithDirMount(host, "host").WithDirMount(pocket, "pocket")
	}
	p, err := extism.NewPlugin(ctx, manifest, extism.PluginConfig{EnableWasi: true, RuntimeConfig: wazero.NewRuntimeConfig().WithCloseOnContextDone(true), ModuleConfig: wazero.NewModuleConfig().WithFSConfig(fs)}, []extism.HostFunction{print})
	if err != nil {
		t.Fatal(err)
	}
	defer p.Close(ctx)
	call := func(fn string, input []byte) []byte {
		t.Helper()
		code, out, err := p.CallWithContext(ctx, fn, input)
		if err != nil || code != 0 {
			t.Fatalf("%s failed code=%d err=%v out=%s", fn, code, err, out)
		}
		return out
	}
	type choice struct {
		Name    string   `json:"name"`
		Query   string   `json:"query"`
		Choices []string `json:"choices"`
	}
	get := func(b []byte) choice {
		t.Helper()
		var msg struct {
			Choice choice `json:"Choice"`
		}
		if err := json.Unmarshal(b, &msg); err != nil || msg.Choice.Name == "" {
			t.Fatalf("bad plugin choice %s", b)
		}
		return msg.Choice
	}
	answer := func(c choice, value string) []byte {
		t.Helper()
		b, _ := json.Marshal(map[string]any{"Answer": map[string]string{"name": c.Name, "value": value}})
		return call("handle_response", b)
	}
	menu := get(call("start", nil))
	for _, tc := range []struct{ game, label string }{{"undertale", "Undertale 1.08"}, {"deltarune", "Deltarune Chapter 1"}} {
		source := filepath.Join(pocket, "Assets/butterscotch/common", tc.game)
		if err := testgame.Write(source, tc.game); err != nil {
			t.Fatal(err)
		}
		addVorbis(t, source)
		native := filepath.Join(root, tc.game+"-native")
		if _, err := prep.Prepare(prep.Options{Game: tc.game, Source: source, Output: native}); err != nil {
			t.Fatal(err)
		}
		confirm := get(answer(menu, tc.label))
		done := get(answer(confirm, "Install and keep originals"))
		if !strings.Contains(done.Query, "Installed in Assets/butterscotch/common") {
			t.Fatalf("not completed: %s", done.Query)
		}
		output := pocket
		compareAssets(t, native, output)
		assertNoInstallWork(t, pocket)
		menu = get(answer(done, "Back"))
		confirm = get(answer(menu, tc.label))
		done = get(answer(confirm, "Install and discard originals"))
		if !strings.Contains(done.Query, "Originals discarded") {
			t.Fatalf("discard installation did not complete: %s", done.Query)
		}
		compareAssets(t, native, output)
		if _, err := os.Stat(source); !os.IsNotExist(err) {
			t.Fatal("original source folder remains after discard", err)
		}
		assertNoInstallWork(t, pocket)
		menu = get(answer(done, "Back"))
	}
	if !strings.Contains(log.String(), "Converting textures\n") || !strings.Contains(log.String(), "Audio: processing track 1\n") {
		t.Fatalf("missing host progress: %s", log.String())
	}
	if out := call("handle_response", []byte(`"Kill"`)); string(out) != `"Exit"` {
		t.Fatalf("kill: %s", out)
	}
	kept, err := os.ReadFile(sentinel)
	if err != nil || !bytes.Equal(kept, []byte("keep progress")) {
		t.Fatal("save changed")
	}
	t.Log("Actual Extism exports converted both games, matched native bytes, installed into Assets, tested keep/discard, preserved saves, logged progress, and handled Kill.")
}

func assertNoInstallWork(t *testing.T, pocket string) {
	t.Helper()
	common := filepath.Join(pocket, "Assets/butterscotch/common")
	entries, err := os.ReadDir(common)
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), ".butterscotch-install-") || strings.HasPrefix(entry.Name(), ".butterscotch-data-") {
			t.Fatalf("left temporary installation data: %s", entry.Name())
		}
	}
}
