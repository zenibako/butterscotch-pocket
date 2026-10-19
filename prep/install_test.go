package prep

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func installFixture(t *testing.T, game string, bad bool) (string, string, []string, []byte) {
	t.Helper()
	root := t.TempDir()
	common := filepath.Join(root, "Assets/butterscotch/common")
	source := filepath.Join(common, game)
	version, group := byte(16), uint32(0)
	names := []string{"data.win", "textures.bin", "music.bin"}
	folder := source
	if game == "deltarune" {
		version, group = 17, 1
		folder = filepath.Join(source, "chapter1_linux")
		names = []string{"deltarune.win", "dr_textures.bin", "dr_music.bin"}
	}
	data, bank := prepareFixture(version, group, bad)
	prepWrite(t, filepath.Join(folder, "data.win"), data)
	if group != 0 {
		prepWrite(t, filepath.Join(folder, "audiogroup1.dat"), bank)
		prepWrite(t, filepath.Join(folder, "lang/lang_en.json"), []byte("{}"))
	}
	prepWrite(t, filepath.Join(source, "private-notes.txt"), []byte("original extras"))
	return root, source, names, data
}

func TestInstallGamesKeepAndDiscard(t *testing.T) {
	for _, game := range []string{"undertale", "deltarune"} {
		for _, discard := range []bool{false, true} {
			t.Run(game+map[bool]string{false: "/keep", true: "/discard"}[discard], func(t *testing.T) {
				root, source, names, data := installFixture(t, game, false)
				common := filepath.Dir(source)
				sentinels := []string{"Saves/butterscotch/common/save", "Cores/butterscotch/core.rbf", "Assets/butterscotch/common/other-game/file", "Assets/butterscotch/common/runner.elf"}
				for _, p := range sentinels {
					prepWrite(t, filepath.Join(root, p), []byte("keep"))
				}
				otherNames := []string{"deltarune.win", "dr_textures.bin", "dr_music.bin"}
				if game == "deltarune" {
					otherNames = []string{"data.win", "textures.bin", "music.bin"}
				}
				for _, n := range otherNames {
					prepWrite(t, filepath.Join(common, n), []byte("other game"))
				}
				result, err := Install(InstallOptions{Root: root, Game: game, DiscardOriginals: discard})
				if err != nil {
					t.Fatal(err)
				}
				if result.OriginalsDiscarded != discard || result.Warning != "" {
					t.Fatalf("result: %+v", result)
				}
				got, err := os.ReadFile(filepath.Join(common, names[0]))
				if err != nil || !bytes.Equal(got, data) {
					t.Fatalf("installed data: %v", err)
				}
				for _, n := range names[1:] {
					got, err := os.ReadFile(filepath.Join(common, n))
					if err != nil || len(got) == 0 {
						t.Fatalf("missing %s: %v", n, err)
					}
				}
				if _, err := os.Stat(source); discard != os.IsNotExist(err) {
					t.Fatalf("original source state: %v", err)
				}
				for _, p := range sentinels {
					got, err := os.ReadFile(filepath.Join(root, p))
					if err != nil || string(got) != "keep" {
						t.Fatalf("changed %s", p)
					}
				}
				for _, n := range otherNames {
					got, _ := os.ReadFile(filepath.Join(common, n))
					if string(got) != "other game" {
						t.Fatalf("changed %s", n)
					}
				}
				report, err := os.ReadFile(filepath.Join(common, ".butterscotch-"+game+".json"))
				if err != nil || strings.Contains(string(report), root) {
					t.Fatalf("report: %v", err)
				}
				if !discard {
					if _, err := Install(InstallOptions{Root: root, Game: game}); err != nil {
						t.Fatalf("repeat: %v", err)
					}
				}
				entries, _ := os.ReadDir(common)
				for _, e := range entries {
					if strings.HasPrefix(e.Name(), ".butterscotch-install-") {
						t.Fatal("temporary folder remains")
					}
				}
			})
		}
	}
}

func TestInstallConversionFailurePreservesOriginalsAndInstalledSet(t *testing.T) {
	root, source, names, _ := installFixture(t, "undertale", true)
	for _, n := range names {
		prepWrite(t, filepath.Join(filepath.Dir(source), n), []byte("old "+n))
	}
	if _, err := Install(InstallOptions{Root: root, Game: "undertale", DiscardOriginals: true}); err == nil {
		t.Fatal("invalid audio accepted")
	}
	for _, n := range names {
		got, _ := os.ReadFile(filepath.Join(filepath.Dir(source), n))
		if string(got) != "old "+n {
			t.Fatalf("changed %s", n)
		}
	}
	if _, err := os.Stat(filepath.Join(source, "data.win")); err != nil {
		t.Fatal(err)
	}
}

func TestInstallRejectsUnsafePaths(t *testing.T) {
	for _, where := range []string{"Assets", "Assets/butterscotch", "Assets/butterscotch/common", "source", "nested", "target", "directory-target"} {
		t.Run(where, func(t *testing.T) {
			root, source, _, _ := installFixture(t, "undertale", false)
			outside := t.TempDir()
			prepWrite(t, filepath.Join(outside, "keep"), []byte("untouched"))
			switch where {
			case "source":
				if err := os.Rename(source, filepath.Join(outside, "originals")); err != nil {
					t.Fatal(err)
				}
				if err := os.Symlink(filepath.Join(outside, "originals"), source); err != nil {
					t.Skip(err)
				}
			case "nested":
				if err := os.Symlink(outside, filepath.Join(source, "redirect")); err != nil {
					t.Skip(err)
				}
			case "target":
				if err := os.Symlink(filepath.Join(outside, "keep"), filepath.Join(filepath.Dir(source), "textures.bin")); err != nil {
					t.Skip(err)
				}
			case "directory-target":
				if err := os.Mkdir(filepath.Join(filepath.Dir(source), "textures.bin"), 0755); err != nil {
					t.Fatal(err)
				}
			default:
				p := filepath.Join(root, where)
				moved := filepath.Join(outside, "moved")
				if err := os.Rename(p, moved); err != nil {
					t.Fatal(err)
				}
				if err := os.Symlink(moved, p); err != nil {
					t.Skip(err)
				}
			}
			if _, err := Install(InstallOptions{Root: root, Game: "undertale", DiscardOriginals: true}); err == nil {
				t.Fatal("unsafe path accepted")
			}
			got, _ := os.ReadFile(filepath.Join(outside, "keep"))
			if string(got) != "untouched" {
				t.Fatal("followed redirect")
			}
		})
	}
}

func TestInstallRollsBackReplacementFailure(t *testing.T) {
	for _, rollbackFails := range []bool{false, true} {
		t.Run(map[bool]string{false: "restored", true: "retained-backup"}[rollbackFails], func(t *testing.T) {
			root, source, names, _ := installFixture(t, "undertale", false)
			common := filepath.Dir(source)
			for _, n := range names {
				prepWrite(t, filepath.Join(common, n), []byte("old "+n))
			}
			rename := func(from, to string) error {
				if filepath.Base(from) == "music.bin" && strings.Contains(from, string(filepath.Separator)+"ready"+string(filepath.Separator)) {
					return errors.New("injected install failure")
				}
				if rollbackFails && strings.Contains(from, string(filepath.Separator)+"backup"+string(filepath.Separator)) && filepath.Base(from) == "textures.bin" {
					return errors.New("injected rollback failure")
				}
				return os.Rename(from, to)
			}
			_, err := installWithOps(InstallOptions{Root: root, Game: "undertale", DiscardOriginals: true}, rename, os.RemoveAll)
			if err == nil {
				t.Fatal("injected failure succeeded")
			}
			if _, err := os.Stat(source); err != nil {
				t.Fatal("originals removed")
			}
			if rollbackFails {
				matches, _ := filepath.Glob(filepath.Join(common, ".butterscotch-install-*", "backup", "textures.bin"))
				if len(matches) != 1 {
					t.Fatalf("backup not reported and preserved: %v %v", matches, err)
				}
				location, resolveErr := canonicalPath(filepath.Dir(matches[0]))
				if resolveErr != nil || !strings.Contains(err.Error(), location) {
					t.Fatalf("backup not reported and preserved: %v %v (%v)", matches, err, resolveErr)
				}
				got, _ := os.ReadFile(matches[0])
				if string(got) != "old textures.bin" {
					t.Fatal("backup corrupted")
				}
			} else {
				for _, n := range names {
					got, _ := os.ReadFile(filepath.Join(common, n))
					if string(got) != "old "+n {
						t.Fatalf("rollback changed %s", n)
					}
				}
			}
		})
	}
}

func TestInstallDiscardFailureIsWarning(t *testing.T) {
	root, source, names, _ := installFixture(t, "undertale", false)
	result, err := installWithOps(InstallOptions{Root: root, Game: "undertale", DiscardOriginals: true}, os.Rename, func(string) error { return errors.New("cannot delete originals") })
	if err != nil || result.Warning == "" || result.OriginalsDiscarded {
		t.Fatalf("result %+v: %v", result, err)
	}
	for _, n := range names {
		if _, err := os.Stat(filepath.Join(filepath.Dir(source), n)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := os.Stat(source); err != nil {
		t.Fatal(err)
	}
}

func TestInstallEveryRenameFailureRestoresCompletePreviousSet(t *testing.T) {
	// Each file has a backup rename and an installation rename. Fail each one
	// independently, after any earlier successful replacements.
	for failAt := 1; failAt <= 8; failAt++ {
		t.Run(fmt.Sprint(failAt), func(t *testing.T) {
			root, source, names, _ := installFixture(t, "undertale", false)
			common := filepath.Dir(source)
			names = append(names, ".butterscotch-undertale.json")
			for _, n := range names {
				prepWrite(t, filepath.Join(common, n), []byte("old "+n))
			}
			calls := 0
			rename := func(from, to string) error {
				calls++
				if calls == failAt {
					return errors.New("injected rename failure")
				}
				return os.Rename(from, to)
			}
			if _, err := installWithOps(InstallOptions{Root: root, Game: "undertale", DiscardOriginals: true}, rename, os.RemoveAll); err == nil {
				t.Fatal("failed rename accepted")
			}
			for _, n := range names {
				got, _ := os.ReadFile(filepath.Join(common, n))
				if string(got) != "old "+n {
					t.Fatalf("lost previous %s", n)
				}
			}
			if _, err := os.Stat(source); err != nil {
				t.Fatal("lost originals")
			}
		})
	}
}

func TestInstallVerificationFailureRemovesNewPartialSet(t *testing.T) {
	root, source, names, _ := installFixture(t, "undertale", false)
	common := filepath.Dir(source)
	rename := func(from, to string) error {
		if err := os.Rename(from, to); err != nil {
			return err
		}
		if filepath.Base(to) == "music.bin" {
			return os.WriteFile(to, []byte("corrupted write"), 0644)
		}
		return nil
	}
	if _, err := installWithOps(InstallOptions{Root: root, Game: "undertale", DiscardOriginals: true}, rename, os.RemoveAll); err == nil || !strings.Contains(err.Error(), "verification") {
		t.Fatalf("verification: %v", err)
	}
	for _, n := range append(names, ".butterscotch-undertale.json") {
		if _, err := os.Lstat(filepath.Join(common, n)); !os.IsNotExist(err) {
			t.Fatalf("left failed output %s: %v", n, err)
		}
	}
	if _, err := os.Stat(source); err != nil {
		t.Fatal("lost originals")
	}
}

func TestInstallProgressDoesNotAnnouncePreparationAsInstallationSuccess(t *testing.T) {
	root, _, _, _ := installFixture(t, "undertale", false)
	var events []string
	_, err := Install(InstallOptions{Root: root, Game: "undertale", Progress: func(s string) { events = append(events, s) }})
	if err != nil {
		t.Fatal(err)
	}
	if len(events) == 0 || events[len(events)-1] != "Installation complete" {
		t.Fatalf("progress: %v", events)
	}
	for _, s := range events {
		if s == "Preparation complete" {
			t.Fatalf("announced success before installation: %v", events)
		}
	}
}

func TestInstallAcceptsCardRootAlias(t *testing.T) {
	root, source, names, _ := installFixture(t, "undertale", false)
	alias := filepath.Join(t.TempDir(), "card")
	if err := os.Symlink(root, alias); err != nil {
		t.Skip(err)
	}
	if _, err := Install(InstallOptions{Root: alias, Game: "undertale"}); err != nil {
		t.Fatal(err)
	}
	for _, name := range names {
		if _, err := os.Stat(filepath.Join(filepath.Dir(source), name)); err != nil {
			t.Fatal(err)
		}
	}
}

func TestInstallFileHashUsesBoundedMemory(t *testing.T) {
	// A full read of a real pack would allocate its entire size. Verification
	// must stay bounded even when the pack is much larger than a copy buffer.
	data := bytes.Repeat([]byte("pack bytes"), 1<<20)
	path := filepath.Join(t.TempDir(), "music.bin")
	prepWrite(t, path, data)
	want := hash(data)
	runtime.GC()
	var before, after runtime.MemStats
	runtime.ReadMemStats(&before)
	got, err := installFileHash(path)
	runtime.ReadMemStats(&after)
	if err != nil || got != want {
		t.Fatalf("hash %q: %v", got, err)
	}
	if allocated := after.TotalAlloc - before.TotalAlloc; allocated > 1<<20 {
		t.Fatalf("verification allocated %d bytes for a %d-byte file", allocated, len(data))
	}
}
