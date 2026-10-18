// Package plugin implements the Pocket Sync menu protocol without host dependencies.
package plugin

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/zenibako/butterscotch-pocket/prep"
)

type choice struct {
	Name    string   `json:"name"`
	Query   string   `json:"query"`
	Choices []string `json:"choices"`
}

// Session holds one menu conversation. Calls must be serialized by the host.
type Session struct {
	root        string
	log         func(string)
	state, game string
	sequence    uint64
	pending     choice
}

// New uses the SD-card root ("pocket" in Pocket Sync), never an answer-supplied path.
func New(root string, logger func(string)) *Session { return &Session{root: root, log: logger} }
func (s *Session) menu(state, query string, choices ...string) ([]byte, error) {
	s.sequence++
	s.state = state
	s.pending = choice{fmt.Sprintf("%s-%d", state, s.sequence), query, choices}
	return json.Marshal(struct {
		Choice choice `json:"Choice"`
	}{s.pending})
}
func (s *Session) games() ([]byte, error) {
	s.game = ""
	return s.menu("game", "Select the game installation to prepare.", "Undertale 1.08", "Deltarune Chapter 1", "Exit")
}

// Start opens a fresh conversation only once, preventing replay from resetting state.
func (s *Session) Start() ([]byte, error) {
	if s.state != "" {
		return nil, fmt.Errorf("session already started")
	}
	return s.games()
}
func (s *Session) confirm() ([]byte, error) {
	return s.menu("confirm", fmt.Sprintf("Copy the full contents of your legally owned game installation into Butterscotch-input/%s on the SD card, including all music and supporting files. Then choose Prepare. Output will be a new Butterscotch-prepared/%s folder; existing output is refused. Existing installation and saves are preserved.", s.game, s.game), "Prepare", "Back", "Exit")
}
func (s *Session) exit() ([]byte, error) {
	s.state = "exit"
	s.pending = choice{}
	return []byte(`"Exit"`), nil
}

// parseAnswer accepts exactly the host's externally tagged Answer or unit Kill.
// Reading tokens explicitly also rejects duplicate JSON keys that Unmarshal accepts.
func parseAnswer(input []byte) (name, value string, kill bool, err error) {
	d := json.NewDecoder(strings.NewReader(string(input)))
	token, e := d.Token()
	if e != nil {
		err = e
		return
	}
	if token == "Kill" {
		kill = true
	} else {
		fail := func() { err = fmt.Errorf("invalid host response") }
		if token != json.Delim('{') {
			fail()
			return
		}
		token, e = d.Token()
		if e != nil || token != "Answer" {
			fail()
			return
		}
		token, e = d.Token()
		if e != nil || token != json.Delim('{') {
			fail()
			return
		}
		seen := map[string]bool{}
		for d.More() {
			token, e = d.Token()
			if e != nil {
				fail()
				return
			}
			key, ok := token.(string)
			if !ok || seen[key] || (key != "name" && key != "value") {
				fail()
				return
			}
			seen[key] = true
			var v string
			if d.Decode(&v) != nil {
				fail()
				return
			}
			if key == "name" {
				name = v
			} else {
				value = v
			}
		}
		if len(seen) != 2 {
			fail()
			return
		}
		token, e = d.Token()
		if e != nil || token != json.Delim('}') {
			fail()
			return
		}
		token, e = d.Token()
		if e != nil || token != json.Delim('}') {
			fail()
			return
		}
	}
	if _, e = d.Token(); e != io.EOF {
		err = fmt.Errorf("trailing host response data")
	}
	return
}

// Handle validates a response before changing state or invoking the shared converter.
func (s *Session) Handle(input []byte) ([]byte, error) {
	name, value, kill, e := parseAnswer(input)
	if e != nil {
		return nil, e
	}
	if kill {
		return s.exit()
	}
	if s.state == "" || s.state == "exit" || name != s.pending.Name {
		return nil, fmt.Errorf("out-of-order host response")
	}
	allowed := false
	for _, v := range s.pending.Choices {
		if v == value {
			allowed = true
			break
		}
	}
	if !allowed {
		return nil, fmt.Errorf("invalid choice")
	}
	if value == "Exit" {
		return s.exit()
	}
	switch s.state {
	case "game":
		if value == "Undertale 1.08" {
			s.game = "undertale"
		} else {
			s.game = "deltarune"
		}
		return s.confirm()
	case "confirm":
		if value == "Back" {
			return s.games()
		}
		s.state = "preparing"
		e := s.prepare()
		if e != nil {
			return s.menu("failed", fmt.Sprintf("Preparation failed: %s. Correct the staged installation, or move an existing prepared output aside before retrying.", e), "Retry", "Back", "Exit")
		}
		return s.menu("done", fmt.Sprintf("Prepared Butterscotch-prepared/%s. Merge its Assets folder into the SD-card root to install the prepared data. Existing installation and saves were untouched by preparation.", s.game), "Back", "Exit")
	case "failed":
		if value == "Retry" {
			return s.confirm()
		}
		return s.games()
	case "done":
		return s.games()
	}
	return nil, fmt.Errorf("invalid session state")
}

// Reject redirection of the fixed SD-card locations, including their game child.
// The supplied root itself may use the host's normal mount alias.
func (s *Session) prepare() error {
	for _, base := range []string{"Butterscotch-input", "Butterscotch-prepared"} {
		for _, path := range []string{filepath.Join(s.root, base), filepath.Join(s.root, base, s.game)} {
			info, err := os.Lstat(path)
			if os.IsNotExist(err) {
				continue
			}
			if err != nil {
				return err
			}
			if info.Mode()&os.ModeSymlink != 0 {
				return fmt.Errorf("staging and prepared paths must not be symbolic links")
			}
		}
	}
	_, err := prep.Prepare(prep.Options{Game: s.game, Source: filepath.Join(s.root, "Butterscotch-input", s.game), Output: filepath.Join(s.root, "Butterscotch-prepared", s.game), Progress: s.log})
	return err
}
