package plugin

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/zenibako/butterscotch-pocket/prep"
)

type menu struct {
	Choice struct {
		Name    string   `json:"name"`
		Query   string   `json:"query"`
		Choices []string `json:"choices"`
	} `json:"Choice"`
}

// TestCompletionPreservesDiscardStatusWithCleanupWarning checks that cleanup warnings retain the actual installation and originals outcomes.
func TestCompletionPreservesDiscardStatusWithCleanupWarning(t *testing.T) {
	for _, tc := range []struct {
		name      string
		discarded bool
		warning   string
		status    string
	}{
		{"kept", false, "", "Originals kept."},
		{"discarded", true, "", "Originals discarded."},
		{"discarded-stage-warning", true, "Could not remove temporary files.", "Originals discarded."},
		{"partial-discard-warning", false, "Could not finish discarding originals.", ""},
	} {
		t.Run(tc.name, func(t *testing.T) {
			s := New(t.TempDir(), nil)
			b, err := s.complete(prep.InstallResult{OriginalsDiscarded: tc.discarded, Warning: tc.warning})
			m := readMenu(t, b, err)
			if !strings.Contains(m.Choice.Query, "Installed in Assets/butterscotch/common") {
				t.Fatal("installation outcome missing", m.Choice.Query)
			}
			if tc.status != "" && !strings.Contains(m.Choice.Query, tc.status) {
				t.Fatal("originals outcome missing", m.Choice.Query)
			}
			if tc.warning != "" && !strings.Contains(m.Choice.Query, tc.warning) {
				t.Fatal("cleanup warning missing", m.Choice.Query)
			}
			if tc.warning != "" && !tc.discarded && strings.Contains(m.Choice.Query, "Originals kept.") {
				t.Fatal("partial deletion reported as kept", m.Choice.Query)
			}
		})
	}
}

// readMenu requires a successful response containing a named choice with available actions.
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

// answer encodes a named menu response in the plugin protocol.
func answer(name, value string) []byte {
	b, _ := json.Marshal(map[string]any{"Answer": map[string]string{"name": name, "value": value}})
	return b
}

// startMenu starts a session and requires a valid initial choice.
func startMenu(t *testing.T, s *Session) menu {
	t.Helper()
	b, e := s.Start()
	return readMenu(t, b, e)
}

// selectGame selects a game from a fresh session and returns its confirmation menu.
func selectGame(t *testing.T, s *Session, game string) menu {
	t.Helper()
	m := startMenu(t, s)
	b, e := s.Handle(answer(m.Choice.Name, game))
	return readMenu(t, b, e)
}

// TestStartSelectionAndConfirmationDoesNotWrite checks read-only selection and invalidation of confirmations after returning to the menu.
func TestStartSelectionAndConfirmationDoesNotWrite(t *testing.T) {
	root := t.TempDir()
	s := New(root, nil)
	m := selectGame(t, s, "Undertale 1.08")
	if !strings.Contains(m.Choice.Query, "Assets/butterscotch/common/undertale") || !strings.Contains(m.Choice.Query, "full") {
		t.Fatal(m.Choice.Query)
	}
	entries, e := os.ReadDir(root)
	if e != nil || len(entries) != 0 {
		t.Fatal("selection wrote files", e)
	}
	b, e := s.Handle(answer(m.Choice.Name, "Back"))
	readMenu(t, b, e)
	if _, e = s.Handle(answer(m.Choice.Name, "Install and keep originals")); e == nil {
		t.Fatal("stale confirmation accepted")
	}
}

// TestInstallPromptsStayShortAndShowSourceFolder checks that each game confirmation uses short, separated instructions.
func TestInstallPromptsStayShortAndShowSourceFolder(t *testing.T) {
	for _, game := range []string{"Undertale 1.08", "Deltarune Chapter 1"} {
		s := New(t.TempDir(), nil)
		m := selectGame(t, s, game)
		if len(m.Choice.Query) > 180 || strings.Count(m.Choice.Query, "\n") < 2 {
			t.Fatalf("prompt needs short separated steps: %q", m.Choice.Query)
		}
	}
}

// TestConfirmationFailureOffersRetryWithoutPublishedFiles checks that failed preparation publishes no output and returns to confirmation on retry.
func TestConfirmationFailureOffersRetryWithoutPublishedFiles(t *testing.T) {
	root := t.TempDir()
	var logs []string
	s := New(root, func(v string) { logs = append(logs, v) })
	m := selectGame(t, s, "Deltarune Chapter 1")
	b, e := s.Handle(answer(m.Choice.Name, "Install and keep originals"))
	m = readMenu(t, b, e)
	if !strings.Contains(m.Choice.Query, "failed") {
		t.Fatal(m.Choice.Query)
	}
	if _, e = os.Stat(filepath.Join(root, "Butterscotch-prepared/deltarune")); !os.IsNotExist(e) {
		t.Fatal("published failure", e)
	}
	b, e = s.Handle(answer(m.Choice.Name, "Retry"))
	m = readMenu(t, b, e)
	if !strings.Contains(m.Choice.Query, "Assets/butterscotch/common/deltarune") {
		t.Fatal(m.Choice.Query)
	}
}

// TestRejectForgedMalformedAndOutOfOrderAnswers checks strict response decoding and rejection of answers outside the active session state.
func TestRejectForgedMalformedAndOutOfOrderAnswers(t *testing.T) {
	for _, input := range []string{`{"Answer":{"name":"game","value":"../../host"}}`, `{"Answer":{"name":"confirm","value":"Install and keep originals"}}`, `{"Answer":{"name":"game","value":"Undertale 1.08","extra":true}}`, `{"Answer":{"name":"game","value":"Undertale 1.08"},"Kill":null}`, `{"Kill":null}`, `null`, `"Answer"`, `{"Answer":{"name":"game","value":"Undertale 1.08"}} {}`, `{"Answer":{"name":"game","name":"game","value":"Undertale 1.08"}}`} {
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

// TestExitAndKill checks that both exit paths terminate the session and reject subsequent answers.
func TestExitAndKill(t *testing.T) {
	for _, cancel := range []string{"", `"Kill"`, `{"Kill":{}}`} {
		s := New(t.TempDir(), nil)
		m := startMenu(t, s)
		input := answer(m.Choice.Name, "Exit")
		if cancel != "" {
			input = []byte(cancel)
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
	path := filepath.Join(root, "Assets/butterscotch/common/undertale/data.win")
	if e := os.MkdirAll(filepath.Dir(path), 0755); e != nil {
		t.Fatal(e)
	}
	if e := os.WriteFile(path, b, 0644); e != nil {
		t.Fatal(e)
	}
	return b
}

// TestInstallChoicesPreserveOrDiscardOriginals checks explicit installation choices, preservation of unrelated data and progress reporting.
func TestInstallChoicesPreserveOrDiscardOriginals(t *testing.T) {
	for _, discard := range []bool{false, true} {
		t.Run(fmt.Sprint(discard), func(t *testing.T) {
			root := t.TempDir()
			source := stagedInstallation(t, root)
			installed := filepath.Join(root, "Assets/butterscotch/common/data.win")
			other := filepath.Join(root, "Assets/butterscotch/common/deltarune.win")
			save := filepath.Join(root, "Saves/butterscotch/save.dat")
			for _, p := range []string{installed, other, save} {
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
			// Merely choosing a game must not modify the installation.
			before, _ := os.ReadFile(installed)
			if string(before) != "keep" {
				t.Fatal("installed before confirmation")
			}
			if !strings.Contains(m.Choice.Query, "entire") {
				t.Fatal("discard scope not explained", m.Choice.Query)
			}
			action := "Install and keep originals"
			if discard {
				action = "Install and discard originals"
			}
			b, e := s.Handle(answer(m.Choice.Name, action))
			m = readMenu(t, b, e)
			if !strings.Contains(m.Choice.Query, "Installed") || strings.Contains(m.Choice.Query, "Merge") || len(logs) == 0 {
				t.Fatal("missing direct installation success", m.Choice.Query, logs)
			}
			actual, e := os.ReadFile(installed)
			if e != nil || !bytes.Equal(source, actual) {
				t.Fatal("wrong installed data", e)
			}
			original := filepath.Join(root, "Assets/butterscotch/common/undertale/data.win")
			if discard {
				if _, e := os.Stat(filepath.Dir(original)); !os.IsNotExist(e) {
					t.Fatal("originals were not discarded", e)
				}
			} else {
				actual, e := os.ReadFile(original)
				if e != nil || !bytes.Equal(source, actual) {
					t.Fatal("originals changed", e)
				}
			}
			for _, p := range []string{other, save} {
				actual, e := os.ReadFile(p)
				if e != nil || string(actual) != "keep" {
					t.Fatal("unrelated file changed", p, e)
				}
			}
			for _, legacy := range []string{"Butterscotch-input", "Butterscotch-prepared"} {
				if _, e := os.Stat(filepath.Join(root, legacy)); !os.IsNotExist(e) {
					t.Fatal("created legacy folder", legacy)
				}
			}
		})
	}
}

// TestDiscardFailureKeepsOriginalsAndInstalledData checks that failed conversion after choosing discard preserves both originals and prior installed data.
func TestDiscardFailureKeepsOriginalsAndInstalledData(t *testing.T) {
	root := t.TempDir()
	source := filepath.Join(root, "Assets/butterscotch/common/undertale/data.win")
	installed := filepath.Join(root, "Assets/butterscotch/common/data.win")
	if err := os.MkdirAll(filepath.Dir(source), 0755); err != nil {
		t.Fatal(err)
	}
	for _, path := range []string{source, installed} {
		if err := os.WriteFile(path, []byte("keep invalid original"), 0644); err != nil {
			t.Fatal(err)
		}
	}
	s := New(root, nil)
	m := selectGame(t, s, "Undertale 1.08")
	b, e := s.Handle(answer(m.Choice.Name, "Install and discard originals"))
	m = readMenu(t, b, e)
	if !strings.Contains(m.Choice.Query, "failed") {
		t.Fatal(m.Choice.Query)
	}
	for _, path := range []string{source, installed} {
		actual, err := os.ReadFile(path)
		if err != nil || string(actual) != "keep invalid original" {
			t.Fatal("lost data on failure", path, err)
		}
	}
}

// TestAssetsPathsRejectSymlinkRedirection checks that redirected asset paths fail without writing outside the card.
func TestAssetsPathsRejectSymlinkRedirection(t *testing.T) {
	for _, part := range []string{"Assets", "Assets/butterscotch", "Assets/butterscotch/common", "Assets/butterscotch/common/undertale"} {
		t.Run(part, func(t *testing.T) {
			root := t.TempDir()
			outside := t.TempDir()
			dest := filepath.Join(root, part)
			if err := os.MkdirAll(filepath.Dir(dest), 0755); err != nil {
				t.Fatal(err)
			}
			if err := os.Symlink(outside, dest); err != nil {
				t.Skip(err)
			}
			s := New(root, nil)
			m := selectGame(t, s, "Undertale 1.08")
			b, e := s.Handle(answer(m.Choice.Name, "Install and discard originals"))
			m = readMenu(t, b, e)
			if !strings.Contains(m.Choice.Query, "failed") {
				t.Fatal("accepted redirect", m.Choice.Query)
			}
			entries, err := os.ReadDir(outside)
			if err != nil || len(entries) != 0 {
				t.Fatal("modified outside card", err)
			}
		})
	}
}

// TestMalformedKillLeavesMenuUsable rejects invalid cancels without consuming the menu.
func TestMalformedKillLeavesMenuUsable(t *testing.T) {
	for _, input := range []string{`{"Kill":null}`, `{"Kill":[]}`, `{"Kill":{"extra":true}}`, `{"Kill":{},"extra":0}`, `{"Kill":{},"Kill":{}}`, `{"Kill":{}} null`, `"Kill" null`} {
		t.Run(input, func(t *testing.T) {
			s := New(t.TempDir(), nil)
			m := startMenu(t, s)
			if _, err := s.Handle([]byte(input)); err == nil {
				t.Fatalf("accepted %s", input)
			}
			b, err := s.Handle(answer(m.Choice.Name, "Exit"))
			if err != nil || string(b) != `"Exit"` {
				t.Fatalf("menu consumed by malformed cancel: %s %v", b, err)
			}
		})
	}
}
