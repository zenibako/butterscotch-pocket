package prep

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRemoveTreeNestedAndMissing(t *testing.T) {
	root := filepath.Join(t.TempDir(), "tree")
	prepWrite(t, filepath.Join(root, "a/b/file"), []byte("remove"))
	prepWrite(t, filepath.Join(root, "file"), []byte("remove"))
	if err := removeTree(root); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Lstat(root); !os.IsNotExist(err) {
		t.Fatalf("tree remains: %v", err)
	}
	if err := removeTree(root); err != nil {
		t.Fatalf("missing tree: %v", err)
	}
}

func TestRemoveTreeNeverFollowsSymlinks(t *testing.T) {
	outside := t.TempDir()
	sentinel := filepath.Join(outside, "keep")
	prepWrite(t, sentinel, []byte("keep"))
	root := filepath.Join(t.TempDir(), "tree")
	if err := os.Mkdir(root, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, filepath.Join(root, "redirect")); err != nil {
		t.Skip(err)
	}
	if err := removeTree(root); err != nil {
		t.Fatal(err)
	}
	alias := filepath.Join(t.TempDir(), "alias")
	if err := os.Symlink(outside, alias); err != nil {
		t.Fatal(err)
	}
	if err := removeTree(alias); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(sentinel)
	if err != nil || string(got) != "keep" {
		t.Fatalf("followed symlink: %v", err)
	}
}

func TestInstallCleanupFailureIsSuccessfulWarning(t *testing.T) {
	root, source, names, _ := installFixture(t, "undertale", false)
	remover := func(path string) error {
		if strings.HasPrefix(filepath.Base(path), ".butterscotch-install-") {
			return errors.New("injected cleanup failure")
		}
		return removeTree(path)
	}
	result, err := installWithOps(InstallOptions{Root: root, Game: "undertale", DiscardOriginals: true}, os.Rename, remover)
	if err != nil || !result.OriginalsDiscarded || !strings.Contains(result.Warning, "cleanup") {
		t.Fatalf("result %+v: %v", result, err)
	}
	if _, err := os.Lstat(source); !os.IsNotExist(err) {
		t.Fatalf("originals remain: %v", err)
	}
	for _, name := range names {
		if _, err := os.Stat(filepath.Join(filepath.Dir(source), name)); err != nil {
			t.Fatal(err)
		}
	}
	matches, _ := filepath.Glob(filepath.Join(filepath.Dir(source), ".butterscotch-install-*"))
	if len(matches) != 1 || !strings.Contains(result.Warning, matches[0]) {
		t.Fatalf("cleanup location not reported: %+v %v", result, matches)
	}
}
