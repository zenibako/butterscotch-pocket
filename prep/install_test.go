package prep

import (
	"bytes"
	"encoding/binary"
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
	version, group := byte(16), uint32(0)
	names := []string{"data.win", "textures.bin", "music.bin"}
	if game == "deltarune" {
		version, group = 17, 1
		names = []string{"deltarune.win", "dr_textures.bin", "dr_music.bin"}
	}
	data, bank := prepareFixture(version, group, bad)
	source := filepath.Join(common, names[0])
	prepWrite(t, source, data)
	if game == "deltarune" {
		prepWrite(t, filepath.Join(common, "audiogroup1.dat"), bank)
		prepWrite(t, filepath.Join(common, "lang/lang_en.json"), []byte("{}"))
	}
	prepWrite(t, filepath.Join(common, "private-notes.txt"), []byte("keep"))
	return root, source, names, data
}

func addInstallMusic(t *testing.T, source, name string) {
	t.Helper()
	b, err := os.ReadFile(source)
	if err != nil {
		t.Fatal(err)
	}
	payload := append([]byte(name), 0)
	b = append(b, []byte("STRG")...)
	size := make([]byte, 4)
	binary.LittleEndian.PutUint32(size, uint32(len(payload)))
	b = append(b, size...)
	b = append(b, payload...)
	binary.LittleEndian.PutUint32(b[4:], uint32(len(b)-8))
	prepWrite(t, source, b)
	musicDir := "music"
	if filepath.Base(source) == "deltarune.win" {
		musicDir = "mus"
	}
	prepWrite(t, filepath.Join(filepath.Dir(source), musicDir, name), audioWAV(8, 1, 32000, []byte{128, 130, 126, 128}))
}

func TestInstallFlatGamesKeepAndDiscard(t *testing.T) {
	for _, game := range []string{"undertale", "deltarune"} {
		for _, discard := range []bool{false, true} {
			t.Run(fmt.Sprintf("%s/discard=%v", game, discard), func(t *testing.T) {
				root, source, names, _ := installFixture(t, game, false)
				common := filepath.Dir(source)
				addInstallMusic(t, source, "selected.ogg")
				original, _ := os.ReadFile(source)
				prepWrite(t, filepath.Join(common, "music/unclaimed.ogg"), audioWAV(8, 1, 32000, []byte{128, 130}))
				sentinels := []string{"Saves/butterscotch/common/save", "Cores/butterscotch/core.rbf", "Assets/butterscotch/common/runner.elf", "Assets/butterscotch/common/private-notes.txt"}
				for _, p := range sentinels {
					prepWrite(t, filepath.Join(root, p), []byte("keep"))
				}
				otherNames := []string{"deltarune.win", "dr_textures.bin", "dr_music.bin"}
				otherVersion := byte(17)
				if game == "deltarune" {
					otherNames = []string{"data.win", "textures.bin", "music.bin"}
					otherVersion = 16
				}
				other, _ := prepareFixture(otherVersion, 0, false)
				prepWrite(t, filepath.Join(common, otherNames[0]), other)
				for _, name := range otherNames[1:] {
					prepWrite(t, filepath.Join(common, name), []byte("other"))
				}
				result, err := Install(InstallOptions{Root: root, Game: game, DiscardOriginals: discard})
				if err != nil {
					t.Fatal(err)
				}
				if result.OriginalsDiscarded != discard || result.Warning != "" {
					t.Fatalf("result %+v", result)
				}
				got, _ := os.ReadFile(source)
				if !bytes.Equal(got, original) {
					t.Fatal("required game data changed")
				}
				for _, name := range names[1:] {
					got, err := os.ReadFile(filepath.Join(common, name))
					if err != nil || len(got) == 0 {
						t.Fatalf("pack %s: %v", name, err)
					}
				}
				musicDir := "music"
				if game == "deltarune" {
					musicDir = "mus"
				}
				if _, err := os.Stat(filepath.Join(common, musicDir, "selected.ogg")); discard != os.IsNotExist(err) {
					t.Fatalf("selected original: %v", err)
				}
				if _, err := os.Stat(filepath.Join(common, "music/unclaimed.ogg")); err != nil {
					t.Fatal("unclaimed music removed")
				}
				if game == "deltarune" {
					for _, p := range []string{"audiogroup1.dat", "lang/lang_en.json"} {
						_, err := os.Stat(filepath.Join(common, p))
						if discard != os.IsNotExist(err) {
							t.Fatalf("support %s: %v", p, err)
						}
					}
				}
				for _, p := range sentinels {
					got, _ := os.ReadFile(filepath.Join(root, p))
					if string(got) != "keep" {
						t.Fatalf("changed %s", p)
					}
				}
				got, _ = os.ReadFile(filepath.Join(common, otherNames[0]))
				if !bytes.Equal(got, other) {
					t.Fatal("changed other data")
				}
				for _, n := range otherNames[1:] {
					got, _ := os.ReadFile(filepath.Join(common, n))
					if string(got) != "other" {
						t.Fatal("changed other pack")
					}
				}
				report, err := os.ReadFile(filepath.Join(common, ".butterscotch-"+game+".json"))
				if err != nil || strings.Contains(string(report), root) {
					t.Fatalf("report %v", err)
				}
				if !discard {
					if _, err := Install(InstallOptions{Root: root, Game: game}); err != nil {
						t.Fatal(err)
					}
				}
				matches, _ := filepath.Glob(filepath.Join(filepath.Dir(common), ".butterscotch-install-*"))
				if len(matches) > 0 {
					t.Fatal("work remains", matches)
				}
			})
		}
	}
}

func TestInstallDiscardRetainsSharedAndChangedAudio(t *testing.T) {
	for _, other := range []string{"reference", "invalid", "absent"} {
		t.Run(other, func(t *testing.T) {
			root, source, _, _ := installFixture(t, "undertale", false)
			common := filepath.Dir(source)
			addInstallMusic(t, source, "shared.ogg")
			if other != "absent" {
				data := []byte("invalid")
				if other == "reference" {
					data, _ = prepareFixture(17, 0, false)
					data = append(data, []byte("STRG\x07\x00\x00\x00SHARED\x00")...)
					binary.LittleEndian.PutUint32(data[4:], uint32(len(data)-8))
				}
				prepWrite(t, filepath.Join(common, "deltarune.win"), data)
			}
			options := InstallOptions{Root: root, Game: "undertale", DiscardOriginals: true}
			if other == "absent" {
				options.Progress = func(s string) {
					if s == "Converting textures" {
						prepWrite(t, filepath.Join(common, "music/shared.ogg"), audioWAV(8, 1, 32000, []byte{128, 128, 128}))
					}
				}
			}
			result, err := Install(options)
			if err != nil {
				t.Fatal(err)
			}
			if _, err := os.Stat(filepath.Join(common, "music/shared.ogg")); err != nil {
				t.Fatal("shared or changed audio removed")
			}
			if other == "absent" && (result.Warning == "" || result.OriginalsDiscarded) {
				t.Fatalf("changed input not reported %+v", result)
			}
		})
	}
}

func TestInstallConversionFailurePreservesInputsAndPacks(t *testing.T) {
	root, source, names, data := installFixture(t, "undertale", true)
	for _, n := range names[1:] {
		prepWrite(t, filepath.Join(filepath.Dir(source), n), []byte("old "+n))
	}
	if _, err := Install(InstallOptions{Root: root, Game: "undertale", DiscardOriginals: true}); err == nil {
		t.Fatal("bad audio accepted")
	}
	got, _ := os.ReadFile(source)
	if !bytes.Equal(got, data) {
		t.Fatal("source changed")
	}
	for _, n := range names[1:] {
		got, _ := os.ReadFile(filepath.Join(filepath.Dir(source), n))
		if string(got) != "old "+n {
			t.Fatal("old pack changed")
		}
	}
}

func TestInstallRejectsUnsafePaths(t *testing.T) {
	for _, where := range []string{"Assets", "Assets/butterscotch", "Assets/butterscotch/common", "source", "nested", "target", "directory-target"} {
		t.Run(where, func(t *testing.T) {
			root, source, _, _ := installFixture(t, "undertale", false)
			common := filepath.Dir(source)
			outside := t.TempDir()
			prepWrite(t, filepath.Join(outside, "keep"), []byte("keep"))
			switch where {
			case "source":
				if err := os.Rename(source, filepath.Join(outside, "data.win")); err != nil {
					t.Fatal(err)
				}
				if err := os.Symlink(filepath.Join(outside, "data.win"), source); err != nil {
					t.Skip(err)
				}
			case "nested":
				if err := os.Symlink(outside, filepath.Join(common, "music")); err != nil {
					t.Skip(err)
				}
			case "target":
				if err := os.Symlink(filepath.Join(outside, "keep"), filepath.Join(common, "textures.bin")); err != nil {
					t.Skip(err)
				}
			case "directory-target":
				if err := os.Mkdir(filepath.Join(common, "textures.bin"), 0755); err != nil {
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
			if string(got) != "keep" {
				t.Fatal("redirect followed")
			}
		})
	}
}

func TestInstallEveryRenameFailureRestoresCompletePreviousSet(t *testing.T) {
	for failAt := 1; failAt <= 8; failAt++ {
		t.Run(fmt.Sprint(failAt), func(t *testing.T) {
			root, source, names, data := installFixture(t, "undertale", false)
			common := filepath.Dir(source)
			names = append(names, ".butterscotch-undertale.json")
			for _, n := range names[1:] {
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
			if _, err := installWithOps(InstallOptions{Root: root, Game: "undertale", DiscardOriginals: true}, rename, os.Remove); err == nil {
				t.Fatal("failure accepted")
			}
			got, _ := os.ReadFile(source)
			if !bytes.Equal(got, data) {
				t.Fatal("required source lost")
			}
			for _, n := range names[1:] {
				got, _ := os.ReadFile(filepath.Join(common, n))
				if string(got) != "old "+n {
					t.Fatalf("lost %s", n)
				}
			}
		})
	}
}

func TestInstallRollbackFailurePreservesRecoveryBackup(t *testing.T) {
	root, source, _, _ := installFixture(t, "undertale", false)
	common := filepath.Dir(source)
	prepWrite(t, filepath.Join(common, "textures.bin"), []byte("old"))
	rename := func(from, to string) error {
		if filepath.Base(from) == "music.bin" && strings.Contains(from, string(filepath.Separator)+"ready"+string(filepath.Separator)) {
			return errors.New("install failure")
		}
		if filepath.Base(from) == "textures.bin" && filepath.Base(filepath.Dir(from)) == "backup" {
			return errors.New("rollback failure")
		}
		return os.Rename(from, to)
	}
	_, err := installWithOps(InstallOptions{Root: root, Game: "undertale", DiscardOriginals: true}, rename, os.Remove)
	matches, _ := filepath.Glob(filepath.Join(filepath.Dir(common), ".butterscotch-install-*", "backup", "textures.bin"))
	if err == nil || len(matches) != 1 || !strings.Contains(err.Error(), filepath.Dir(matches[0])) {
		t.Fatalf("backup %v error %v", matches, err)
	}
	got, _ := os.ReadFile(matches[0])
	if string(got) != "old" {
		t.Fatal("backup lost")
	}
}

func TestInstallDiscardFailureIsWarning(t *testing.T) {
	root, source, _, _ := installFixture(t, "deltarune", false)
	result, err := installWithOps(InstallOptions{Root: root, Game: "deltarune", DiscardOriginals: true}, os.Rename, func(string) error { return errors.New("cannot remove") })
	if err != nil || result.Warning == "" || result.OriginalsDiscarded {
		t.Fatalf("result %+v: %v", result, err)
	}
	if _, err := os.Stat(source); err != nil {
		t.Fatal(err)
	}
}

func TestInstallAcceptsCardRootAlias(t *testing.T) {
	root, _, _, _ := installFixture(t, "undertale", false)
	alias := filepath.Join(t.TempDir(), "card")
	if err := os.Symlink(root, alias); err != nil {
		t.Skip(err)
	}
	if _, err := Install(InstallOptions{Root: alias, Game: "undertale"}); err != nil {
		t.Fatal(err)
	}
}

func TestInstallFileHashUsesBoundedMemory(t *testing.T) {
	data := bytes.Repeat([]byte("pack bytes"), 1<<20)
	path := filepath.Join(t.TempDir(), "pack")
	prepWrite(t, path, data)
	want := hash(data)
	runtime.GC()
	var before, after runtime.MemStats
	runtime.ReadMemStats(&before)
	got, err := installFileHash(path)
	runtime.ReadMemStats(&after)
	if err != nil || got != want {
		t.Fatalf("hash: %v", err)
	}
	if allocated := after.TotalAlloc - before.TotalAlloc; allocated > 1<<20 {
		t.Fatalf("allocated %d bytes", allocated)
	}
}

func TestInstallProgressDoesNotAnnouncePreparationSuccess(t *testing.T) {
	root, _, _, _ := installFixture(t, "undertale", false)
	var events []string
	if _, err := Install(InstallOptions{Root: root, Game: "undertale", Progress: func(s string) { events = append(events, s) }}); err != nil {
		t.Fatal(err)
	}
	if len(events) == 0 || events[len(events)-1] != "Installation complete" {
		t.Fatal(events)
	}
	for _, s := range events {
		if s == "Preparation complete" {
			t.Fatal(events)
		}
	}
}

func TestInstallDiscoverySeparatesFactoryAudioWithCollidingNames(t *testing.T) {
	root, drSource, _, _ := installFixture(t, "deltarune", false)
	common := filepath.Dir(drSource)
	data, _ := prepareFixture(16, 0, false)
	utSource := filepath.Join(common, "data.win")
	prepWrite(t, utSource, data)
	addInstallMusic(t, utSource, "shared.ogg")
	if err := os.Rename(filepath.Join(common, "music/shared.ogg"), filepath.Join(common, "shared.ogg")); err != nil {
		t.Fatal(err)
	}
	addInstallMusic(t, drSource, "shared.ogg")
	prepWrite(t, filepath.Join(common, "mus/shared.ogg"), audioWAV(8, 1, 32000, []byte{140, 160, 120, 130}))

	for _, game := range []string{"undertale", "deltarune"} {
		source, want := utSource, filepath.Join(common, "shared.ogg")
		if game == "deltarune" {
			source, want = drSource, filepath.Join(common, "mus/shared.ogg")
		}
		d, err := discoverBounded(game, source, common)
		if err != nil {
			t.Fatalf("%s discovery: %v", game, err)
		}
		if d.external["shared.ogg"] != want {
			t.Fatalf("%s selected %s, want %s", game, d.external["shared.ogg"], want)
		}
		if _, err := Install(InstallOptions{Root: root, Game: game, DiscardOriginals: true}); err != nil {
			t.Fatalf("%s installation: %v", game, err)
		}
	}
	for _, path := range []string{"shared.ogg", "mus/shared.ogg"} {
		if _, err := os.Stat(filepath.Join(common, path)); err != nil {
			t.Fatalf("shared raw file lost %s: %v", path, err)
		}
	}
}

func TestInstallBoundedDiscoveryPrefersGameDirectoryAndIgnoresParent(t *testing.T) {
	root, source, _, _ := installFixture(t, "undertale", false)
	common := filepath.Dir(source)
	addInstallMusic(t, source, "preferred.ogg")
	prepWrite(t, filepath.Join(common, "preferred.ogg"), audioWAV(8, 1, 32000, []byte{140, 150}))
	prepWrite(t, filepath.Join(filepath.Dir(common), "mus/outside.ogg"), audioWAV(8, 1, 32000, []byte{128, 130}))
	d, err := discoverBounded("undertale", source, common)
	if err != nil {
		t.Fatal(err)
	}
	if d.external["preferred.ogg"] != filepath.Join(common, "music/preferred.ogg") {
		t.Fatalf("wrong priority: %v", d.external)
	}
	if _, exists := d.external["outside.ogg"]; exists {
		t.Fatal("discovered music outside common")
	}
	if _, err := Install(InstallOptions{Root: root, Game: "undertale", DiscardOriginals: true}); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(common, "preferred.ogg")); err != nil {
		t.Fatal("unconsumed fallback deleted")
	}
}

func TestInstallVerificationFailureRestoresRequiredDataAndRemovesNewPacks(t *testing.T) {
	root, source, names, data := installFixture(t, "undertale", false)
	common := filepath.Dir(source)
	rename := func(from, to string) error {
		if err := os.Rename(from, to); err != nil {
			return err
		}
		if filepath.Base(to) == "music.bin" {
			return os.WriteFile(to, []byte("corrupted"), 0644)
		}
		return nil
	}
	if _, err := installWithOps(InstallOptions{Root: root, Game: "undertale", DiscardOriginals: true}, rename, os.Remove); err == nil || !strings.Contains(err.Error(), "verification") {
		t.Fatalf("verification error: %v", err)
	}
	got, _ := os.ReadFile(source)
	if !bytes.Equal(got, data) {
		t.Fatal("required input lost")
	}
	for _, name := range append(names[1:], ".butterscotch-undertale.json") {
		if _, err := os.Lstat(filepath.Join(common, name)); !os.IsNotExist(err) {
			t.Fatalf("failed output %s remains", name)
		}
	}
}

func TestInstallDiscardRetainsChangedSupportFiles(t *testing.T) {
	root, source, _, _ := installFixture(t, "deltarune", false)
	common := filepath.Dir(source)
	changed := []byte("new support bytes")
	result, err := Install(InstallOptions{Root: root, Game: "deltarune", DiscardOriginals: true, Progress: func(event string) {
		if event == "Converting textures" {
			prepWrite(t, filepath.Join(common, "audiogroup1.dat"), changed)
		}
	}})
	if err != nil || result.OriginalsDiscarded || !strings.Contains(result.Warning, "changed") {
		t.Fatalf("result %+v: %v", result, err)
	}
	got, _ := os.ReadFile(filepath.Join(common, "audiogroup1.dat"))
	if !bytes.Equal(got, changed) {
		t.Fatal("changed support removed")
	}
}

func TestInstallDiscardRejectsRedirectIntroducedDuringConversion(t *testing.T) {
	// Check platform support before changing the live conversion fixture.
	probeDir := t.TempDir()
	probe := filepath.Join(probeDir, "symlink-probe")
	if err := os.Symlink(probeDir, probe); err != nil {
		t.Skipf("symlinks unavailable on this host: %v", err)
	}
	if err := os.Remove(probe); err != nil {
		t.Fatal(err)
	}
	root, source, _, _ := installFixture(t, "undertale", false)
	common := filepath.Dir(source)
	addInstallMusic(t, source, "selected.ogg")
	outside := t.TempDir()
	saved := filepath.Join(outside, "original-music")
	result, err := Install(InstallOptions{Root: root, Game: "undertale", DiscardOriginals: true, Progress: func(event string) {
		if event == "Conversion complete" {
			if err := os.Rename(filepath.Join(common, "music"), saved); err != nil {
				t.Fatal(err)
			}
			if err := os.Symlink(saved, filepath.Join(common, "music")); err != nil {
				t.Fatal(err)
			}
		}
	}})
	if err != nil || result.Warning == "" || result.OriginalsDiscarded {
		t.Fatalf("result %+v: %v", result, err)
	}
	if _, err := os.Stat(filepath.Join(saved, "selected.ogg")); err != nil {
		t.Fatal("followed redirect while discarding")
	}
}

func TestInstallDiscardDoesNotClaimSubstringNamedAudio(t *testing.T) {
	root, source, _, _ := installFixture(t, "undertale", false)
	common := filepath.Dir(source)
	addInstallMusic(t, source, "mus_selected.ogg")
	unknown := filepath.Join(common, "music/selected.ogg")
	original := audioWAV(8, 1, 32000, []byte{129, 128, 130})
	prepWrite(t, unknown, original)
	if _, err := Install(InstallOptions{Root: root, Game: "undertale", DiscardOriginals: true}); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(unknown)
	if err != nil || !bytes.Equal(got, original) {
		t.Fatalf("unclaimed substring-named input deleted: %v", err)
	}
	if _, err := os.Stat(filepath.Join(common, "music/mus_selected.ogg")); !os.IsNotExist(err) {
		t.Fatalf("owned exact filename not discarded: %v", err)
	}
}

func TestInstallAudioOwnershipRequiresWholeFilename(t *testing.T) {
	for _, test := range []struct {
		data string
		want bool
	}{
		{"selected.ogg\x00", true},
		{"\x00music/selected.ogg\x00", true},
		{"\x00music\\selected.ogg\x00", true},
		{"\x00mus_selected.ogg\x00", false},
		{"\x00selected.ogg.backup\x00", false},
		{"\x00selected.ogg", false},
		{"\x00mus_selected.ogg\x00selected.ogg\x00", true},
	} {
		if got := installOwnsAudio([]byte(test.data), "selected.ogg"); got != test.want {
			t.Errorf("%q ownership %v want %v", test.data, got, test.want)
		}
	}
}
