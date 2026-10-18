package plugin

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

type menu struct {
	Choice struct {
		Name    string   `json:"name"`
		Query   string   `json:"query"`
		Choices []string `json:"choices"`
	} `json:"Choice"`
}

func readMenu(t *testing.T, b []byte, err error) menu {
	t.Helper()
	if err != nil {
		t.Fatal(err)
	}
	var m menu
	if err := json.Unmarshal(b, &m); err != nil || m.Choice.Name == "" || len(m.Choice.Choices) == 0 {
		t.Fatalf("invalid menu: %s (%v)", b, err)
	}
	return m
}
func answer(name, value string) []byte {
	b, _ := json.Marshal(map[string]any{"Answer": map[string]string{"name": name, "value": value}})
	return b
}
func startMenu(t *testing.T, s *Session) menu {
	t.Helper()
	b, e := s.Start()
	return readMenu(t, b, e)
}
func selectGame(t *testing.T, s *Session, game string) menu {
	t.Helper()
	m := startMenu(t, s)
	b, e := s.Handle(answer(m.Choice.Name, game))
	return readMenu(t, b, e)
}

func TestStartSelectionAndConfirmationDoesNotWrite(t *testing.T) {
	root := t.TempDir()
	s := New(root, nil)
	m := selectGame(t, s, "Undertale 1.08")
	if !strings.Contains(m.Choice.Query, "Butterscotch-input/undertale") || !strings.Contains(m.Choice.Query, "full") {
		t.Fatal(m.Choice.Query)
	}
	entries, e := os.ReadDir(root)
	if e != nil || len(entries) != 0 {
		t.Fatal("selection wrote files", e)
	}
	b, e := s.Handle(answer(m.Choice.Name, "Back"))
	readMenu(t, b, e)
	if _, e = s.Handle(answer(m.Choice.Name, "Prepare")); e == nil {
		t.Fatal("stale confirmation accepted")
	}
}
func TestConfirmationFailureOffersRetryWithoutPublishedFiles(t *testing.T) {
	root := t.TempDir()
	var logs []string
	s := New(root, func(v string) { logs = append(logs, v) })
	m := selectGame(t, s, "Deltarune Chapter 1")
	b, e := s.Handle(answer(m.Choice.Name, "Prepare"))
	m = readMenu(t, b, e)
	if !strings.Contains(m.Choice.Query, "failed") {
		t.Fatal(m.Choice.Query)
	}
	if _, e = os.Stat(filepath.Join(root, "Butterscotch-prepared/deltarune")); !os.IsNotExist(e) {
		t.Fatal("published failure", e)
	}
	b, e = s.Handle(answer(m.Choice.Name, "Retry"))
	m = readMenu(t, b, e)
	if !strings.Contains(m.Choice.Query, "Butterscotch-input/deltarune") {
		t.Fatal(m.Choice.Query)
	}
}
func TestRejectForgedMalformedAndOutOfOrderAnswers(t *testing.T) {
	for _, input := range []string{`{"Answer":{"name":"game","value":"../../host"}}`, `{"Answer":{"name":"confirm","value":"Prepare"}}`, `{"Answer":{"name":"game","value":"Undertale 1.08","extra":true}}`, `{"Answer":{"name":"game","value":"Undertale 1.08"},"Kill":null}`, `{"Kill":null}`, `null`, `"Answer"`, `{"Answer":{"name":"game","value":"Undertale 1.08"}} {}`, `{"Answer":{"name":"game","name":"game","value":"Undertale 1.08"}}`} {
		t.Run(input, func(t *testing.T) {
			s := New(t.TempDir(), nil)
			m := startMenu(t, s)
			input = strings.ReplaceAll(input, `"game"`, `"`+m.Choice.Name+`"`)
			if _, e := s.Handle([]byte(input)); e == nil {
				t.Fatalf("accepted %s", input)
			}
		})
	}
	s := New(t.TempDir(), nil)
	if _, e := s.Handle(answer("game", "Undertale 1.08")); e == nil {
		t.Fatal("answer before start accepted")
	}
}
func TestExitAndKill(t *testing.T) {
	for _, kill := range []bool{false, true} {
		s := New(t.TempDir(), nil)
		m := startMenu(t, s)
		input := answer(m.Choice.Name, "Exit")
		if kill {
			input = []byte(`"Kill"`)
		}
		b, e := s.Handle(input)
		if e != nil || string(b) != `"Exit"` {
			t.Fatalf("%s %v", b, e)
		}
		if _, e = s.Handle(answer(m.Choice.Name, "Undertale 1.08")); e == nil {
			t.Fatal("continued after exit")
		}
	}
}

// A tiny synthetic installation exercises the real shared converter end to end.
func stagedInstallation(t *testing.T, root string) []byte {
	t.Helper()
	b := []byte("FORM\x00\x00\x00\x00")
	put := func(p int, v uint32) { binary.LittleEndian.PutUint32(b[p:], v) }
	add := func(name string, payload []byte) int {
		o := len(b)
		b = append(b, []byte(name)...)
		b = append(b, make([]byte, 4)...)
		put(o+4, uint32(len(payload)))
		b = append(b, payload...)
		return o + 8
	}
	add("GEN8", []byte{0, 16, 0, 0, 0, 0, 0, 0})
	q := []byte{'f', 'i', 'o', 'q', 1, 0, 1, 0, 5, 0, 0, 0, 255, 255, 0, 0, 255}
	p := add("TXTR", append(make([]byte, 16), q...))
	put(p, 1)
	put(p+4, uint32(p+8))
	put(p+12, uint32(p+16))
	p = add("SOND", append(make([]byte, 44), []byte("tone\x00")...))
	put(p, 1)
	put(p+4, uint32(p+8))
	put(p+8, uint32(p+44))
	put(p+12, 3)
	wav := make([]byte, 48)
	copy(wav, "RIFF")
	binary.LittleEndian.PutUint32(wav[4:], 40)
	copy(wav[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(wav[16:], 16)
	binary.LittleEndian.PutUint16(wav[20:], 1)
	binary.LittleEndian.PutUint16(wav[22:], 1)
	binary.LittleEndian.PutUint32(wav[24:], 32000)
	binary.LittleEndian.PutUint32(wav[28:], 32000)
	binary.LittleEndian.PutUint16(wav[32:], 1)
	binary.LittleEndian.PutUint16(wav[34:], 8)
	copy(wav[36:], "data")
	binary.LittleEndian.PutUint32(wav[40:], 4)
	copy(wav[44:], []byte{128, 130, 126, 128})
	p = add("AUDO", append(make([]byte, 12), wav...))
	put(p, 1)
	put(p+4, uint32(p+8))
	put(p+8, uint32(len(wav)))
	put(4, uint32(len(b)-8))
	path := filepath.Join(root, "Butterscotch-input/undertale/data.win")
	if e := os.MkdirAll(filepath.Dir(path), 0755); e != nil {
		t.Fatal(e)
	}
	if e := os.WriteFile(path, b, 0644); e != nil {
		t.Fatal(e)
	}
	return b
}
func TestConfirmedPreparationPublishesAndPreservesExistingData(t *testing.T) {
	root := t.TempDir()
	source := stagedInstallation(t, root)
	installed := filepath.Join(root, "Assets/butterscotch/common/data.win")
	save := filepath.Join(root, "Saves/butterscotch/save.dat")
	for _, p := range []string{installed, save} {
		if e := os.MkdirAll(filepath.Dir(p), 0755); e != nil {
			t.Fatal(e)
		}
		if e := os.WriteFile(p, []byte("keep"), 0644); e != nil {
			t.Fatal(e)
		}
	}
	var logs []string
	s := New(root, func(v string) { logs = append(logs, v) })
	m := selectGame(t, s, "Undertale 1.08")
	output := filepath.Join(root, "Butterscotch-prepared/undertale")
	if _, e := os.Stat(output); !os.IsNotExist(e) {
		t.Fatal("prepared before confirmation")
	}
	b, e := s.Handle(answer(m.Choice.Name, "Prepare"))
	m = readMenu(t, b, e)
	if !strings.Contains(m.Choice.Query, "Merge") || len(logs) == 0 {
		t.Fatal("missing success or progress", m.Choice.Query, logs)
	}
	prepared, e := os.ReadFile(filepath.Join(output, "Assets/butterscotch/common/data.win"))
	if e != nil || !bytes.Equal(source, prepared) {
		t.Fatal("wrong prepared data", e)
	}
	original, e := os.ReadFile(filepath.Join(root, "Butterscotch-input/undertale/data.win"))
	if e != nil || !bytes.Equal(source, original) {
		t.Fatal("source changed", e)
	}
	for _, p := range []string{installed, save} {
		v, e := os.ReadFile(p)
		if e != nil || string(v) != "keep" {
			t.Fatal("existing data changed", p, e)
		}
	}
	b, e = s.Handle(answer(m.Choice.Name, "Back"))
	m = readMenu(t, b, e)
	b, e = s.Handle(answer(m.Choice.Name, "Undertale 1.08"))
	m = readMenu(t, b, e)
	b, e = s.Handle(answer(m.Choice.Name, "Prepare"))
	m = readMenu(t, b, e)
	if !strings.Contains(m.Choice.Query, "failed") {
		t.Fatal("existing output replaced", m.Choice.Query)
	}
	prepared, e = os.ReadFile(filepath.Join(output, "Assets/butterscotch/common/data.win"))
	if e != nil || !bytes.Equal(source, prepared) {
		t.Fatal("existing output changed", e)
	}
}

func TestFixedStagingAndOutputRejectSymlinkRedirection(t *testing.T) {
	for _, part := range []string{"Butterscotch-input", "Butterscotch-prepared"} {
		t.Run(part, func(t *testing.T) {
			root := t.TempDir()
			outside := t.TempDir()
			if part == "Butterscotch-input" {
				stagedInstallation(t, outside)
				if e := os.Symlink(filepath.Join(outside, part), filepath.Join(root, part)); e != nil {
					t.Fatal(e)
				}
			} else {
				stagedInstallation(t, root)
				if e := os.Symlink(outside, filepath.Join(root, part)); e != nil {
					t.Fatal(e)
				}
			}
			s := New(root, nil)
			m := selectGame(t, s, "Undertale 1.08")
			b, e := s.Handle(answer(m.Choice.Name, "Prepare"))
			m = readMenu(t, b, e)
			if !strings.Contains(m.Choice.Query, "failed") {
				t.Fatal("accepted redirected path", m.Choice.Query)
			}
			if _, e := os.Stat(filepath.Join(outside, "undertale")); !os.IsNotExist(e) {
				t.Fatal("wrote outside SD root", e)
			}
		})
	}
}
