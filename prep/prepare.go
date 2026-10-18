package prep

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"unicode/utf8"
)

// Options selects a supported installation and a new output folder.
type Options struct {
	Game, Source, Output string
	Progress             func(string)
}

// canonicalPath resolves symlinks through the nearest existing ancestor, including
// for output paths whose final components do not yet exist.
func canonicalPath(p string) (string, error) {
	if p == "" {
		return "", fmt.Errorf("path is required")
	}
	if p == "~" || strings.HasPrefix(p, "~/") || strings.HasPrefix(p, "~\\") {
		home, e := os.UserHomeDir()
		if e != nil {
			return "", e
		}
		p = filepath.Join(home, p[1:])
	}
	p, e := filepath.Abs(p)
	if e != nil {
		return "", e
	}
	var suffix []string
	ancestor := p
	for {
		_, e = os.Lstat(ancestor)
		if e == nil {
			break
		}
		if !os.IsNotExist(e) {
			return "", e
		}
		parent := filepath.Dir(ancestor)
		if parent == ancestor {
			return "", e
		}
		suffix = append(suffix, filepath.Base(ancestor))
		ancestor = parent
	}
	resolved, e := filepath.EvalSymlinks(ancestor)
	if e != nil {
		return "", e
	}
	for i := len(suffix) - 1; i >= 0; i-- {
		resolved = filepath.Join(resolved, suffix[i])
	}
	return resolved, nil
}
func hasSaves(p string) bool {
	for _, part := range strings.FieldsFunc(p, func(r rune) bool { return r == '/' || r == '\\' }) {
		if strings.EqualFold(part, "saves") {
			return true
		}
	}
	return false
}
func inside(path, folder string) bool {
	r, e := filepath.Rel(folder, path)
	return e == nil && (r == "." || (r != ".." && !strings.HasPrefix(r, ".."+string(filepath.Separator))))
}
func hash(b []byte) string { s := sha256.Sum256(b); return hex.EncodeToString(s[:]) }

// validatePacks checks every emitted track, then adds the production texture tail.
func validatePacks(tex, audio []byte, expected map[string]bool) ([]byte, error) {
	if len(tex) < 16 || string(tex[:4]) != "UTX2" || len(audio) < 8 || string(audio[:4]) != "UTM1" {
		return nil, fmt.Errorf("converter produced an invalid texture or audio pack")
	}
	n := uint64(binary.LittleEndian.Uint32(audio[4:]))
	table := 8 + 48*n
	if n == 0 || table > uint64(len(audio)) {
		return nil, fmt.Errorf("audio pack has no valid tracks")
	}
	found := map[string]bool{}
	last := ""
	for i := uint64(0); i < n; i++ {
		p := int(8 + 48*i)
		field := audio[p : p+32]
		end := bytes.IndexByte(field, 0)
		if end < 0 || end == 0 || !utf8.Valid(field[:end]) {
			return nil, fmt.Errorf("invalid audio track name")
		}
		name := string(field[:end])
		if found[name] || (i > 0 && name <= last) {
			return nil, fmt.Errorf("duplicate or unsorted audio track %s", name)
		}
		found[name] = true
		last = name
		start := uint64(binary.LittleEndian.Uint32(audio[p+32:]))
		samples := uint64(binary.LittleEndian.Uint32(audio[p+36:]))
		rate := binary.LittleEndian.Uint32(audio[p+40:])
		size := samples
		if rate != 0 {
			if rate != 32000 {
				return nil, fmt.Errorf("unsupported audio sample rate")
			}
			size = (samples + 1) / 2
		}
		if samples == 0 || start < table || start+size > uint64(len(audio)) {
			return nil, fmt.Errorf("incomplete audio track %s", name)
		}
	}
	var missing []string
	for name := range expected {
		if !found[name] {
			missing = append(missing, name)
		}
	}
	if len(missing) > 0 {
		sort.Strings(missing)
		return nil, fmt.Errorf("missing audio tracks: %s", strings.Join(missing, ", "))
	}
	pad := (-len(audio)) & 4095
	start := len(audio) + pad
	if uint64(start)+uint64(len(tex))+16 > MaxInputBytes {
		return nil, fmt.Errorf("combined music and texture pack exceeds maximum size")
	}
	audio = append(audio, make([]byte, pad)...)
	audio = append(audio, tex...)
	footer := make([]byte, 16)
	binary.LittleEndian.PutUint32(footer, uint32(start))
	binary.LittleEndian.PutUint32(footer[4:], uint32(len(tex)))
	copy(footer[12:], "UTXT")
	return append(audio, footer...), nil
}

// sourceAncestor also handles case aliases on case-insensitive filesystems.
func sourceAncestor(out, source string) (bool, error) {
	identity, err := os.Stat(source)
	if err != nil {
		return false, err
	}
	// Do not walk above the common ancestor: WASI mounts do not grant access
	// to their virtual parent, and identity checks above it cannot locate source.
	boundary := source
	for !inside(out, boundary) && filepath.Dir(boundary) != boundary {
		boundary = filepath.Dir(boundary)
	}
	for p := out; ; p = filepath.Dir(p) {
		st, err := os.Stat(p)
		if err == nil && os.SameFile(identity, st) {
			return true, nil
		}
		if err != nil && !os.IsNotExist(err) {
			return false, err
		}
		if p == boundary || filepath.Dir(p) == p {
			break
		}
	}
	return false, nil
}

// Prepare publishes a complete converted folder; failures leave no staged output.
func Prepare(o Options) (string, error) {
	if o.Game != "undertale" && o.Game != "deltarune" {
		return "", fmt.Errorf("choose undertale or deltarune")
	}
	src, e := canonicalPath(o.Source)
	if e != nil {
		return "", e
	}
	out, e := canonicalPath(o.Output)
	if e != nil {
		return "", e
	}
	sourceFolder := src
	if st, e := os.Stat(src); e == nil && !st.IsDir() {
		sourceFolder = filepath.Dir(src)
	}
	within, e := sourceAncestor(out, sourceFolder)
	if e != nil {
		return "", e
	}
	if inside(out, sourceFolder) || within {
		return "", fmt.Errorf("choose an output folder outside the original game installation")
	}
	if hasSaves(o.Output) || hasSaves(out) {
		return "", fmt.Errorf("choose an output folder outside Saves")
	}
	if _, e := os.Lstat(out); !os.IsNotExist(e) {
		if e != nil {
			return "", e
		}
		return "", fmt.Errorf("output already exists; choose a new folder")
	}
	d, e := discover(o.Game, src)
	if e != nil {
		return "", e
	}
	report := func(s string) {
		if o.Progress != nil {
			o.Progress(s)
		}
	}
	report("Converting textures")
	tex, e := TexturePack(d.data, o.Progress)
	if e != nil {
		return "", e
	}
	report("Converting audio")
	audio, e := MusicPack(d.data, d.external, d.groups, d.raw, o.Progress)
	if e != nil {
		return "", e
	}
	audio, e = validatePacks(tex, audio, d.expected)
	if e != nil {
		return "", e
	}
	parent := filepath.Dir(out)
	if e = os.MkdirAll(parent, 0755); e != nil {
		return "", e
	}
	check, e := canonicalPath(parent)
	if e != nil {
		return "", e
	}
	if check != parent {
		return "", fmt.Errorf("output parent changed during conversion")
	}
	stage, e := os.MkdirTemp(parent, ".butterscotch-data-")
	if e != nil {
		return "", e
	}
	defer os.RemoveAll(stage)
	ready := filepath.Join(stage, "ready")
	common := filepath.Join(ready, "Assets/butterscotch/common")
	if e = os.MkdirAll(common, 0755); e != nil {
		return "", e
	}
	names := []string{"data.win", "textures.bin", "music.bin"}
	if o.Game == "deltarune" {
		names = []string{"deltarune.win", "dr_textures.bin", "dr_music.bin"}
	}
	files := map[string]string{}
	for i, b := range [][]byte{d.data, tex, audio} {
		if e = os.WriteFile(filepath.Join(common, names[i]), b, 0644); e != nil {
			return "", e
		}
		files["Assets/butterscotch/common/"+names[i]] = hash(b)
	}
	manifest := struct {
		Game     string            `json:"game"`
		Source   string            `json:"source_filename"`
		DataHash string            `json:"data_sha256"`
		Files    map[string]string `json:"files"`
	}{o.Game, filepath.Base(d.path), hash(d.data), files}
	m, e := json.MarshalIndent(manifest, "", "  ")
	if e != nil {
		return "", e
	}
	if e = os.WriteFile(filepath.Join(ready, "PREPARED-DATA.json"), append(m, '\n'), 0644); e != nil {
		return "", e
	}
	if _, e = os.Lstat(out); !os.IsNotExist(e) {
		return "", fmt.Errorf("output appeared during conversion; choose a new folder")
	}
	if e = os.Rename(ready, out); e != nil {
		return "", e
	}
	report("Preparation complete")
	return out, nil
}
