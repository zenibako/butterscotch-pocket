// Package plugin implements the Pocket Sync menu protocol without host dependencies.
package plugin

import (
	"encoding/json"
	"fmt"
	"io"
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
	guideOS     string
	guideStep   int
	sequence    uint64
	pending     choice
}

// New uses the SD-card root ("pocket" in Pocket Sync), never an answer-supplied path.
func New(root string, logger func(string)) *Session { return &Session{root: root, log: logger} }

// menu advances the conversation and issues a unique response name so earlier
// answers cannot be replayed against a later screen.
func (s *Session) menu(state, query string, choices ...string) ([]byte, error) {
	s.sequence++
	s.state = state
	s.pending = choice{fmt.Sprintf("%s-%d", state, s.sequence), query, choices}
	return json.Marshal(struct {
		Choice choice `json:"Choice"`
	}{s.pending})
}

// games clears the selected game while retaining the session's remembered OS.
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

// confirm shows the selected source folder and destructive-choice scope, keeping
// preservation as the default and offering guidance without accessing the card.
func (s *Session) confirm() ([]byte, error) {
	return s.menu("confirm", fmt.Sprintf("Copy full game resources to:\nAssets/butterscotch/common/%s\n\nDiscard deletes this entire source folder after a successful install.", s.game), "Install and keep originals", "Install and discard originals", "Find my game files", "Back", "Exit")
}

// exit closes the conversation and invalidates its pending menu response.
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
		if value == "Find my game files" {
			s.guideStep = 0
			if s.guideOS == "" {
				return s.guidePlatforms()
			}
			return s.guidePage()
		}
		if value == "Back" {
			return s.games()
		}
		s.state = "preparing"
		result, e := s.install(value == "Install and discard originals")
		if e != nil {
			return s.menu("failed", fmt.Sprintf("Installation failed:\n%s\n\nCheck Assets/butterscotch/common/%s, then retry.", e, s.game), "Retry", "Back", "Exit")
		}
		return s.complete(result)
	case "guide-os":
		if value == "Back" {
			return s.confirm()
		}
		s.guideOS = value
		s.guideStep = 0
		return s.guidePage()
	case "guide":
		if value == "Ready to install" {
			return s.confirm()
		}
		if value == "Back" {
			if s.guideStep == 0 {
				return s.guidePlatforms()
			}
			s.guideStep--
		} else {
			s.guideStep++
		}
		return s.guidePage()
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

// complete reports installation and cleanup outcomes independently.
func (s *Session) complete(result prep.InstallResult) ([]byte, error) {
	originals := "Originals kept."
	if result.OriginalsDiscarded {
		originals = "Originals discarded."
	}
	if result.Warning != "" {
		if result.OriginalsDiscarded {
			originals += "\nWarning: " + result.Warning
		} else {
			originals = result.Warning
		}
	}
	return s.menu("done", "Installed in Assets/butterscotch/common.\n"+originals+" Ready to play.", "Back", "Exit")
}

// install converts only the selected game under the host-provided card root and
// forwards progress to the logger; answers never supply filesystem paths.
func (s *Session) install(discard bool) (prep.InstallResult, error) {
	return prep.Install(prep.InstallOptions{Root: s.root, Game: s.game, DiscardOriginals: discard, Progress: s.log})
}
