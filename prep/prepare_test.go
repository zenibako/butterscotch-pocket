package prep

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func prepareFixture(version byte, group uint32, bad bool) ([]byte, []byte) {
	b := []byte("FORM\x00\x00\x00\x00")
	put := func(p int, v uint32) { binary.LittleEndian.PutUint32(b[p:], v) }
	add := func(name string, p []byte) int {
		o := len(b)
		b = append(b, []byte(name)...)
		b = append(b, make([]byte, 4)...)
		put(o+4, uint32(len(p)))
		b = append(b, p...)
		return o + 8
	}
	add("GEN8", []byte{0, version, 0, 0, 0, 0, 0, 0})
	p := add("TXTR", append(make([]byte, 16), qoiFixture(1, 1, 0xff, 255, 0, 0, 255)...))
	put(p, 1)
	put(p+4, uint32(p+8))
	put(p+12, uint32(p+16))
	s := add("SOND", append(make([]byte, 44), []byte("tone\x00")...))
	put(s, 1)
	put(s+4, uint32(s+8))
	put(s+8, uint32(s+44))
	put(s+12, 3)
	put(s+36, group)
	wav := audioWAV(8, 1, 32000, []byte{128, 130, 126, 128})
	if bad {
		wav = []byte("broken")
	}
	bank := []byte("FORM\x00\x00\x00\x00AUDO\x00\x00\x00\x00")
	bank = append(bank, make([]byte, 12)...)
	binary.LittleEndian.PutUint32(bank[16:], 1)
	binary.LittleEndian.PutUint32(bank[20:], 24)
	binary.LittleEndian.PutUint32(bank[24:], uint32(len(wav)))
	bank = append(bank, wav...)
	binary.LittleEndian.PutUint32(bank[4:], uint32(len(bank)-8))
	binary.LittleEndian.PutUint32(bank[12:], uint32(len(bank)-16))
	if group == 0 {
		a := add("AUDO", append(make([]byte, 12), wav...))
		put(a, 1)
		put(a+4, uint32(a+8))
		put(a+8, uint32(len(wav)))
	} else {
		add("AUDO", make([]byte, 4))
	}
	put(4, uint32(len(b)-8))
	return b, bank
}
func prepWrite(t *testing.T, path string, b []byte) {
	t.Helper()
	if e := os.MkdirAll(filepath.Dir(path), 0755); e != nil {
		t.Fatal(e)
	}
	if e := os.WriteFile(path, b, 0644); e != nil {
		t.Fatal(e)
	}
}
func TestPrepareCompleteAndDiscovery(t *testing.T) {
	for _, layout := range []string{"direct", "app", "linux", "chapter"} {
		t.Run(layout, func(t *testing.T) {
			r := t.TempDir()
			src := filepath.Join(r, "game 雪")
			folder := src
			name := "data.win"
			game := "undertale"
			v := byte(16)
			g := uint32(0)
			if layout == "app" {
				folder = filepath.Join(src, "Contents/Resources")
				name = "game.ios"
			}
			if layout == "linux" {
				folder = filepath.Join(src, "assets")
				name = "game.unx"
			}
			if layout == "chapter" {
				folder = filepath.Join(src, "chapter1_linux")
				game = "deltarune"
				v = 17
				g = 1
			}
			b, bank := prepareFixture(v, g, false)
			prepWrite(t, filepath.Join(folder, name), b)
			if g != 0 {
				prepWrite(t, filepath.Join(folder, "audiogroup1.dat"), bank)
				prepWrite(t, filepath.Join(folder, "lang/lang_en.json"), []byte("{}"))
			}
			if layout == "direct" {
				src = filepath.Join(folder, name)
			}
			out := filepath.Join(r, "ready 雪")
			got, e := Prepare(Options{Game: game, Source: src, Output: out})
			if e != nil {
				t.Fatal(e)
			}
			canonicalOut, _ := filepath.EvalSymlinks(out)
			if got != canonicalOut {
				t.Fatal(got)
			}
			prefix := ""
			dn := "data.win"
			if g != 0 {
				prefix = "dr_"
				dn = "deltarune.win"
			}
			c := filepath.Join(out, "Assets/butterscotch/common")
			original, _ := os.ReadFile(filepath.Join(c, dn))
			if !bytes.Equal(original, b) {
				t.Fatal("changed source")
			}
			tex, _ := os.ReadFile(filepath.Join(c, prefix+"textures.bin"))
			audio, _ := os.ReadFile(filepath.Join(c, prefix+"music.bin"))
			footer := audio[len(audio)-16:]
			start := int(binary.LittleEndian.Uint32(footer))
			if start%4096 != 0 || string(footer[12:]) != "UTXT" || !bytes.Equal(audio[start:len(audio)-16], tex) {
				t.Fatal("invalid appended pack")
			}
			manifest, _ := os.ReadFile(filepath.Join(out, "PREPARED-DATA.json"))
			var m map[string]any
			if json.Unmarshal(manifest, &m) != nil || m["game"] != game || strings.Contains(string(manifest), r) {
				t.Fatal(string(manifest))
			}
		})
	}
}
func TestPrepareRejectUnsafePaths(t *testing.T) {
	r := t.TempDir()
	src := filepath.Join(r, "source")
	b, _ := prepareFixture(16, 0, false)
	prepWrite(t, filepath.Join(src, "data.win"), b)
	existing := filepath.Join(r, "existing")
	prepWrite(t, filepath.Join(existing, "save"), []byte("keep"))
	for _, out := range []string{src, filepath.Join(src, "new"), existing, filepath.Join(r, "SaVeS/new")} {
		if _, e := Prepare(Options{Game: "undertale", Source: src, Output: out}); e == nil {
			t.Fatalf("accepted %s", out)
		}
	}
	t.Run("symlink", func(t *testing.T) {
		link := filepath.Join(r, "alias")
		if e := os.Symlink(src, link); e != nil {
			t.Skipf("symlinks unavailable on this host: %v", e)
		}
		if _, e := Prepare(Options{Game: "undertale", Source: src, Output: filepath.Join(link, "new/deep")}); e == nil {
			t.Fatal("accepted output through a source symlink")
		}
	})
	keep, _ := os.ReadFile(filepath.Join(existing, "save"))
	if string(keep) != "keep" {
		t.Fatal("save changed")
	}
}
func TestPrepareFailureCleanup(t *testing.T) {
	r := t.TempDir()
	src := filepath.Join(r, "source")
	b, _ := prepareFixture(16, 0, true)
	prepWrite(t, filepath.Join(src, "data.win"), b)
	out := filepath.Join(r, "ready")
	if _, e := Prepare(Options{Game: "undertale", Source: src, Output: out}); e == nil {
		t.Fatal("invalid audio succeeded")
	}
	entries, _ := os.ReadDir(r)
	if len(entries) != 1 {
		t.Fatal("failure left staging or output")
	}
}
func TestPrepareRejectIncompleteDiscovery(t *testing.T) {
	for _, which := range []string{"bytecode", "ambiguous", "chapter", "oversize"} {
		t.Run(which, func(t *testing.T) {
			r := t.TempDir()
			src := filepath.Join(r, "src")
			b, _ := prepareFixture(16, 0, false)
			if which == "bytecode" {
				b[17] = 17
			}
			prepWrite(t, filepath.Join(src, "data.win"), b)
			if which == "ambiguous" {
				prepWrite(t, filepath.Join(src, "game.unx"), b)
			}
			if which == "oversize" {
				f, e := os.OpenFile(filepath.Join(src, "data.win"), os.O_WRONLY, 0644)
				if e != nil {
					t.Fatal(e)
				}
				if e = f.Truncate(MaxInputBytes + 1); e != nil {
					t.Fatal(e)
				}
				f.Close()
			}
			game := "undertale"
			if which == "chapter" {
				game = "deltarune"
			}
			if _, e := Prepare(Options{Game: game, Source: src, Output: filepath.Join(r, "out")}); e == nil {
				t.Fatal("incomplete source accepted")
			}
		})
	}
}

func TestPrepareMusicRequirementsAndDuplicates(t *testing.T) {
	for _, which := range []string{"missing", "duplicate", "identical", "embedded"} {
		t.Run(which, func(t *testing.T) {
			r := t.TempDir()
			src := filepath.Join(r, "src")
			b, _ := prepareFixture(16, 0, false)
			c, _ := Parse(b)
			s, _ := c.Chunk("SOND")
			entry := int(binary.LittleEndian.Uint32(s[4:]))
			o := len(b)
			b = append(b, []byte("STRG\x0a\x00\x00\x00theme.ogg\x00")...)
			binary.LittleEndian.PutUint32(b[entry+12:], uint32(o+8))
			binary.LittleEndian.PutUint32(b[4:], uint32(len(b)-8))
			if which == "missing" {
				binary.LittleEndian.PutUint32(b[entry+4:], 0x64)
				binary.LittleEndian.PutUint32(b[entry+32:], 0xffffffff)
			}
			prepWrite(t, filepath.Join(src, "data.win"), b)
			if which == "duplicate" || which == "identical" {
				wav := audioWAV(8, 1, 32000, []byte{128, 128})
				prepWrite(t, filepath.Join(src, "theme.ogg"), wav)
				if which == "duplicate" {
					wav = []byte("different")
				}
				prepWrite(t, filepath.Join(src, "mus/THEME.OGG"), wav)
			}
			_, e := Prepare(Options{Game: "undertale", Source: src, Output: filepath.Join(r, "out")})
			if which == "missing" || which == "duplicate" {
				if e == nil {
					t.Fatal("incomplete or conflicting music accepted")
				}
			} else if e != nil {
				t.Fatal(e)
			}
		})
	}
}
func TestPrepareOutputAppearsDuringConversion(t *testing.T) {
	r := t.TempDir()
	src := filepath.Join(r, "src")
	b, _ := prepareFixture(16, 0, false)
	prepWrite(t, filepath.Join(src, "data.win"), b)
	out := filepath.Join(r, "out")
	_, e := Prepare(Options{Game: "undertale", Source: src, Output: out, Progress: func(string) { prepWrite(t, filepath.Join(out, "keep"), []byte("keep")) }})
	if e == nil {
		t.Fatal("replaced output")
	}
	keep, _ := os.ReadFile(filepath.Join(out, "keep"))
	if string(keep) != "keep" {
		t.Fatal("changed concurrent output")
	}
}
func TestPrepareRejectInvalidPacks(t *testing.T) {
	tex := append([]byte("UTX2"), make([]byte, 12)...)
	for _, b := range [][]byte{nil, []byte("UTM1\x00\x00\x00\x00")} {
		if _, e := validatePacks(tex, b, nil); e == nil {
			t.Fatal("accepted invalid pack")
		}
	}
}

func TestPrepareRejectIncompleteDeltarune(t *testing.T) {
	for _, which := range []string{"group", "language", "invalid-json", "empty-group", "two-chapters"} {
		t.Run(which, func(t *testing.T) {
			r := t.TempDir()
			src := filepath.Join(r, "src")
			folder := src
			b, bank := prepareFixture(17, 1, false)
			if which == "two-chapters" {
				folder = filepath.Join(src, "chapter1_linux")
				if e := os.MkdirAll(filepath.Join(src, "chapter1_mac"), 0755); e != nil {
					t.Fatal(e)
				}
			}
			prepWrite(t, filepath.Join(folder, "data.win"), b)
			if which != "group" {
				if which == "empty-group" {
					bank = []byte("FORM\x0c\x00\x00\x00AUDO\x04\x00\x00\x00\x00\x00\x00\x00")
				}
				prepWrite(t, filepath.Join(folder, "audiogroup1.dat"), bank)
			}
			if which != "language" {
				lang := []byte("{}")
				if which == "invalid-json" {
					lang = []byte("invalid")
				}
				prepWrite(t, filepath.Join(folder, "lang/lang_en.json"), lang)
			}
			if _, e := Prepare(Options{Game: "deltarune", Source: src, Output: filepath.Join(r, "out")}); e == nil {
				t.Fatal("incomplete chapter accepted")
			}
		})
	}
}

func TestPrepareCaseInsensitiveSourceAncestor(t *testing.T) {
	root := t.TempDir()
	source := filepath.Join(root, "Source")
	data, _ := prepareFixture(16, 0, false)
	prepWrite(t, filepath.Join(source, "data.win"), data)
	a, err := os.Stat(source)
	if err != nil {
		t.Fatal(err)
	}
	b, err := os.Stat(filepath.Join(root, "source"))
	if err != nil || !os.SameFile(a, b) {
		t.Skip("case-sensitive filesystem")
	}
	out := filepath.Join(root, "source", "prepared")
	if _, err := Prepare(Options{Game: "undertale", Source: source, Output: out}); err == nil {
		t.Fatal("output inside source accepted through different casing")
	}
	if _, err := os.Stat(out); !os.IsNotExist(err) {
		t.Fatal("source installation was modified")
	}
}
