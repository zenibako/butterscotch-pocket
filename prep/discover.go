package prep

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

type discovered struct {
	path     string
	data     []byte
	external map[string]string
	groups   map[int][]byte
	raw      map[string][]byte
	expected map[string]bool
}

// readInput checks the size before allocation and limits reads even if a file grows.
func readInput(path string) ([]byte, error) {
	f, e := os.Open(path)
	if e != nil {
		return nil, e
	}
	defer f.Close()
	st, e := f.Stat()
	if e != nil {
		return nil, e
	}
	if !st.Mode().IsRegular() || st.Size() > MaxInputBytes {
		return nil, fmt.Errorf("input %s must be a regular file of at most %d MiB", filepath.Base(path), MaxInputBytes>>20)
	}
	b, e := io.ReadAll(io.LimitReader(f, MaxInputBytes+1))
	if e != nil {
		return nil, e
	}
	if len(b) > MaxInputBytes {
		return nil, fmt.Errorf("input exceeds maximum size")
	}
	return b, nil
}
func isFile(p string) bool { s, e := os.Stat(p); return e == nil && s.Mode().IsRegular() }
func isDir(p string) bool  { s, e := os.Stat(p); return e == nil && s.IsDir() }

func requirements(c *Container) (map[string]bool, map[string]bool, error) {
	embedded, external := map[string]bool{}, map[string]bool{}
	s, e := c.Chunk("SOND")
	if e != nil || len(s) < 4 {
		return nil, nil, fmt.Errorf("game data has no complete SOND audio table")
	}
	n := uint64(binary.LittleEndian.Uint32(s))
	if n > uint64((len(s)-4)/4) {
		return nil, nil, fmt.Errorf("truncated SOND audio table")
	}
	for i := 0; i < int(n); i++ {
		o := uint64(binary.LittleEndian.Uint32(s[4+i*4:]))
		if o+36 > uint64(len(c.Data)) {
			return nil, nil, fmt.Errorf("invalid sound entry")
		}
		p := int(o)
		flags := binary.LittleEndian.Uint32(c.Data[p+4:])
		idx := int32(binary.LittleEndian.Uint32(c.Data[p+32:]))
		name, e := c.String(binary.LittleEndian.Uint32(c.Data[p:]))
		if e != nil {
			return nil, nil, e
		}
		filename, e := c.String(binary.LittleEndian.Uint32(c.Data[p+12:]))
		if e != nil {
			return nil, nil, e
		}
		if flags&3 != 0 && idx >= 0 {
			embedded[strings.ToLower(name)] = true
		}
		filename = strings.ToLower(strings.ReplaceAll(filename, "\\", "/"))
		if flags&3 == 0 && strings.HasSuffix(filename, ".ogg") {
			external[filepath.Base(filename)] = true
		}
	}
	return embedded, external, nil
}
func discover(game, source string) (*discovered, error) {
	st, e := os.Stat(source)
	if e != nil {
		return nil, fmt.Errorf("game source does not exist: %w", e)
	}
	folder := source
	if !st.IsDir() {
		folder = filepath.Dir(source)
	}
	if isDir(filepath.Join(folder, "Contents/Resources")) {
		folder = filepath.Join(folder, "Contents/Resources")
	}
	root := folder
	if game == "deltarune" && st.IsDir() {
		var chapters []string
		for _, system := range []string{"mac", "windows", "linux"} {
			p := filepath.Join(folder, "chapter1_"+system)
			if isDir(p) {
				chapters = append(chapters, p)
			}
		}
		if len(chapters) > 1 {
			return nil, fmt.Errorf("multiple Chapter 1 folders found; select one chapter folder")
		}
		if len(chapters) == 1 {
			folder = chapters[0]
		}
	}
	dataPath := source
	if st.IsDir() {
		var found []string
		for _, name := range []string{"data.win", "game.ios", "game.unx", "assets/data.win", "assets/game.unx"} {
			p := filepath.Join(folder, filepath.FromSlash(name))
			if isFile(p) {
				found = append(found, p)
			}
		}
		if len(found) != 1 {
			return nil, fmt.Errorf("select an installed game folder, .app, Chapter 1 folder or exact data file; exactly one data file must be identifiable")
		}
		dataPath = found[0]
		folder = filepath.Dir(dataPath)
	}
	data, e := readInput(dataPath)
	if e != nil {
		return nil, e
	}
	c, e := Parse(data)
	if e != nil {
		return nil, e
	}
	gen, e := c.Chunk("GEN8")
	want := byte(16)
	if game == "deltarune" {
		want = 17
	}
	if e != nil || len(gen) < 2 || gen[1] != want {
		return nil, fmt.Errorf("%s requires bytecode version %d; use Undertale v1.08 or supported Deltarune Chapter 1 files", game, want)
	}
	tx, e := c.Chunk("TXTR")
	if e != nil || len(tx) < 4 || binary.LittleEndian.Uint32(tx) == 0 {
		return nil, fmt.Errorf("game file has no texture pages")
	}
	expected, required, e := requirements(c)
	if e != nil {
		return nil, e
	}
	d := &discovered{dataPath, data, map[string]string{}, map[int][]byte{}, map[string][]byte{}, expected}
	if game == "deltarune" {
		bank, e := readInput(filepath.Join(folder, "audiogroup1.dat"))
		if e != nil {
			return nil, fmt.Errorf("Deltarune Chapter 1 needs audiogroup1.dat: %w", e)
		}
		bc, e := Parse(bank)
		if e != nil {
			return nil, e
		}
		a, e := bc.Chunk("AUDO")
		if e != nil || len(a) < 4 {
			return nil, fmt.Errorf("audiogroup1.dat has no complete AUDO audio chunk")
		}
		lang, e := readInput(filepath.Join(folder, "lang/lang_en.json"))
		if e != nil {
			return nil, fmt.Errorf("Deltarune Chapter 1 needs lang/lang_en.json: %w", e)
		}
		if !json.Valid(bytes.TrimPrefix(lang, []byte{0xef, 0xbb, 0xbf})) {
			return nil, fmt.Errorf("lang_en.json is not valid JSON")
		}
		d.groups[1] = bank
		d.raw["lang_en.json"] = lang
		d.expected["lang_en.json"] = true
	}
	dirs := []string{folder, filepath.Join(folder, "music"), filepath.Join(folder, "mus"), filepath.Join(root, "mus"), filepath.Join(filepath.Dir(folder), "mus")}
	seen := map[string]bool{}
	var lowerData []byte
	if game == "deltarune" {
		lowerData = bytes.ToLower(data)
	}
	for _, dir := range dirs {
		if seen[dir] {
			continue
		}
		seen[dir] = true
		if !isDir(dir) {
			continue
		}
		entries, e := os.ReadDir(dir)
		if os.IsNotExist(e) {
			continue
		}
		if e != nil {
			return nil, e
		}
		for _, entry := range entries {
			if !strings.EqualFold(filepath.Ext(entry.Name()), ".ogg") {
				continue
			}
			path := filepath.Join(dir, entry.Name())
			if !isFile(path) {
				continue
			}
			name := strings.ToLower(entry.Name())
			if game == "deltarune" && !bytes.Contains(lowerData, []byte(name)) {
				continue
			}
			if old, ok := d.external[name]; ok {
				a, e := readInput(old)
				if e != nil {
					return nil, e
				}
				b, e := readInput(path)
				if e != nil {
					return nil, e
				}
				if !bytes.Equal(a, b) {
					return nil, fmt.Errorf("different music files share name %s; select one complete installation", name)
				}
			}
			d.external[name] = path
			d.expected[strings.TrimSuffix(name, ".ogg")] = true
		}
	}
	var missing []string
	for name := range required {
		if _, ok := d.external[name]; !ok {
			missing = append(missing, name)
		}
	}
	if len(missing) > 0 {
		sort.Strings(missing)
		return nil, fmt.Errorf("missing game music: %s; select the complete installed game folder", strings.Join(missing, ", "))
	}
	return d, nil
}
